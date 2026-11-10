// Drive the real Mongoose HTTP parser with controlled network read boundaries.
#include "../localsend.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static mg_event_handler_t parser;
static const char *test_path = "native/build/upload-test.bin";
static int cases;
static int use_memory = 1;

static void begin(struct mg_connection *c, size_t expected) {
    memset(c, 0, sizeof(*c));
    c->mgr = &g.mgr;
    c->recv.align = c->send.align = 65536;
    c->pfn = parser;
    c->fn = http_cb;
    c->is_accepted = 1;
    g.s.state = SESSION_RECEIVING;
    g.s.id = 1;
    strcpy(g.s.sid, "session");
    g.s.sender = dup_str("test");
    g.s.device = dup_str("test");
    g.s.nfiles = 1;
    g.s.total = (int64_t) expected;
    g.s.in_memory = use_memory;
    g.s.files = (ls_file *) calloc(1, sizeof(ls_file));
    g.s.files[0].id = dup_str("file");
    g.s.files[0].token = dup_str("token");
    g.s.files[0].name = dup_str("image.png");
    g.s.files[0].mime = dup_str("image/png");
    g.s.files[0].path = dup_str(test_path);
    g.s.files[0].size = (int64_t) expected;
}

static void feed(struct mg_connection *c, const void *buf, size_t len) {
    CHECK(mg_iobuf_add(&c->recv, c->recv.len, buf, len) == len);
    mg_call(c, MG_EV_READ, &len);
}

static void feed_split(struct mg_connection *c, const void *data, size_t len, size_t split) {
    const unsigned char *buf = (const unsigned char *) data;
    while (len) {
        size_t n = len < split ? len : split;
        feed(c, buf, n);
        buf += n;
        len -= n;
    }
}

static void headers(struct mg_connection *c, const char *framing, size_t split) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "POST /api/localsend/v2/upload?sessionId=session&fileId=file&token=token HTTP/1.1\r\n"
        "Host: localhost\r\n%s\r\n", framing);
    feed_split(c, buf, (size_t) n, split);
}

static void finish(struct mg_connection *c, int status, const unsigned char *expected, size_t len) {
    if (status) {
        struct mg_http_message hm;
        CHECK(mg_http_parse((const char *) c->send.buf, c->send.len, &hm) > 0);
        CHECK(mg_http_status(&hm) == status);
    } else {
        CHECK(c->send.len == 0);  // Incomplete uploads must not return success.
    }
    mg_call(c, MG_EV_CLOSE, NULL);
    int done = 0;
    while (g.ev_head) {
        ls_node *n = g.ev_head;
        g.ev_head = n->next;
        if (n->ev.kind == LS_EV_DONE) {
            done++;
            CHECK(n->ev.received_bytes == (int64_t) len);
            if (use_memory) {
                CHECK(n->ev.files != NULL);
                CHECK(n->ev.files[0].size == (int64_t) len);
                CHECK(len == 0 || memcmp(n->ev.files[0].data, expected, len) == 0);
                CHECK(GetFileAttributesA(test_path) == INVALID_FILE_ATTRIBUTES);
            } else {
                FILE *fp = fopen(test_path, "rb");
                CHECK(fp != NULL);
                for (size_t ofs = 0; ofs < len;) {
                    unsigned char buf[4096];
                    size_t nbytes = len - ofs < sizeof(buf) ? len - ofs : sizeof(buf);
                    CHECK(fread(buf, 1, nbytes, fp) == nbytes);
                    CHECK(memcmp(buf, expected + ofs, nbytes) == 0);
                    ofs += nbytes;
                }
                CHECK(fgetc(fp) == EOF);
                fclose(fp);
            }
        }
        ls_event_free(&n->ev);
        free(n);
    }
    g.ev_tail = NULL;
    CHECK(done == (status == 200));
    // Releasing an unclaimed DONE event also removes its temporary file.
    CHECK(GetFileAttributesA(test_path) == INVALID_FILE_ATTRIBUTES);
    session_reset(1);
    remove(test_path);
    mg_iobuf_free(&c->recv);
    mg_iobuf_free(&c->send);
    cases++;
}

static void chunked(const unsigned char *payload, size_t len, size_t split) {
    struct mg_connection c;
    begin(&c, len);
    headers(&c, "Transfer-Encoding: chunked\r\n", split);
    for (size_t ofs = 0; ofs < len;) {
        size_t n = len - ofs < 524288 ? len - ofs : 524288;
        char size[64];
        int sz = snprintf(size, sizeof(size), "%zX;test=value\r\n", n);
        feed_split(&c, size, (size_t) sz, split);
        feed_split(&c, payload + ofs, n, split);
        feed_split(&c, "\r\n", 2, split);
        ofs += n;
    }
    CHECK(c.send.len == 0);  // File size alone does not terminate chunked HTTP.
    const char *end = "0\r\nX-Test: trailer\r\n\r\n";
    feed_split(&c, end, strlen(end), split);
    finish(&c, 200, payload, len);
}

static void invalid_chunk(const char *body) {
    struct mg_connection c;
    begin(&c, 3);
    headers(&c, "Transfer-Encoding: chunked\r\n", 65536);
    feed(&c, body, strlen(body));
    finish(&c, 400, NULL, 0);
}

