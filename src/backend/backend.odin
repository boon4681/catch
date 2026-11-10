package backend

import "core:crypto"
import "core:encoding/hex"
import "core:encoding/json"
import "core:os"
import "core:strings"
import "core:time"

PORT        :: 53317
APP_NAME    :: "Catch"
MAX_HISTORY :: 200
MAX_FOLDERS :: 6

Mode :: enum {
    Ten_Minutes,
    One_Hour,
    Always,
    Off,
}

Event_Kind :: enum {
    None,
    Request,
    Progress,
    Landed,
    Canceled,
    Text,
}

Item_Kind :: enum {
    File,
    Files,
    Link,
    Text,
}

Item :: struct {
    id:     int,
    name:   string,
    sender: string,
    kind:   Item_Kind,
    path:   string, // the file, the first file of a batch, or the text itself
    folder: string,
    count:  int,
    at:     i64,
}

Settings :: struct {
    alias:         string,
    mode:          Mode,
    visible_until: i64,
    popup:         bool,
    sound:         bool,
    last_folder:   string,
    folders:       [dynamic]string,
}

// Strings live until the next poll.
Event :: struct {
    kind:     Event_Kind,
    session:  u64,
    sender:   string,
    device:   string,
    title:    string,
    mime:     string,
    count:    int,
    total:    i64,
    received: i64,
}

Save_Result :: struct {
    saved:  int,
    failed: int,
    first:  string,
}

@(private)
Stored :: struct {
    settings: Settings,
    history:  [dynamic]Item,
    next_id:  int,
    fingerprint: string,
}

@(private)
Landed :: struct {
    session: u64,
    sender:  string,
    title:   string,
    mime:    string,
    dir:     string,
    paths:   [dynamic]string,
    files:   [^]LS_Received_File,
    count:   int,
    saved:   []bool,
}

@(private)
state: struct {
    started:    bool,
    error:      string,
    user:       string,
    downloads:  string,
    pictures:   string,
    state_path: string,
    staging:    string,
    stored:     Stored,
    landed:     Landed,
    event:      LS_Event,
}

@(private)
from_buffer :: proc(buf: []u8, n: i32) -> string {
    if n <= 0 do return ""
    return strings.clone(string(buf[:n]))
}

@(private)
known_folder :: proc(which: Known_Folder) -> string {
    buf: [2048]u8
    return from_buffer(buf[:], cw_known_folder(which, raw_data(buf[:]), len(buf)))
}

@(private)
cstr :: proc(s: string) -> cstring {
    return strings.clone_to_cstring(s, context.temp_allocator)
}

@(private)
replace :: proc(target: ^string, value: string) {
    delete(target^)
    target^ = strings.clone(value)
}

@(private)
persist :: proc() -> bool {
    data, err := json.marshal(state.stored, {pretty = true}, context.temp_allocator)
    if err != nil do return false
    return os.write_entire_file(state.state_path, data) == nil
}

@(private)
ensure_identity :: proc() -> bool {
    if state.stored.fingerprint != "" do return true
    // Save before advertising so even an unclean exit keeps the same identity.
    entropy: [16]u8
    crypto.rand_bytes(entropy[:])
    state.stored.fingerprint = string(hex.encode(entropy[:]))
    if !persist() {
        delete(state.stored.fingerprint)
        state.stored.fingerprint = ""
        return false
    }
    return true
}

@(private)
load :: proc() {
    st := &state.stored
    if data, err := os.read_entire_file(state.state_path, context.allocator); err == nil {
        json.unmarshal(data, st)
        delete(data)
    }
    s := &st.settings
    if s.alias == "" {
        buf: [256]u8
        s.alias = from_buffer(buf[:], cw_computer_name(raw_data(buf[:]), len(buf)))
        s.popup = true
        s.mode = .Ten_Minutes
        s.visible_until = now() + 10 * 60
    }
    if s.last_folder == "" do s.last_folder = strings.clone(state.downloads)
}

// ---------------------------------------------------------------- lifecycle

start :: proc() -> bool {
    if state.started do return state.error == ""
    state.started = true

    buf: [256]u8
    state.user = from_buffer(buf[:], cw_user_display_name(raw_data(buf[:]), len(buf)))
    state.downloads = known_folder(.Downloads)
    state.pictures = known_folder(.Pictures)

    app_dir := strings.concatenate({known_folder(.Roaming_Data), "\\", APP_NAME})
    _ = os.make_directory_all(app_dir)
    state.state_path = strings.concatenate({app_dir, "\\state.json"})
    temp_dir, temp_err := os.temp_dir(context.temp_allocator)
    if temp_err != nil {
        state.error = strings.clone("Could not access the temporary folder.")
        return false
    }
    state.staging = strings.concatenate({temp_dir, "\\Catch-incoming"})
    if os.make_directory_all(state.staging) != nil {
        state.error = strings.clone("Could not create the temporary receiving folder.")
        return false
    }

    load()
    if !ensure_identity() {
        state.error = strings.clone("Could not save the device identity. Check access to Catch's settings folder.")
        return false
    }
    s := &state.stored.settings
    if ls_start(cstr(s.alias), "Windows", cstr(state.stored.fingerprint), PORT, cstr(state.staging)) == 0 {
        state.error = strings.clone(string(ls_last_error()))
        return false
    }
    ls_set_visible(1 if s.mode != .Off else 0)
    return true
}

