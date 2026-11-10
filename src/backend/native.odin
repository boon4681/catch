package backend

import "core:c"

foreign import native {
    "../../native/build/catch_native.lib",
    "system:ws2_32.lib",
    "system:iphlpapi.lib",
    "system:shell32.lib",
    "system:ole32.lib",
    "system:secur32.lib",
    "system:advapi32.lib",
    "system:user32.lib",
}

LS_EV_NONE     :: 0
LS_EV_REQUEST  :: 1
LS_EV_PROGRESS :: 2
LS_EV_DONE     :: 3
LS_EV_CANCELED :: 4
LS_EV_TEXT     :: 5

LS_Received_File :: struct {
    name: cstring,
    data: [^]u8,
    size: i64,
}

LS_Event :: struct {
    kind:           c.int,
    session:        u64,
    sender:         cstring,
    device:         cstring,
    title:          cstring,
    mime:           cstring,
    dir:            cstring,
    paths:          cstring,
    file_count:     c.int,
    total_bytes:    i64,
    received_bytes: i64,
    files:          [^]LS_Received_File,
}

Known_Folder :: enum c.int {
    Downloads     = 0,
    Pictures      = 1,
    Documents     = 2,
    Roaming_Data  = 3,
    Local_Data    = 4,
}

@(default_calling_convention = "c")
foreign native {
    ls_start       :: proc(alias, device_model, fingerprint: cstring, port: c.int, staging_root: cstring) -> c.int ---
    ls_stop        :: proc() ---
    ls_set_alias   :: proc(alias: cstring) ---
    ls_set_visible :: proc(visible: c.int) ---
    ls_poll        :: proc(out: ^LS_Event) -> c.int ---
    ls_event_free  :: proc(ev: ^LS_Event) ---
    ls_received_free :: proc(files: [^]LS_Received_File, count: c.int) ---
    ls_respond     :: proc(session: u64, accept: c.int) ---
    ls_cancel      :: proc(session: u64) ---
    ls_last_error  :: proc() -> cstring ---

    cw_known_folder       :: proc(which: Known_Folder, out: [^]u8, out_len: c.int) -> c.int ---
    cw_user_display_name  :: proc(out: [^]u8, out_len: c.int) -> c.int ---
    cw_computer_name      :: proc(out: [^]u8, out_len: c.int) -> c.int ---
    cw_move_into          :: proc(src, dest_dir: cstring, out: [^]u8, out_len: c.int) -> c.int ---
    cw_write_into         :: proc(data: [^]u8, size: i64, name, dest_dir: cstring, out: [^]u8, out_len: c.int) -> c.int ---
    cw_remove_dir         :: proc(dir: cstring) -> c.int ---
    cw_open               :: proc(path_or_url: cstring) -> c.int ---
    cw_reveal             :: proc(path: cstring) -> c.int ---
    cw_set_startup        :: proc(name, command: cstring) -> c.int ---
    cw_get_startup        :: proc(name: cstring) -> c.int ---
    cw_exe_path           :: proc(out: [^]u8, out_len: c.int) -> c.int ---
    cw_folder_name        :: proc(path: cstring, out: [^]u8, out_len: c.int) -> c.int ---
    cw_copy_text          :: proc(text: cstring) -> c.int ---
    cw_local_day          :: proc(unix_seconds: i64) -> c.int ---
    cw_bring_to_front     :: proc() -> c.int ---
    cw_work_area          :: proc(left, top, right, bottom: ^c.int) ---
}
