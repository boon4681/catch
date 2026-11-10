// LocalSend v2 receiver (https://github.com/localsend/protocol) on top of
// Mongoose. Mongoose runs on its own thread; the UI thread talks to it only
// through the command and event queues below, guarded by one lock.

#include "mongoose/mongoose.h"
#include "catch_native.h"

#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LS_MCAST_GROUP "224.0.0.167"
#define LS_PROTOCOL_VERSION "2.0"
#define LS_MAX_IFACES 16
#define LS_TEXT_PREVIEW_MAX 4096
#ifndef LS_MEMORY_LIMIT
#define LS_MEMORY_LIMIT (256LL * 1024 * 1024)
#endif

typedef struct ls_file {
    char *id;
    char *token;
    char *name;
    char *mime;
    char *path;
    unsigned char *data;
    int64_t size;
    int64_t received;
    int done;
    int active;
} ls_file;

enum { SESSION_IDLE, SESSION_ASKING, SESSION_RECEIVING };

typedef struct ls_session {
    int state;
    uint64_t id;
    char sid[33];
    unsigned long ask_conn;  // prepare-upload request waiting for the user
    char *sender;
    char *device;
    char dir[1024];
    ls_file *files;
    int nfiles;
    int64_t total;
    int64_t received;
    int in_memory;
    uint64_t last_progress_ms;
} ls_session;

// Lives in c->data of a connection detached from the HTTP parser.
enum { UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_DATA, UPLOAD_CHUNK_END, UPLOAD_TRAILERS };

typedef struct ls_upload {
    FILE *fp;
    int file_index;
    int64_t expected;
    int64_t got;
    int chunked;
    int chunk_state;
    int64_t chunk_left;
    size_t trailer_bytes;
} ls_upload;

typedef struct ls_node {
    struct ls_node *next;
    ls_event ev;
} ls_node;

enum { CMD_RESPOND, CMD_CANCEL, CMD_VISIBLE, CMD_ALIAS };

typedef struct ls_cmd {
    struct ls_cmd *next;
    int kind;
    uint64_t session;
    int value;
    char *text;
} ls_cmd;

static struct {
    int running;
    volatile LONG stop;
    HANDLE thread;
    CRITICAL_SECTION lock;
    ls_node *ev_head, *ev_tail;
    ls_cmd *cmd_head, *cmd_tail;

    struct mg_mgr mgr;
    SOCKET tx;
    struct mg_connection *udp;
    char alias[256];
    char model[128];
    char fingerprint[33];
    char staging[1024];
    int port;
    int visible;
    uint64_t next_session;
    ls_session s;
    char error[256];
} g;

static char *dup_str(const char *s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s);
    char *r = (char *) malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static char *dup_mg(struct mg_str s) {
    char *r = (char *) malloc(s.len + 1);
    memcpy(r, s.buf, s.len);
    r[s.len] = 0;
    return r;
}

static void set_error(const char *msg) {
    snprintf(g.error, sizeof(g.error), "%s", msg);
}

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *) malloc(sizeof(wchar_t) * (size_t) (n > 0 ? n : 1));
    if (n <= 0) {
        w[0] = 0;
        return w;
    }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void push_event(const ls_event *ev) {
    ls_node *n = (ls_node *) calloc(1, sizeof(*n));
    n->ev = *ev;
    EnterCriticalSection(&g.lock);
    if (g.ev_tail) g.ev_tail->next = n;
    else g.ev_head = n;
    g.ev_tail = n;
    LeaveCriticalSection(&g.lock);
}

static void push_cmd(int kind, uint64_t session, int value, const char *text) {
    if (!g.running) return;
    ls_cmd *c = (ls_cmd *) calloc(1, sizeof(*c));
    c->kind = kind, c->session = session, c->value = value, c->text = dup_str(text);
    EnterCriticalSection(&g.lock);
    if (g.cmd_tail) g.cmd_tail->next = c;
    else g.cmd_head = c;
    g.cmd_tail = c;
    LeaveCriticalSection(&g.lock);
}

static void emit_simple(int kind) {
    ls_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.session = g.s.id;
    ev.sender = dup_str(g.s.sender);
    ev.device = dup_str(g.s.device);
    ev.file_count = g.s.nfiles;
    ev.total_bytes = g.s.total;
    ev.received_bytes = g.s.received;
    if (g.s.nfiles > 0) {
        ev.title = dup_str(g.s.files[0].name);
        ev.mime = dup_str(g.s.files[0].mime);
    }
    push_event(&ev);
}

