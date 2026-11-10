package fonts

import runtime "src:banana_runtime"
import style_native "src:core/style/native"
import "src:core/text"

// Static weights: a variable font's instances are laid out but drawn at the default weight.
@(private)
FIGTREE := [?][]u8{
    #load("assets/fonts/Figtree-Light.ttf"),
    #load("assets/fonts/Figtree-Regular.ttf"),
    #load("assets/fonts/Figtree-SemiBold.ttf"),
    #load("assets/fonts/Figtree-Bold.ttf"),
}

@(private)
PLEX_SANS := [?][]u8{
    #load("assets/fonts/IBMPlexSans-Regular.ttf"),
    #load("assets/fonts/IBMPlexSans-Medium.ttf"),
    #load("assets/fonts/IBMPlexSans-SemiBold.ttf"),
}

Fonts :: struct {
    body:    ^text.Font_Set,
    display: ^text.Font_Set,
}

// Registers the app's families with the calling UI thread's style host.
load :: proc() -> Fonts {
    f := Fonts{body = text.set_create(), display = text.set_create()}
    for data in PLEX_SANS do text.set_register(f.body, data, family = "IBM Plex Sans")
    for data in FIGTREE do text.set_register(f.display, data, family = "Figtree")
    host := runtime.Style_Host()
    style_native.register_font(host, "IBM Plex Sans", f.body)
    style_native.register_font(host, "system-ui", f.body)
    style_native.register_font(host, "Figtree", f.display)
    return f
}

destroy :: proc(f: Fonts) {
    text.set_destroy(f.body)
    text.set_destroy(f.display)
}
