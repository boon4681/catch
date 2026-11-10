package catch

import main "generated:main"
import c "generated:app/components"
import runtime "src:banana_runtime"
import "core:os"
import "core:time"
import "src:core/common"
import "src:core/events"
import "src:core/layout"
import "src:core/paint"
import "src:core/painter"
import "src:core/platform"
import "src:core/text"
import "src:ui/text_node"
import "src:core/inspector"
import "src:core/redraw"
import "catch:kit/fonts"
import "catch:kit/sound"
import "catch:shell"
import backend "catch:backend"

devtools: ^inspector.Inspector

TRAY_SHOW :: 1
TRAY_EXIT :: 2

render_frame :: proc(window: ^platform.Window) {
    scale := window.scale if window.scale > 0 else 1
    layout.update(
        window.root,
        f32(window.width) / scale,
        f32(window.height) / scale,
    )
    // inspector.update(devtools)

    if redraw.dirty() {
        painter.begin_frame(window.painter, common.Color{0, 0, 0, 0})
        dpi := common.IDENTITY_TRANSFORM
        dpi.scale = {scale, scale}
        painter.push_transform(window.painter, dpi, {0, 0})
        paint.draw(window.painter, window.root)
        painter.pop_transform(window.painter)
        painter.end_frame(window.painter)
    }

    free_all(context.temp_allocator)
}

main :: proc() {
    platform.prefer_nvd_gpu(true)
    layout.scheduler_setup()
    defer layout.scheduler_shutdown()

    loaded := fonts.load()
    defer fonts.destroy(loaded)
    font := loaded.body

    window := platform.New({
        width       = 760,
        height      = 500,
        title       = "Catch",
        transparent = true,
            // vsync       = true,
        frame_mode = .On_Demand,
            // decorated   = false,
    })
    // Sizes are physical pixels; the design is 760x500 at 100%.
    ui_scale := window.scale if window.scale > 0 else 1
    platform.set_size(window, int(760 * ui_scale), int(500 * ui_scale))
    platform.set_min_size(window, int(760 * ui_scale), int(500 * ui_scale))
    defer platform.free(window)
    window.root->style()->set_font(font)->set_padding(0)
    defer backend.stop()
    defer sound.shutdown()
    defer shell.stop()
    app := main.App()
    window.root->add(app)
    title_bar := runtime.result(app->get_by_key("title-bar"), c.TitleBar_Result)
    frame := platform.enable_native_frame(window)
    frame->set_title_bar({
        caption_node    = title_bar.root,
        minimize_button = title_bar.minimize_button,
        close_button    = title_bar.close_button,
    })

    frame->set_background_erase(false)
    frame->set_shadow(true)
    frame->set_resize_border(8)
    frame->set_clamp_maximized(true)

    // devtools = inspector.New(window, {
    //     overlay_font = font,
    // })
    // defer inspector.destroy(devtools)

    platform.set_taskbar_visible(window, false)
    // Empty icon path: the tray reuses the exe icon embedded from catch.rc.
    tray := platform.tray_create(window, {
        tooltip = "Catch",
        menu    = {
            {id = TRAY_SHOW, label = "Show Catch"},
            {separator = true},
            {id = TRAY_EXIT, label = "Exit"},
        },
    })
    defer platform.tray_destroy(tray)
    window->on(events.TRAY_CLICK_EVENT, proc(s: ^events.Event_Signal) {
        tray_event := cast(^events.Tray_Event)s.data
        if tray_event.button == 0 do platform.show(cast(^platform.Window)s.current_target)
    })
    window->on(events.TRAY_MENU_EVENT, proc(s: ^events.Event_Signal) {
        w := cast(^platform.Window)s.current_target
        switch (cast(^events.Tray_Event)s.data).item {
        case TRAY_SHOW: platform.show(w)
        case TRAY_EXIT: platform.close(w)
        }
    })

    layout.awake_window(window)
    for arg in os.args {
        if arg == "--minimized" do platform.minimize(window)
    }
    for arg, i in os.args {
        if arg == "--shot" && i + 1 < len(os.args) {
            // Let entrance animations settle before capturing.
            settle := time.tick_now()
            for time.tick_since(settle) < 1500 * time.Millisecond {
                platform.update(window)
                render_frame(window)
            }
            if !platform.capture(window, os.args[i + 1]) do os.exit(1)
            return
        }
    }

    window.on_refresh = render_frame
    for platform.update(window) do render_frame(window)
}