// ---------------------------------------------------------------- session

static void session_reset(int remove_files) {
    for (int i = 0; i < g.s.nfiles; i++) {
        ls_file *f = &g.s.files[i];
        if (remove_files && f->path) {
            wchar_t *w = widen(f->path);
            DeleteFileW(w);
            free(w);
        }
        free(f->id), free(f->token), free(f->name), free(f->mime), free(f->path), free(f->data);
    }
    if (remove_files && g.s.dir[0]) {
        wchar_t *w = widen(g.s.dir);
        RemoveDirectoryW(w);
        free(w);
    }
    free(g.s.files);
    free(g.s.sender);
    free(g.s.device);
    memset(&g.s, 0, sizeof(g.s));
}

static struct mg_connection *find_conn(unsigned long id) {
    for (struct mg_connection *c = g.mgr.conns; c != NULL; c = c->next) {
        if (c->id == id) return c;
    }
    return NULL;
}

static void close_uploads(void) {
    for (struct mg_connection *c = g.mgr.conns; c != NULL; c = c->next) {
        ls_upload *u = *(ls_upload **) c->data;
        if (c->is_accepted && c->pfn == NULL && u != NULL) {
            if (u->fp) fclose(u->fp);
            free(u);
            *(ls_upload **) c->data = NULL;
            c->is_closing = 1;
        }
    }
}

static void session_cancel(void) {
    if (g.s.state == SESSION_IDLE) return;
    if (g.s.state == SESSION_ASKING) {
        struct mg_connection *c = find_conn(g.s.ask_conn);
        if (c) mg_http_reply(c, 403, "", "");
    }
    close_uploads();
    emit_simple(LS_EV_CANCELED);
    session_reset(1);
}

// A safe single path segment: LocalSend sends folder entries as "a/b.txt".
static void sanitize_name(char *s) {
    for (char *p = s; *p; p++) {
        unsigned char ch = (unsigned char) *p;
        if (ch < 32 || strchr("<>:\"/\\|?*", ch)) *p = '_';
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '.' || s[n - 1] == ' ')) s[--n] = 0;
    if (n == 0) strcpy(s, "file");
}

static char *unique_path(const char *dir, const char *name) {
    char base[512], ext[128] = "";
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = 0;
    }
    char buf[2048];
    for (int i = 0; i < 10000; i++) {
        if (i == 0) snprintf(buf, sizeof(buf), "%s\\%s%s", dir, base, ext);
        else snprintf(buf, sizeof(buf), "%s\\%s (%d)%s", dir, base, i, ext);
        wchar_t *w = widen(buf);
        DWORD attr = GetFileAttributesW(w);
        free(w);
        if (attr == INVALID_FILE_ATTRIBUTES) return dup_str(buf);
    }
    return dup_str(buf);
}

static int all_files_done(void) {
    for (int i = 0; i < g.s.nfiles; i++) {
        if (!g.s.files[i].done) return 0;
    }
    return 1;
}

static void session_finish(void) {
    size_t len = 1;
    for (int i = 0; i < g.s.nfiles; i++) if (g.s.files[i].path) len += strlen(g.s.files[i].path) + 1;
    char *paths = (char *) calloc(1, len);
    for (int i = 0; i < g.s.nfiles; i++) {
        if (g.s.files[i].path) {
            if (i) strcat(paths, "\n");
            strcat(paths, g.s.files[i].path);
        }
    }
    ls_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = LS_EV_DONE;
    ev.session = g.s.id;
    ev.sender = dup_str(g.s.sender);
    ev.device = dup_str(g.s.device);
    ev.title = dup_str(g.s.files[0].name);
    ev.mime = dup_str(g.s.files[0].mime);
    ev.dir = dup_str(g.s.dir);
    ev.paths = paths;
    ev.file_count = g.s.nfiles;
    ev.total_bytes = g.s.total;
    ev.received_bytes = g.s.received;
    if (g.s.in_memory) {
        ev.files = (ls_received_file *) calloc((size_t) g.s.nfiles, sizeof(*ev.files));
        if (!ev.files) {
            free(paths), free(ev.sender), free(ev.device), free(ev.title), free(ev.mime), free(ev.dir);
            session_cancel();
            return;
        }
        for (int i = 0; i < g.s.nfiles; i++) {
            ev.files[i].name = g.s.files[i].name;
            ev.files[i].data = g.s.files[i].data;
            ev.files[i].size = g.s.files[i].size;
            g.s.files[i].name = NULL;
            g.s.files[i].data = NULL;
        }
    }
    push_event(&ev);
    session_reset(0);
}