stop :: proc() {
    if !state.started do return
    persist()
    ls_stop()
    discard_landed()
    ls_event_free(&state.event)
}

error :: proc() -> string {
    return state.error
}

// ---------------------------------------------------------------- events

poll :: proc() -> Event {
    ev := &state.event
    ls_event_free(ev)
    if ls_poll(ev) == 0 do return {}
    out := Event{
        kind     = Event_Kind(ev.kind),
        session  = ev.session,
        sender   = string(ev.sender),
        device   = string(ev.device),
        title    = string(ev.title),
        mime     = string(ev.mime),
        count    = int(ev.file_count),
        total    = ev.total_bytes,
        received = ev.received_bytes,
    }
    #partial switch out.kind {
    case .Landed:
        keep_landed(ev)
    case .Text:
        record(Item{name = out.title, sender = out.sender, kind = .Link if is_url(out.title) else .Text, path = out.title, count = 1})
    }
    return out
}

@(private)
keep_landed :: proc(ev: ^LS_Event) {
    discard_landed()
    l := &state.landed
    l.session = ev.session
    l.sender = strings.clone(string(ev.sender))
    l.title = strings.clone(string(ev.title))
    l.mime = strings.clone(string(ev.mime))
    l.dir = strings.clone(string(ev.dir))
    for line in strings.split_lines(string(ev.paths), context.temp_allocator) {
        if line != "" do append(&l.paths, strings.clone(line))
    }
    l.files = ev.files
    l.count = int(ev.file_count)
    l.saved = make([]bool, l.count)
    // Transfer ownership of memory and temporary files to state.landed.
    ev.files = nil
    ev.kind = LS_EV_NONE
}

@(private)
discard_landed :: proc() {
    l := &state.landed
    for p in l.paths do os.remove(p)
    if l.dir != "" do cw_remove_dir(cstr(l.dir))
    ls_received_free(l.files, i32(l.count))
    delete(l.saved)
    delete(l.sender)
    delete(l.title)
    delete(l.mime)
    delete(l.dir)
    for p in l.paths do delete(p)
    delete(l.paths)
    l^ = {}
}

accept :: proc(session: u64) { ls_respond(session, 1) }
decline :: proc(session: u64) { ls_respond(session, 0) }
cancel :: proc(session: u64) { ls_cancel(session) }

discard :: proc(session: u64) {
    if state.landed.session == session do discard_landed()
}

// Files of the session that landed last, still in the staging folder.
landed_paths :: proc(session: u64) -> []string {
    if state.landed.session != session do return nil
    paths := make([dynamic]string, 0, len(state.landed.paths), context.temp_allocator)
    for p, i in state.landed.paths do if !state.landed.saved[i] do append(&paths, p)
    return paths[:]
}

landed_in_memory :: proc(session: u64) -> bool {
    return state.landed.session == session && state.landed.files != nil
}

landed_size :: proc(session: u64) -> i64 {
    total: i64 = 0
    l := &state.landed
    if l.session != session do return 0
    if l.files != nil {
        for i in 0..<l.count do if !l.saved[i] do total += l.files[i].size
        return total
    }
    for p in landed_paths(session) {
        if info, err := os.stat(p, context.temp_allocator); err == nil do total += info.size
    }
    return total
}

// Encoded preview bytes remain valid until this held transfer is discarded.
landed_image :: proc(session: u64) -> []u8 {
    l := &state.landed
    if l.session != session || l.files == nil do return nil
    for i in 0..<l.count {
        if !l.saved[i] && is_image(string(l.files[i].name)) {
            return l.files[i].data[:int(l.files[i].size)]
        }
    }
    return nil
}

landed_count :: proc(session: u64) -> int {
    l := &state.landed
    if l.session != session do return 0
    count := 0
    for saved in l.saved do if !saved do count += 1
    return count
}

// Only the Save action calls this: write memory or move temporary files.
save_landed :: proc(session: u64, folder: string, name: string) -> Save_Result {
    l := &state.landed
    result: Save_Result
    if l.session != session || l.count == 0 do return result
    for i in 0..<l.count {
        if l.saved[i] do continue
        buf: [2048]u8
        ok: i32
        if l.files != nil {
            f := &l.files[i]
            ok = cw_write_into(f.data, f.size, f.name, cstr(folder), raw_data(buf[:]), len(buf))
        } else {
            ok = cw_move_into(cstr(l.paths[i]), cstr(folder), raw_data(buf[:]), len(buf))
        }
        if ok == 0 {
            result.failed += 1
            continue
        }
        result.saved += 1
        l.saved[i] = true
        if result.first == "" do result.first = strings.clone(string(cstring(raw_data(buf[:]))), context.temp_allocator)
    }
    if result.saved > 0 {
        record(Item{
            name   = name,
            sender = l.sender,
            kind   = .File if result.saved == 1 else .Files,
            path   = result.first,
            folder = folder,
            count  = result.saved,
        })
        replace(&state.stored.settings.last_folder, folder)
        persist()
    }
    if result.failed == 0 do discard_landed()
    return result
}