int main(void) {
    InitializeCriticalSection(&g.lock);
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&g.mgr);
    struct mg_connection *listener = mg_http_listen(&g.mgr, "http://127.0.0.1:0", http_cb, NULL);
    CHECK(listener != NULL);
    parser = listener->pfn;

    unsigned char small[43];
    for (size_t i = 0; i < sizeof(small); i++) small[i] = (unsigned char) (i * 79);
    for (size_t split = 1; split <= 64; split++) chunked(small, sizeof(small), split);
    size_t len = 2787238;  // Same payload size and chunk sizes as the corrupt PNG.
    unsigned char *payload = (unsigned char *) malloc(17 * 1024 * 1024);
    CHECK(payload != NULL);
    for (size_t i = 0; i < 17 * 1024 * 1024; i++) payload[i] = (unsigned char) (i * 37 + i / 251);
    memcpy(payload, "\x89PNG\r\n\x1a\n", 8);
    chunked(payload, len, 65536);
    chunked(payload, len, 65521);
    chunked(payload, 17 * 1024 * 1024, 65536);  // Larger than MG_MAX_RECV_SIZE.
    chunked(payload, 0, 1);

    struct mg_connection c;
    begin(&c, len);
    char framing[64];
    snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", len);
    headers(&c, framing, 1);
    feed_split(&c, payload, len, 65536);
    finish(&c, 200, payload, len);

    // Headers and a whole chunked body arrive in the same read.
    begin(&c, 3);
    const char *combined =
        "POST /api/localsend/v2/upload?sessionId=session&fileId=file&token=token HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n1\r\na\r\n2\r\nbc\r\n0\r\n\r\n";
    feed(&c, combined, strlen(combined));
    finish(&c, 200, (const unsigned char *) "abc", 3);

    begin(&c, 0);
    headers(&c, "Content-Length: 0\r\n", 1);
    finish(&c, 200, payload, 0);

    begin(&c, 3);
    headers(&c, "Content-Length: 2\r\n", 65536);
    finish(&c, 400, NULL, 0);

    invalid_chunk("z\r\n");
    invalid_chunk("4\r\n");  // Declared payload exceeds metadata.
    invalid_chunk("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\n");
    invalid_chunk("0\r\n\r\n");  // Ends before the expected payload.
    invalid_chunk("3\r\nabcXX");
    invalid_chunk("3\nabc\r\n0\r\n\r\n");
    invalid_chunk("3\r\nabc\r\n0\r\nbad-trailer\r\n\r\n");

    begin(&c, 3);
    headers(&c, "Transfer-Encoding: chunked\r\n", 1);
    feed(&c, "3\r\nabc\r\n", 8);  // Missing terminal chunk: cancel on close.
    finish(&c, 0, NULL, 0);
    begin(&c, 3);
    headers(&c, "Content-Length: 3\r\n", 1);
    feed(&c, "ab", 2);
    finish(&c, 0, NULL, 0);

    free(payload);
    use_memory = 0;
    chunked(small, sizeof(small), 1);
    begin(&c, 3);
    headers(&c, "Content-Length: 3\r\n", 1);
    feed(&c, "abc", 3);
    finish(&c, 200, (const unsigned char *) "abc", 3);
    begin(&c, 3);
    headers(&c, "Content-Length: 3\r\n", 1);
    feed(&c, "ab", 2);
    finish(&c, 0, NULL, 0);

    // Exercise the actual metadata-based routing at the memory boundary.
    for (int large = 0; large < 2; large++) {
        struct mg_connection *req = mg_alloc_conn(&g.mgr);
        CHECK(req != NULL);
        req->next = g.mgr.conns;
        g.mgr.conns = req;
        req->fn = http_cb;
        req->pfn = parser;
        req->is_accepted = 1;
        g.visible = 1;
        strcpy(g.staging, "native/build");
        char body[512], request[1024];
        int blen = snprintf(body, sizeof(body),
            "{\"info\":{\"alias\":\"test\"},\"files\":{\"file\":{\"id\":\"file\",\"fileName\":\"test.bin\",\"size\":%lld}}}",
            (long long) LS_MEMORY_LIMIT + large);
        int rlen = snprintf(request, sizeof(request),
            "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\nContent-Length: %d\r\n\r\n%s", blen, body);
        feed(req, request, (size_t) rlen);
        CHECK(g.s.state == SESSION_ASKING);
        answer_prepare(1);
        CHECK(g.s.state == SESSION_RECEIVING);
        CHECK(g.s.in_memory == !large);
        if (large) CHECK(GetFileAttributesA(g.s.files[0].path) != INVALID_FILE_ATTRIBUTES);
        else CHECK(g.s.files[0].path == NULL);
        char *path = dup_str(g.s.files[0].path);
        session_cancel();
        if (path) CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
        free(path);
        mg_close_conn(req);
        while (g.ev_head) {
            ls_node *n = g.ev_head;
            g.ev_head = n->next;
            ls_event_free(&n->ev);
            free(n);
        }
        g.ev_tail = NULL;
        cases++;
    }
    mg_mgr_free(&g.mgr);
    DeleteCriticalSection(&g.lock);
    printf("Passed %d upload regression cases\n", cases);

    // The native receiver advertises exactly the persisted ID after restarts,
    // even when its display name changes. It must not invent fallback IDs.
    CHECK(!ls_start("test", "Windows", NULL, 0, "native/build"));
    CHECK(!ls_start("test", "Windows", "", 0, "native/build"));
    CHECK(!ls_start("test", "Windows", "012345678901234567890123456789012", 0, "native/build"));
    const char *identity = "0123456789abcdef0123456789abcdef";
    for (int i = 0; i < 2; i++) {
        CHECK(ls_start(i ? "renamed" : "test", "Windows", identity, 0, "native/build"));
        struct mg_iobuf info = {0, 0, 0, 256};
        info_json(&info, 1);
        char *fp = mg_json_get_str(mg_str_n((char *) info.buf, info.len), "$.fingerprint");
        CHECK(fp != NULL && strcmp(fp, identity) == 0);
        free(fp);
        mg_iobuf_free(&info);
        ls_stop();
    }
    puts("Passed native identity restart checks");
    return 0;
}