// ---------------------------------------------------------------- discovery

static void info_json(struct mg_iobuf *io, int announce) {
    mg_xprintf(mg_pfn_iobuf, io,
               "{%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%d,%m:%m,%m:false",
               MG_ESC("alias"), MG_ESC(g.alias),
               MG_ESC("version"), MG_ESC(LS_PROTOCOL_VERSION),
               MG_ESC("deviceModel"), MG_ESC(g.model),
               MG_ESC("deviceType"), MG_ESC("desktop"),
               MG_ESC("fingerprint"), MG_ESC(g.fingerprint),
               MG_ESC("port"), g.port,
               MG_ESC("protocol"), MG_ESC("http"),
               MG_ESC("download"));
    if (announce >= 0) {
        mg_xprintf(mg_pfn_iobuf, io, ",%m:%s", MG_ESC("announce"), announce ? "true" : "false");
    }
    mg_xprintf(mg_pfn_iobuf, io, "}");
}

static int local_ipv4s(ULONG *out, int max) {
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = (IP_ADAPTER_ADDRESSES *) malloc(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
    }
    int n = 0;
    if (rc == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = list; a && n < max; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                out[n++] = ((struct sockaddr_in *) u->Address.lpSockaddr)->sin_addr.s_addr;
            }
        }
    }
    free(list);
    return n;
}

// Wi-Fi is rarely the only adapter (Hyper-V, WSL, VPNs), so the group is
// joined and announced on every interface instead of the default route only.
static void join_group(struct mg_connection *c) {
    ULONG ips[LS_MAX_IFACES];
    int n = local_ipv4s(ips, LS_MAX_IFACES);
    SOCKET fd = (SOCKET) (size_t) c->fd;
    for (int i = 0; i < n; i++) {
        struct ip_mreq m;
        m.imr_multiaddr.s_addr = inet_addr(LS_MCAST_GROUP);
        m.imr_interface.s_addr = ips[i];
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *) &m, sizeof(m));
    }
    if (n == 0) {
        struct ip_mreq m;
        m.imr_multiaddr.s_addr = inet_addr(LS_MCAST_GROUP);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *) &m, sizeof(m));
    }
}

static void send_datagram(ULONG ip, const char *buf, size_t len) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((u_short) g.port);
    to.sin_addr.s_addr = ip;
    sendto(g.tx, buf, (int) len, 0, (struct sockaddr *) &to, sizeof(to));
}

static void multicast(int announce) {
    if (g.tx == INVALID_SOCKET) return;
    struct mg_iobuf io = {0, 0, 0, 256};
    info_json(&io, announce);
    ULONG ips[LS_MAX_IFACES];
    int n = local_ipv4s(ips, LS_MAX_IFACES);
    ULONG group = inet_addr(LS_MCAST_GROUP);
    for (int i = 0; i < n; i++) {
        struct in_addr itf;
        itf.s_addr = ips[i];
        setsockopt(g.tx, IPPROTO_IP, IP_MULTICAST_IF, (char *) &itf, sizeof(itf));
        send_datagram(group, (char *) io.buf, io.len);
    }
    if (n == 0) send_datagram(group, (char *) io.buf, io.len);
    mg_iobuf_free(&io);
}

static void udp_cb(struct mg_connection *c, int ev, void *ev_data) {
    (void) ev_data;
    if (ev != MG_EV_READ) return;
    struct mg_str msg = mg_str_n((char *) c->recv.buf, c->recv.len);
    char *fp = mg_json_get_str(msg, "$.fingerprint");
    bool announce = false;
    mg_json_get_bool(msg, "$.announce", &announce);
    if (g.visible && announce && fp && strcmp(fp, g.fingerprint) != 0) {
        // Answer by unicast so the reply leaves on the sender's network;
        // the multicast copy covers senders that only listen to the group.
        struct mg_iobuf io = {0, 0, 0, 256};
        info_json(&io, 0);
        if (!c->rem.is_ip6) send_datagram(c->rem.addr.ip4, (char *) io.buf, io.len);
        mg_iobuf_free(&io);
        multicast(0);
    }
    free(fp);
    c->recv.len = 0;
}

// ---------------------------------------------------------------- HTTP

