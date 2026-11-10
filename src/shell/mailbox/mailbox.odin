package mailbox

// Hands a preview from the main UI thread to the popup's thread, and the
// popup's choices back. Everything crossing threads is copied under `lock`.

import "core:strings"
import "core:sync"
import "core:slice"
import "base:runtime"
import "src:core/platform"

Kind :: enum {
    None,
    File,
    Batch,
    Link,
    Request,
    Receiving,
    Saved,
    Notice,
}

Preview :: struct {
    serial:    int,
    kind:      Kind,
    session:   u64,
    sender:    string,
    title:     string,
    mime:      string,
    image:     string,
    image_bytes: []u8, // input to publish; the popup obtains its own copy
    image_memory: bool,
    held_in_memory: bool,
    count:     int,
    size:      i64,
    is_url:    bool,
    folders:   []string,
    picked:    string,
    downloads: string,
    pictures:  string,
    meta:      string,
    initial:   string,
    progress:  f32,
    photo:     bool,
}

Action_Kind :: enum {
    None,
    Save,
    Close,
    Open,
    Copy,
    Remember,
    Accept,
    Decline,
    Cancel,
    Reveal,
    Dismiss,
}

Action :: struct {
    kind:    Action_Kind,
    session: u64,
    folder:  string,
}

@(private)
lock: sync.Mutex

// The popup reads the newest preview without copying it again, so the ones
// before it stay alive for a few more publishes (progress publishes often).
@(private)
generations: [4]Preview

@(private)
newest: int

@(private)
serial: int

@(private)
actions: [dynamic]Action

@(private)
image_data: []u8
@(private)
image_source: [^]u8
@(private)
image_session: u64

@(private)
clone_preview :: proc(p: Preview) -> Preview {
    out := p
    out.sender = strings.clone(p.sender)
    out.title = strings.clone(p.title)
    out.mime = strings.clone(p.mime)
    out.image = strings.clone(p.image)
    out.image_bytes = nil
    out.image_memory = len(p.image_bytes) > 0
    out.picked = strings.clone(p.picked)
    out.downloads = strings.clone(p.downloads)
    out.pictures = strings.clone(p.pictures)
    out.meta = strings.clone(p.meta)
    out.initial = strings.clone(p.initial)
    folders := make([]string, len(p.folders))
    for f, i in p.folders do folders[i] = strings.clone(f)
    out.folders = folders
    return out
}

@(private)
free_preview :: proc(p: ^Preview) {
    delete(p.sender)
    delete(p.title)
    delete(p.mime)
    delete(p.image)
    delete(p.picked)
    delete(p.downloads)
    delete(p.pictures)
    delete(p.meta)
    delete(p.initial)
    for f in p.folders do delete(f)
    delete(p.folders)
    p^ = {}
}

// Main thread.
publish :: proc(p: Preview) {
    sync.lock(&lock)
    defer sync.unlock(&lock)
    serial += 1
    if p.session != image_session || raw_data(p.image_bytes) != image_source || len(p.image_bytes) != len(image_data) {
        delete(image_data)
        image_data = slice.clone(p.image_bytes)
        image_source = raw_data(p.image_bytes)
        image_session = p.session
    }
    next := (newest + 1) % len(generations)
    free_preview(&generations[next])
    generations[next] = clone_preview(p)
    generations[next].serial = serial
    newest = next
}

// The popup owns this snapshot while decoding, even if the main thread closes
// or saves the held transfer at the same time.
copy_image :: proc(session: u64, allocator: runtime.Allocator) -> []u8 {
    sync.lock(&lock)
    defer sync.unlock(&lock)
    if image_session != session do return nil
    return slice.clone(image_data, allocator)
}

// Popup thread. Valid until the fourth publish after this call.
current :: proc() -> Preview {
    sync.lock(&lock)
    defer sync.unlock(&lock)
    return generations[newest]
}

// Whether the popup has anything to show; it closes once this turns false.
visible :: proc() -> bool {
    sync.lock(&lock)
    defer sync.unlock(&lock)
    return generations[newest].kind != .None
}

// Popup thread.
post :: proc(kind: Action_Kind, session: u64, folder: string = "") {
    sync.lock(&lock)
    defer sync.unlock(&lock)
    append(&actions, Action{kind = kind, session = session, folder = strings.clone(folder)})
}

// Main thread. The folder string lives until the next take.
take :: proc() -> Action {
    @(static) last: string
    sync.lock(&lock)
    defer sync.unlock(&lock)
    delete(last)
    last = ""
    if len(actions) == 0 do return {}
    a := actions[0]
    ordered_remove(&actions, 0)
    last = a.folder
    return a
}

@(private, thread_local)
browsed: string

// Popup thread: native folder picker; the choice arrives through take_browsed.
browse :: proc(w: ^platform.Window) {
    platform.file_dialog(w, {mode = .Open_Folder, title = "Save to folder"}, proc(w: ^platform.Window, files: []platform.Dialog_File, user: rawptr) {
        if len(files) == 0 || files[0].path == "" do return
        delete(browsed)
        browsed = strings.clone(files[0].path)
    })
}

take_browsed :: proc() -> string {
    if browsed == "" do return ""
    out := strings.clone(browsed, context.temp_allocator)
    delete(browsed)
    browsed = ""
    return out
}