// ---------------------------------------------------------------- history

@(private)
record :: proc(item: Item) {
    st := &state.stored
    st.next_id += 1
    inject_at(&st.history, 0, Item{
        id     = st.next_id,
        name   = strings.clone(item.name),
        sender = strings.clone(item.sender),
        kind   = item.kind,
        path   = strings.clone(item.path),
        folder = strings.clone(item.folder),
        count  = item.count,
        at     = now(),
    })
    // Dropped items are not freed: a view built this frame may still hold them.
    if len(st.history) > MAX_HISTORY do pop(&st.history)
    persist()
}

history :: proc() -> []Item {
    return state.stored.history[:]
}

history_count :: proc() -> int {
    return len(state.stored.history)
}

// ---------------------------------------------------------------- settings

settings :: proc() -> Settings {
    return state.stored.settings
}

set_mode :: proc(mode: Mode, visible_until: i64) {
    s := &state.stored.settings
    s.mode = mode
    s.visible_until = visible_until
    ls_set_visible(1 if mode != .Off else 0)
    persist()
}

set_alias :: proc(alias: string) {
    name := strings.trim_space(alias)
    s := &state.stored.settings
    if name == "" || name == s.alias do return
    replace(&s.alias, name)
    ls_set_alias(cstr(name))
    persist()
}

set_popup :: proc(on: bool) {
    state.stored.settings.popup = on
    persist()
}

set_sound :: proc(on: bool) {
    state.stored.settings.sound = on
    persist()
}

// Remembers a folder the user picked, most recent first.
remember_folder :: proc(path: string) {
    s := &state.stored.settings
    if path == "" do return
    for f, i in s.folders {
        if strings.equal_fold(f, path) {
            ordered_remove(&s.folders, i)
            break
        }
    }
    inject_at(&s.folders, 0, strings.clone(path))
    if len(s.folders) > MAX_FOLDERS do pop(&s.folders)
    persist()
}

startup :: proc() -> bool {
    return cw_get_startup(APP_NAME) != 0
}

set_startup :: proc(on: bool) {
    if !on {
        cw_set_startup(APP_NAME, nil)
        return
    }
    buf: [1024]u8
    n := cw_exe_path(raw_data(buf[:]), len(buf))
    if n <= 0 do return
    command := strings.concatenate({"\"", string(buf[:n]), "\" --minimized"}, context.temp_allocator)
    cw_set_startup(APP_NAME, cstr(command))
}

// ---------------------------------------------------------------- system

now :: proc() -> i64 {
    return time.to_unix_seconds(time.now())
}

// yyyymmdd in local time.
local_day :: proc(unix_seconds: i64) -> int {
    return int(cw_local_day(unix_seconds))
}

user_name :: proc() -> string { return state.user }
downloads :: proc() -> string { return state.downloads }
pictures :: proc() -> string { return state.pictures }

folder_name :: proc(path: string) -> string {
    buf: [512]u8
    n := cw_folder_name(cstr(path), raw_data(buf[:]), len(buf))
    if n <= 0 do return path
    return strings.clone(string(buf[:n]), context.temp_allocator)
}

is_url :: proc(text: string) -> bool {
    return (strings.has_prefix(text, "http://") || strings.has_prefix(text, "https://")) && strings.index_any(text, " \t\r\n") < 0
}

url_host :: proc(url: string) -> string {
    i := strings.index(url, "://")
    if i < 0 do return ""
    rest := url[i + 3:]
    if end := strings.index_any(rest, "/?#"); end >= 0 do rest = rest[:end]
    return rest
}

is_image :: proc(path: string) -> bool {
    lower := strings.to_lower(path, context.temp_allocator)
    for ext in ([]string{".png", ".jpg", ".jpeg", ".gif", ".bmp"}) {
        if strings.has_suffix(lower, ext) do return true
    }
    return false
}

open :: proc(path_or_url: string) { cw_open(cstr(path_or_url)) }
reveal :: proc(path: string) { cw_reveal(cstr(path)) }
copy_text :: proc(text: string) { cw_copy_text(cstr(text)) }
bring_to_front :: proc() { cw_bring_to_front() }


folders :: proc() -> []string {
    return state.stored.settings.folders[:]
}

// Desktop area not covered by the taskbar, in physical pixels.
work_area :: proc() -> (left, top, right, bottom: int) {
    l, t, r, b: i32
    cw_work_area(&l, &t, &r, &b)
    return int(l), int(t), int(r), int(b)
}

file_exists :: proc(path: string) -> bool {
    return path != "" && os.exists(path)
}