static void reply_json_info(struct mg_connection *c) {
    struct mg_iobuf io = {0, 0, 0, 256};
    info_json(&io, -1);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%.*s", (int) io.len, (char *) io.buf);
    mg_iobuf_free(&io);
}

static int is_text_message(struct mg_str files) {
    struct mg_str key, val;
    size_t ofs = 0;
    int count = 0, text = 0;
    while ((ofs = mg_json_next(files, ofs, &key, &val)) > 0) {
        count++;
        char *type = mg_json_get_str(val, "$.fileType");
        struct mg_str preview = mg_json_get_tok(val, "$.preview");
        if (type && strncmp(type, "text/", 5) == 0 && preview.len > 0) text = 1;
        free(type);
    }
    return count == 1 && text;
}

static void handle_prepare(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g.visible) {
        mg_http_reply(c, 403, "", "");
        return;
    }
    if (g.s.state != SESSION_IDLE) {
        mg_http_reply(c, 409, "", "");
        return;
    }
    struct mg_str files = mg_json_get_tok(hm->body, "$.files");
    if (files.len == 0) {
        mg_http_reply(c, 400, "", "");
        return;
    }
    char *sender = mg_json_get_str(hm->body, "$.info.alias");
    char *device = mg_json_get_str(hm->body, "$.info.deviceModel");

    if (is_text_message(files)) {
        struct mg_str key, val;
        mg_json_next(files, 0, &key, &val);
        char *text = mg_json_get_str(val, "$.preview");
        if (text && strlen(text) > LS_TEXT_PREVIEW_MAX) text[LS_TEXT_PREVIEW_MAX] = 0;
        ls_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = LS_EV_TEXT;
        ev.session = ++g.next_session;
        ev.sender = dup_str(sender ? sender : "Unknown device");
        ev.device = dup_str(device ? device : "");
        ev.title = text ? text : dup_str("");
        ev.mime = dup_str("text/plain");
        ev.file_count = 1;
        push_event(&ev);
        mg_http_reply(c, 204, "", "");
        free(sender), free(device);
        return;
    }

    struct mg_str key, val;
    size_t ofs = 0;
    int cap = 8;
    g.s.files = (ls_file *) calloc((size_t) cap, sizeof(ls_file));
    while ((ofs = mg_json_next(files, ofs, &key, &val)) > 0) {
        if (g.s.nfiles == cap) {
            cap *= 2;
            g.s.files = (ls_file *) realloc(g.s.files, sizeof(ls_file) * (size_t) cap);
            memset(g.s.files + g.s.nfiles, 0, sizeof(ls_file) * (size_t) (cap - g.s.nfiles));
        }
        ls_file *f = &g.s.files[g.s.nfiles++];
        f->id = mg_json_get_str(val, "$.id");
        if (f->id == NULL) {
            if (key.len >= 2 && key.buf[0] == '"') key.buf++, key.len -= 2;
            f->id = dup_mg(key);
        }
        f->name = mg_json_get_str(val, "$.fileName");
        if (f->name == NULL) f->name = dup_str("file");
        f->mime = mg_json_get_str(val, "$.fileType");
        if (f->mime == NULL) f->mime = dup_str("application/octet-stream");
        double size = 0;
        mg_json_get_num(val, "$.size", &size);
        if (!(size >= 0 && size < (double) INT64_MAX) || (double) (int64_t) size != size ||
            (int64_t) size > INT64_MAX - g.s.total) {
            session_reset(0);
            free(sender), free(device);
            mg_http_reply(c, 400, "", "");
            return;
        }
        f->size = (int64_t) size;
        g.s.total += f->size;
        char tok[33];
        mg_random_str(tok, sizeof(tok));
        f->token = dup_str(tok);
    }
    if (g.s.nfiles == 0) {
        session_reset(0);
        mg_http_reply(c, 400, "", "");
        free(sender), free(device);
        return;
    }
    g.s.id = ++g.next_session;
    mg_random_str(g.s.sid, sizeof(g.s.sid));
    g.s.sender = dup_str(sender ? sender : "Unknown device");
    g.s.device = dup_str(device ? device : "");
    g.s.state = SESSION_ASKING;
    g.s.ask_conn = c->id;
    free(sender), free(device);
    emit_simple(LS_EV_REQUEST);
}

