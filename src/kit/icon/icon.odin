package icon

import css "src:core/style"
import style_native "src:core/style/native"
import "src:core/node"
import "src:ui/svg_node"
import YG "src:yoga"

BANANA_COMPONENT      :: true
BANANA_COMPONENT_TYPE :: ^Icon

Icon_Style :: struct {
    using base: node.Style,
}

// A Lucide icon (ISC license) by name, stroked with the inherited text color.
Icon :: struct {
    using node: node.BaseNode,
    style: proc(self: ^Icon) -> ^Icon_Style,
    glyph: ^svg_node.SVG_Node,
    size:  f32,
}

New :: proc(name: string = "", size: f32 = 20, key: Maybe(string) = nil) -> ^Icon {
    n := new(Icon)
    node.Init(n, key)
    n.style = _get_style
    node.Set_Style(auto_cast(n), new_clone(Icon_Style{}))
    node.Init_Style(auto_cast(n))
    n.glyph, _ = svg_node.New(source_of(name))
    n.glyph->style()->set_width(100, .Percent)->set_height(100, .Percent)->set_pointer_events(.None)
    n->add(n.glyph)
    n->style()->set_pointer_events(.None)
    set_size(n, size)
    return n
}

set_name :: proc(n: ^Icon, name: string) {
    n.glyph->set_svg(source_of(name))
}

set_size :: proc(n: ^Icon, size: f32) {
    n.size = size
    n->style()->set_width(size)->set_height(size)->set_flex_shrink(0)
}

CSS_TAG :: "icon"

// The style engine computes `width: auto` for an icon no rule sizes and would
// drop the size given in markup, so the size comes back after every restyle.
apply_style :: proc(n: ^node.BaseNode, c: css.Computed_Style, alpha: f32, pseudo: style_native.Pseudo_Styles) {
    icon := cast(^Icon)n
    if icon.size <= 0 do return
    if c[.Width].kind == .Keyword && c[.Width].keyword == .Auto do YG.NodeStyleSetWidth(n.raw, icon.size)
    if c[.Height].kind == .Keyword && c[.Height].keyword == .Auto do YG.NodeStyleSetHeight(n.raw, icon.size)
    if c[.Flex_Shrink].number != 0 do YG.NodeStyleSetFlexShrink(n.raw, 0)
}

@(private = "file")
_get_style :: proc(self: ^Icon) -> ^Icon_Style {
    self->damage()
    return auto_cast(self._internal_style)
}

source_of :: proc(name: string) -> string {
    switch name {
    case "x": return _X
    case "minus": return _MINUS
    case "plus": return _PLUS
    case "check": return _CHECK
    case "check-2": return _CHECK_2
    case "radar": return _RADAR
    case "pencil": return _PENCIL
    case "sliders": return _SLIDERS
    case "inbox": return _INBOX
    case "power": return _POWER
    case "arrow-ur": return _ARROW_UR
    case "arrow-l": return _ARROW_L
    case "search": return _SEARCH
    case "info": return _INFO
    case "more": return _MORE
    case "globe": return _GLOBE
    case "copy": return _COPY
    case "folder": return _FOLDER
    case "image": return _IMAGE
    case "file": return _FILE
    case "text": return _TEXT
    }
    return ""
}

@(private)
_X :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>`
@(private)
_MINUS :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/></svg>`
@(private)
_PLUS :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>`
@(private)
_CHECK :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M20 6 9 17l-5-5"/></svg>`
@(private)
_CHECK_2 :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 6 9 17l-5-5"/></svg>`
@(private)
_RADAR :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4.9 19.1C1 15.2 1 8.8 4.9 4.9"/><path d="M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5"/><circle cx="12" cy="12" r="2"/><path d="M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5"/><path d="M19.1 4.9C23 8.8 23 15.1 19.1 19"/></svg>`
@(private)
_PENCIL :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"/><path d="m15 5 4 4"/></svg>`
@(private)
_SLIDERS :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="21" x2="14" y1="4" y2="4"/><line x1="10" x2="3" y1="4" y2="4"/><line x1="21" x2="12" y1="12" y2="12"/><line x1="8" x2="3" y1="12" y2="12"/><line x1="21" x2="16" y1="20" y2="20"/><line x1="12" x2="3" y1="20" y2="20"/><line x1="14" x2="14" y1="2" y2="6"/><line x1="8" x2="8" y1="10" y2="14"/><line x1="16" x2="16" y1="18" y2="22"/></svg>`
@(private)
_INBOX :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="22 12 16 12 14 15 10 15 8 12 2 12"/><path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/></svg>`
@(private)
_POWER :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.77.04"/></svg>`
@(private)
_ARROW_UR :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M7 7h10v10"/><path d="M7 17 17 7"/></svg>`
@(private)
_ARROW_L :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m12 19-7-7 7-7"/><path d="M19 12H5"/></svg>`
@(private)
_SEARCH :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/></svg>`
@(private)
_INFO :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/></svg>`
@(private)
_MORE :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/><circle cx="5" cy="12" r="1"/></svg>`
@(private)
_GLOBE :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20"/><path d="M2 12h20"/></svg>`
@(private)
_COPY :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="14" height="14" x="8" y="8" rx="2" ry="2"/><path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"/></svg>`
@(private)
_FOLDER :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"/></svg>`
@(private)
_IMAGE :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="18" x="3" y="3" rx="2" ry="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"/></svg>`
@(private)
_FILE :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/></svg>`
@(private)
_TEXT :: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17 6.1H3"/><path d="M21 12.1H3"/><path d="M15.1 18H3"/></svg>`
