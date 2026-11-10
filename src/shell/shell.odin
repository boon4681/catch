package shell

// The preview popup is its own window on its own thread: banana runs one
// window per thread, each with its own signal graph and style host.

import "base:intrinsics"
import "core:thread"
import "src:core/common"
import "src:core/layout"
import "src:core/paint"
import "src:core/painter"
import "src:core/platform"
import "src:core/redraw"
import popup "generated:app/popup"
import "catch:kit/fonts"
import "catch:shell/mailbox"

@(private)
worker: ^thread.Thread

@(private)
alive: u32

@(private)
quit: u32

// Main thread: shows what the popup should show, opening its window if it is closed.
show :: proc(p: mailbox.Preview) {
    mailbox.publish(p)
    if intrinsics.atomic_load(&alive) != 0 do return
    if worker != nil {
        thread.join(worker)
        thread.destroy(worker)
    }
    intrinsics.atomic_store(&alive, 1)
    worker = thread.create_and_start(run)
}

// Main thread, on exit.
stop :: proc() {
    intrinsics.atomic_store(&quit, 1)
    if worker != nil {
        thread.join(worker)
        thread.destroy(worker)
        worker = nil
    }
}

@(private)
render :: proc(w: ^platform.Window) {
    scale := w.scale if w.scale > 0 else 1
    layout.update(w.root, f32(w.width) / scale, f32(w.height) / scale)
    if redraw.dirty() {
        painter.begin_frame(w.painter, common.Color{0, 0, 0, 0})
        dpi := common.IDENTITY_TRANSFORM
        dpi.scale = {scale, scale}
        painter.push_transform(w.painter, dpi, {0, 0})
        paint.draw(w.painter, w.root)
        painter.pop_transform(w.painter)
        painter.end_frame(w.painter)
    }
    free_all(context.temp_allocator)
}

@(private)
run :: proc() {
    defer intrinsics.atomic_store(&alive, 0)
    layout.scheduler_setup()
    defer layout.scheduler_shutdown()

    f := fonts.load()
    defer fonts.destroy(f)

    // Something may arrive while the window is closing; open it again for that.
    for intrinsics.atomic_load(&quit) == 0 {
        open_window(f)
        if !mailbox.visible() do break
    }
}

@(private)
open_window :: proc(f: fonts.Fonts) {
    w := platform.New({
        width       = 440,
        height      = 160,
        title       = "Catch",
        transparent = true,
        decorated   = false,
        frame_mode  = .On_Demand,
    })
    defer platform.free(w)
    w.root->style()->set_font(f.body)->set_padding(0)
    w.root->add(popup.Popup())

    platform.enable_native_frame(w)->
        set_shadow(false)->
        set_resize(false)->
        set_background_erase(false)->
        set_always_top_most(true)->
        set_click_through(true)

    layout.awake_window(w)
    w.on_refresh = render
    user_closed := false
    for intrinsics.atomic_load(&quit) == 0 {
        if !platform.update(w) {
            user_closed = true
            break
        }
        render(w)
    }
    if user_closed && mailbox.visible() {
        p := mailbox.current()
        kind := mailbox.Action_Kind.Close
        if p.kind == .Request do kind = .Decline
        if p.kind == .Receiving do kind = .Cancel
        if p.kind == .Saved || p.kind == .Notice do kind = .Dismiss
        mailbox.post(kind, p.session)
        mailbox.publish({})
    }
}