static void answer_prepare(int accept) {
    struct mg_connection *c = find_conn(g.s.ask_conn);
    if (c == NULL) {
        emit_simple(LS_EV_CANCELED);
        session_reset(0);
        return;
    }
    if (!accept) {
        mg_http_reply(c, 403, "", "");
        session_reset(0);
        return;
    }
    g.s.in_memory = g.s.total <= LS_MEMORY_LIMIT;
    if (!g.s.in_memory) {
        snprintf(g.s.dir, sizeof(g.s.dir), "%s\\%s", g.staging, g.s.sid);
        wchar_t *w = widen(g.s.dir);
        int ready = CreateDirectoryW(w, NULL) != 0;
        free(w);
        for (int i = 0; ready && i < g.s.nfiles; i++) {
            char name[512];
            snprintf(name, sizeof(name), "%s", g.s.files[i].name);
            sanitize_name(name);
            g.s.files[i].path = unique_path(g.s.dir, name);
            // Reserve the name so a later file of the same name gets " (1)".
            wchar_t *wp = widen(g.s.files[i].path);
            HANDLE h = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
            ready = h != INVALID_HANDLE_VALUE;
            if (ready) CloseHandle(h);
            free(wp);
        }
        if (!ready) {
            mg_http_reply(c, 500, "", "");
            emit_simple(LS_EV_CANCELED);
            session_reset(1);
            return;
        }
    }
    struct mg_iobuf io = {0, 0, 0, 512};
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%m,%m:{", MG_ESC("sessionId"), MG_ESC(g.s.sid), MG_ESC("files"));
    for (int i = 0; i < g.s.nfiles; i++) {
        mg_xprintf(mg_pfn_iobuf, &io, "%s%m:%m", i ? "," : "", MG_ESC(g.s.files[i].id), MG_ESC(g.s.files[i].token));
    }
    mg_xprintf(mg_pfn_iobuf, &io, "}}");
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%.*s", (int) io.len, (char *) io.buf);
    mg_iobuf_free(&io);
    g.s.state = SESSION_RECEIVING;
    g.s.ask_conn = 0;
    if (g.s.total == 0) {
        for (int i = 0; i < g.s.nfiles; i++) g.s.files[i].done = 1;
        session_finish();
    }
}

static int find_file(struct mg_str query) {
    char sid[64], fid[256], tok[64];
    if (mg_http_get_var(&query, "sessionId", sid, sizeof(sid)) <= 0) return -1;
    if (mg_http_get_var(&query, "fileId", fid, sizeof(fid)) <= 0) return -1;
    if (mg_http_get_var(&query, "token", tok, sizeof(tok)) <= 0) return -1;
    if (g.s.state != SESSION_RECEIVING || strcmp(sid, g.s.sid) != 0) return -1;
    for (int i = 0; i < g.s.nfiles; i++) {
        if (strcmp(g.s.files[i].id, fid) == 0 && strcmp(g.s.files[i].token, tok) == 0) return i;
    }
    return -1;
}

static void report_progress(int force) {
    uint64_t now = mg_millis();
    if (!force && now - g.s.last_progress_ms < 100) return;
    g.s.last_progress_ms = now;
    emit_simple(LS_EV_PROGRESS);
}

static void upload_fail(struct mg_connection *c, int status) {
    ls_upload *u = *(ls_upload **) c->data;
    if (u) {
        if (u->fp) fclose(u->fp);
        free(u);
        *(ls_upload **) c->data = NULL;
    }
    c->recv.len = 0;
    mg_http_reply(c, status, "", "");
    c->is_draining = 1;
    session_cancel();
}

static int upload_write(struct mg_connection *c, const unsigned char *buf, size_t n) {
    ls_upload *u = *(ls_upload **) c->data;
    if (g.s.in_memory) {
        if (n > 0) memcpy(g.s.files[u->file_index].data + u->got, buf, n);
    } else if (n > 0 && fwrite(buf, 1, n, u->fp) != n) {
        upload_fail(c, 500);
        return 0;
    }
    u->got += (int64_t) n;
    g.s.files[u->file_index].received = u->got;
    g.s.received += (int64_t) n;
    return 1;
}

static void upload_finish(struct mg_connection *c) {
    ls_upload *u = *(ls_upload **) c->data;
    int rc = u->fp ? fclose(u->fp) : 0;
    u->fp = NULL;
    if (rc != 0) {
        upload_fail(c, 500);
        return;
    }
    g.s.files[u->file_index].done = 1;
    g.s.files[u->file_index].active = 0;
    free(u);
    *(ls_upload **) c->data = NULL;
    c->recv.len = 0;
    mg_http_reply(c, 200, "", "");
    c->is_draining = 1;
    report_progress(1);
    if (all_files_done()) session_finish();
}

static void upload_feed(struct mg_connection *c) {
    ls_upload *u = *(ls_upload **) c->data;
    if (u == NULL) {
        c->recv.len = 0;
        return;
    }
    if (!u->chunked) {
        size_t n = c->recv.len;
        if ((int64_t) n > u->expected - u->got) n = (size_t) (u->expected - u->got);
        if (!upload_write(c, c->recv.buf, n)) return;
        c->recv.len = 0;
        if (u->got == u->expected) upload_finish(c);
        else report_progress(0);
        return;
    }

    // The HTTP parser is detached for streaming. Decode chunk framing here,
    // retaining partial size lines and CRLFs until the next network read.
    size_t ofs = 0;
    while (ofs < c->recv.len) {
        unsigned char *buf = c->recv.buf + ofs;
        size_t len = c->recv.len - ofs;
        if (u->chunk_state == UPLOAD_CHUNK_DATA) {
            size_t n = len;
            if ((int64_t) n > u->chunk_left) n = (size_t) u->chunk_left;
            if (!upload_write(c, buf, n)) return;
            ofs += n;
            u->chunk_left -= (int64_t) n;
            if (u->chunk_left == 0) u->chunk_state = UPLOAD_CHUNK_END;
        } else if (u->chunk_state == UPLOAD_CHUNK_END) {
            if (buf[0] != '\r' || (len >= 2 && buf[1] != '\n')) {
                upload_fail(c, 400);
                return;
            }
            if (len < 2) break;
            ofs += 2;
            u->chunk_state = UPLOAD_CHUNK_SIZE;
        } else {
            unsigned char *lf = (unsigned char *) memchr(buf, '\n', len);
            size_t line = lf ? (size_t) (lf - buf) + 1 : len;
            if (line > 8192 || (lf && (line < 2 || buf[line - 2] != '\r'))) {
                upload_fail(c, 400);
                return;
            }
            if (!lf) break;
            ofs += line;
            if (u->chunk_state == UPLOAD_TRAILERS) {
                u->trailer_bytes += line;
                if (u->trailer_bytes > 8192 || (line > 2 && !memchr(buf, ':', line - 2))) {
                    upload_fail(c, 400);
                    return;
                }
                if (line == 2) {
                    if (u->got != u->expected) upload_fail(c, 400);
                    else upload_finish(c);
                    return;
                }
            } else {
                int64_t size = 0, limit = u->expected - u->got;
                size_t i = 0;
                for (; i < line - 2 && buf[i] != ';'; i++) {
                    int ch = buf[i], digit = -1;
                    if (ch >= '0' && ch <= '9') digit = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
                    if (digit < 0 || digit > limit || size > (limit - digit) / 16) {
                        upload_fail(c, 400);
                        return;
                    }
                    size = size * 16 + digit;
                }
                if (i == 0) {
                    upload_fail(c, 400);
                    return;
                }
                u->chunk_left = size;
                u->chunk_state = size ? UPLOAD_CHUNK_DATA : UPLOAD_TRAILERS;
            }
        }
    }
    mg_iobuf_del(&c->recv, 0, ofs);
    report_progress(0);
}

static void start_upload(struct mg_connection *c, struct mg_http_message *hm) {
    int index = find_file(hm->query);
    if (index < 0) {
        mg_http_reply(c, 403, "", "");
        c->is_draining = 1;
        return;
    }
    if (g.s.files[index].done || g.s.files[index].active) {
        mg_http_reply(c, 409, "", "");
        c->is_draining = 1;
        return;
    }
    struct mg_str *cl = mg_http_get_header(hm, "Content-Length");
    struct mg_str *te = mg_http_get_header(hm, "Transfer-Encoding");
    int64_t expected = g.s.files[index].size;
    int chunked = te && mg_strcasecmp(*te, mg_str("chunked")) == 0;
    if (expected < 0 || (te && (!chunked || cl)) ||
        (!te && (!cl || hm->body.len != (uint64_t) expected))) {
        mg_http_reply(c, 400, "", "");
        c->is_draining = 1;
        return;
    }
    FILE *fp = NULL;
    if (g.s.in_memory) {
        if (expected > 0) g.s.files[index].data = (unsigned char *) malloc((size_t) expected);
    } else {
        wchar_t *w = widen(g.s.files[index].path);
        fp = _wfopen(w, L"wb");
        free(w);
    }
    if ((g.s.in_memory && expected > 0 && !g.s.files[index].data) || (!g.s.in_memory && fp == NULL)) {
        upload_fail(c, 500);
        return;
    }
    g.s.files[index].active = 1;
    ls_upload *u = (ls_upload *) calloc(1, sizeof(*u));
    if (!u) {
        if (fp) fclose(fp);
        upload_fail(c, 500);
        return;
    }
    u->fp = fp, u->file_index = index, u->expected = expected;
    u->chunked = chunked;
    *(ls_upload **) c->data = u;
    // Dropping the head detaches Mongoose's HTTP parser (see http_cb), so the
    // body streams into the holding buffer or temporary file. For a
    // chunked request upload_feed also takes over transfer decoding.
    mg_iobuf_del(&c->recv, 0, hm->head.len);
    upload_feed(c);
}

static void http_cb(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_HDRS) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (mg_match(hm->uri, mg_str("/api/localsend/v2/upload"), NULL)) start_upload(c, hm);
    } else if (ev == MG_EV_READ && c->pfn == NULL) {
        upload_feed(c);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (mg_match(hm->uri, mg_str("/api/localsend/v2/upload"), NULL)) {
            // Already answered when its headers arrived.
        } else if (mg_match(hm->uri, mg_str("/api/localsend/v2/register"), NULL) ||
            mg_match(hm->uri, mg_str("/api/localsend/v2/info"), NULL) ||
            mg_match(hm->uri, mg_str("/api/localsend/v1/info"), NULL)) {
            if (g.visible) reply_json_info(c);
            else mg_http_reply(c, 403, "", "");
        } else if (mg_match(hm->uri, mg_str("/api/localsend/v2/prepare-upload"), NULL)) {
            handle_prepare(c, hm);
        } else if (mg_match(hm->uri, mg_str("/api/localsend/v2/cancel"), NULL)) {
            char sid[64];
            if (mg_http_get_var(&hm->query, "sessionId", sid, sizeof(sid)) > 0 && strcmp(sid, g.s.sid) == 0) {
                session_cancel();
            }
            mg_http_reply(c, 200, "", "");
        } else {
            mg_http_reply(c, 404, "", "");
        }
    } else if (ev == MG_EV_CLOSE) {
        ls_upload *u = *(ls_upload **) c->data;
        if (u != NULL) {
            if (u->fp) fclose(u->fp);
            free(u);
            *(ls_upload **) c->data = NULL;
            session_cancel();
        } else if (g.s.state == SESSION_ASKING && g.s.ask_conn == c->id) {
            emit_simple(LS_EV_CANCELED);
            session_reset(0);
        }
    }
}

