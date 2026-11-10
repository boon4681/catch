#ifndef CATCH_NATIVE_H
#define CATCH_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LS_EV_NONE = 0,
    LS_EV_REQUEST,   // a sender asks to deliver files; answer with ls_respond
    LS_EV_PROGRESS,  // bytes of the accepted session arrived
    LS_EV_DONE,      // files are held in memory or in temporary `dir`
    LS_EV_CANCELED,  // sender canceled, connection dropped, or an upload failed
    LS_EV_TEXT,      // a text/link message; nothing to download
};

typedef struct ls_received_file {
    char *name;
    unsigned char *data;
    int64_t size;
} ls_received_file;

// Strings and files are owned by the event, released by ls_event_free.
typedef struct ls_event {
    int kind;
    uint64_t session;
    char *sender;
    char *device;
    char *title;  // first file name, or the text of a message
    char *mime;   // type of the first file
    char *dir;    // staging folder of the session (DONE)
    char *paths;  // absolute paths, one per line (DONE)
    int file_count;
    int64_t total_bytes;
    int64_t received_bytes;
    ls_received_file *files;  // non-NULL for in-memory transfers
} ls_event;

// fingerprint is a persisted device identity (1-32 bytes), reused across starts.
int  ls_start(const char *alias, const char *device_model, const char *fingerprint, int port, const char *staging_root);
void ls_stop(void);
void ls_set_alias(const char *alias);
void ls_set_visible(int visible);
int  ls_poll(ls_event *out);
void ls_event_free(ls_event *ev);
void ls_received_free(ls_received_file *files, int count);
void ls_respond(uint64_t session, int accept);
void ls_cancel(uint64_t session);
const char *ls_last_error(void);

// Win32 helpers. Every returned path/name is written into `out` (UTF-8).
int  cw_known_folder(int which, char *out, int out_len);  // 0 Downloads, 1 Pictures, 2 Documents, 3 Roaming AppData, 4 Local AppData
int  cw_user_display_name(char *out, int out_len);
int  cw_computer_name(char *out, int out_len);
int  cw_move_into(const char *src, const char *dest_dir, char *out, int out_len);
int  cw_write_into(const unsigned char *data, int64_t size, const char *name, const char *dest_dir, char *out, int out_len);
int  cw_remove_dir(const char *dir);
int  cw_open(const char *path_or_url);
int  cw_reveal(const char *path);
int  cw_set_startup(const char *name, const char *command);  // command NULL removes the entry
int  cw_get_startup(const char *name);
int  cw_exe_path(char *out, int out_len);
int  cw_folder_name(const char *path, char *out, int out_len);
int  cw_copy_text(const char *text);
int  cw_local_day(int64_t unix_seconds);  // yyyymmdd in local time
int  cw_bring_to_front(void);
void cw_work_area(int *left, int *top, int *right, int *bottom);  // physical pixels

#ifdef __cplusplus
}
#endif

#endif