// ---------------------------------------------------------------- thread

static void run_commands(void) {
    EnterCriticalSection(&g.lock);
    ls_cmd *list = g.cmd_head;
    g.cmd_head = g.cmd_tail = NULL;
    LeaveCriticalSection(&g.lock);
    while (list) {
        ls_cmd *next = list->next;
        switch (list->kind) {
            case CMD_RESPOND:
                if (g.s.state == SESSION_ASKING && g.s.id == list->session) answer_prepare(list->value);
                break;
            case CMD_CANCEL:
                if (g.s.state != SESSION_IDLE && g.s.id == list->session) session_cancel();
                break;
            case CMD_VISIBLE:
                g.visible = list->value;
                if (!g.visible && g.s.state == SESSION_ASKING) session_cancel();
                if (g.visible) multicast(1);
                break;
            case CMD_ALIAS:
                snprintf(g.alias, sizeof(g.alias), "%s", list->text ? list->text : "");
                if (g.visible) multicast(1);
                break;
        }
        free(list->text);
        free(list);
        list = next;
    }
}

static DWORD WINAPI thread_main(LPVOID arg) {
    (void) arg;
    uint64_t last_announce = 0;
    while (!g.stop) {
        mg_mgr_poll(&g.mgr, 20);
        run_commands();
        uint64_t now = mg_millis();
        // Re-announce now and then: phones that opened their send sheet
        // before Catch started never see the first announcement.
        if (g.visible && now - last_announce > 15000) {
            multicast(1);
            last_announce = now;
        }
    }
    session_cancel();
    mg_mgr_free(&g.mgr);
    return 0;
}

int ls_start(const char *alias, const char *device_model, const char *fingerprint, int port, const char *staging_root) {
    if (g.running) return 1;
    memset(&g, 0, sizeof(g));
    g.tx = INVALID_SOCKET;
    if (fingerprint == NULL || fingerprint[0] == 0 || strlen(fingerprint) >= sizeof(g.fingerprint)) {
        set_error("Invalid device identity in Catch's settings.");
        return 0;
    }
    snprintf(g.fingerprint, sizeof(g.fingerprint), "%s", fingerprint);
    InitializeCriticalSection(&g.lock);
    mg_log_set(MG_LL_ERROR);
    snprintf(g.alias, sizeof(g.alias), "%s", alias);
    snprintf(g.model, sizeof(g.model), "%s", device_model);
    snprintf(g.staging, sizeof(g.staging), "%s", staging_root);
    g.port = port;
    mg_mgr_init(&g.mgr);
    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);
    if (mg_http_listen(&g.mgr, url, http_cb, NULL) == NULL) {
        set_error("Port is in use. Close LocalSend or other apps using it.");
        mg_mgr_free(&g.mgr);
        DeleteCriticalSection(&g.lock);
        return 0;
    }
    snprintf(url, sizeof(url), "udp://0.0.0.0:%d", port);
    g.udp = mg_listen(&g.mgr, url, udp_cb, NULL);
    if (g.udp == NULL) {
        set_error("Discovery port is in use. Close LocalSend or other apps using it.");
        mg_mgr_free(&g.mgr);
        DeleteCriticalSection(&g.lock);
        return 0;
    }
    join_group(g.udp);
    g.tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int ttl = 1;
    setsockopt(g.tx, IPPROTO_IP, IP_MULTICAST_TTL, (char *) &ttl, sizeof(ttl));

    g.running = 1;
    g.thread = CreateThread(NULL, 0, thread_main, NULL, 0, NULL);
    return 1;
}

void ls_stop(void) {
    if (!g.running) return;
    InterlockedExchange(&g.stop, 1);
    WaitForSingleObject(g.thread, INFINITE);
    CloseHandle(g.thread);
    if (g.tx != INVALID_SOCKET) closesocket(g.tx);
    EnterCriticalSection(&g.lock);
    while (g.ev_head) {
        ls_node *n = g.ev_head;
        g.ev_head = n->next;
        ls_event_free(&n->ev);
        free(n);
    }
    while (g.cmd_head) {
        ls_cmd *c = g.cmd_head;
        g.cmd_head = c->next;
        free(c->text);
        free(c);
    }
    LeaveCriticalSection(&g.lock);
    DeleteCriticalSection(&g.lock);
    g.running = 0;
}

void ls_set_alias(const char *alias) { push_cmd(CMD_ALIAS, 0, 0, alias); }
void ls_set_visible(int visible) { push_cmd(CMD_VISIBLE, 0, visible, NULL); }
void ls_respond(uint64_t session, int accept) { push_cmd(CMD_RESPOND, session, accept, NULL); }
void ls_cancel(uint64_t session) { push_cmd(CMD_CANCEL, session, 0, NULL); }
const char *ls_last_error(void) { return g.error; }

int ls_poll(ls_event *out) {
    if (!g.running) return 0;
    EnterCriticalSection(&g.lock);
    ls_node *n = g.ev_head;
    if (n) {
        g.ev_head = n->next;
        if (g.ev_head == NULL) g.ev_tail = NULL;
    }
    LeaveCriticalSection(&g.lock);
    if (n == NULL) return 0;
    *out = n->ev;
    free(n);
    return 1;
}

void ls_event_free(ls_event *ev) {
    // A completed event owns its temporary files until the backend takes them.
    if (ev->kind == LS_EV_DONE) {
        if (ev->paths) {
            char *p = ev->paths;
            while (*p) {
                char *end = strchr(p, '\n');
                if (end) *end = 0;
                wchar_t *w = widen(p);
                DeleteFileW(w);
                free(w);
                if (!end) break;
                p = end + 1;
            }
        }
        if (ev->dir && ev->dir[0]) {
            wchar_t *w = widen(ev->dir);
            RemoveDirectoryW(w);
            free(w);
        }
    }
    ls_received_free(ev->files, ev->file_count);
    free(ev->sender), free(ev->device), free(ev->title), free(ev->mime), free(ev->dir), free(ev->paths);
    memset(ev, 0, sizeof(*ev));
}

void ls_received_free(ls_received_file *files, int count) {
    if (!files) return;
    for (int i = 0; i < count; i++) free(files[i].name), free(files[i].data);
    free(files);
}
