//! The handful of controls the window needs, drawn with the design tokens:
//! buttons, pills, toggles, segmented choices, setting rows, icons and Clawd.

use std::time::Duration;

use gpui::{
    canvas, div, fill, percentage, point, prelude::*, px, rgb, size, svg, Animation, AnimationExt as _, AnyView, App,
    Bounds, ClickEvent, ElementId, Hsla, Rgba, SharedString, Svg, Transformation, Window,
};

use super::theme::{Theme, FONT_BODY, FONT_DISPLAY, FONT_MONO, R_ITEM, R_M, R_PILL, R_S};
use crate::art::{cell_rgb, idle_cell, IDLE_H, IDLE_W};

type Handler = Box<dyn Fn(&ClickEvent, &mut Window, &mut App) + 'static>;

/// Button look.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ButtonKind {
    /// Accent fill: the one action that moves things forward. At most one
    /// per view.
    Primary,
    /// Hairline outline.
    Normal,
    /// Hairline outline, compact (row actions).
    Small,
}

/// A rounded button. Disabled buttons are flat and grey whatever their
/// kind, so a disabled primary never reads as a dimmed-but-clickable one.
pub fn button(
    t: &Theme,
    id: impl Into<ElementId>,
    label: impl Into<SharedString>,
    kind: ButtonKind,
    enabled: bool,
    on_click: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
) -> impl IntoElement {
    let (bg, fg, border) = match (kind, enabled) {
        (_, false) => (t.panel, t.mute, t.panel),
        (ButtonKind::Primary, true) => (t.accent, t.accent_ink, t.accent),
        (_, true) => (t.surface, t.text, t.line),
    };
    let hover_bg = match kind {
        ButtonKind::Primary => t.focus,
        _ => t.hover,
    };
    let (pad_x, h, size) = if kind == ButtonKind::Small { (12., 28., 13.) } else { (16., 34., 14.) };
    let id: ElementId = id.into();
    let selector = id.to_string();
    let el = div()
        .id(id)
        .debug_selector(|| selector)
        .flex()
        .flex_shrink_0()
        .items_center()
        .justify_center()
        .h(px(h))
        .px(px(pad_x))
        .rounded(px(R_ITEM))
        .border_1()
        .border_color(border)
        .bg(bg)
        .text_color(fg)
        .text_size(px(size))
        .whitespace_nowrap()
        .child(label.into());
    if enabled {
        let handler: Handler = Box::new(on_click);
        el.cursor_pointer().hover(move |s| s.bg(hover_bg)).on_click(move |e, w, cx| handler(e, w, cx))
    } else {
        el
    }
}

/// A bundled monochrome icon (`assets/icons/<name>.svg`, Lucide) at `size`
/// px in `color`. GPUI paints an `svg()` only in its own text colour (it
/// doesn't inherit one), so hover changes go through `group_hover`.
pub fn icon(name: &str, size: f32, color: impl Into<Hsla>) -> Svg {
    svg().path(SharedString::from(format!("icons/{name}.svg"))).flex_shrink_0().size(px(size)).text_color(color)
}

/// A hover tooltip: one short line.
struct Tip(SharedString);

impl Render for Tip {
    fn render(&mut self, window: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        let t = Theme::for_appearance(window.appearance());
        div()
            .px(px(8.))
            .py(px(4.))
            .rounded(px(R_S))
            .bg(t.panel)
            .border_1()
            .border_color(t.line)
            .font_family(FONT_BODY)
            .text_size(px(12.5))
            .text_color(t.text)
            .child(self.0.clone())
    }
}

/// A tooltip builder for `.tooltip(…)`: names an icon-only control.
pub fn tooltip(text: &'static str) -> impl Fn(&mut Window, &mut App) -> AnyView + 'static {
    move |_, cx| cx.new(|_| Tip(text.into())).into()
}

/// A square icon-only button (page actions) named by `label` in its
/// tooltip. `spinning` turns the glyph while something runs.
pub fn icon_button(
    t: &Theme,
    id: &'static str,
    name: &str,
    label: &'static str,
    enabled: bool,
    spinning: bool,
    on_click: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
) -> impl IntoElement {
    let text = t.text;
    let glyph = icon(name, 16., if enabled { t.dim } else { t.mute }).group_hover(id, move |s| s.text_color(text));
    let glyph = if spinning {
        glyph
            .with_animation("spin", Animation::new(Duration::from_millis(900)).repeat(), |g, d| {
                g.with_transformation(Transformation::rotate(percentage(d)))
            })
            .into_any_element()
    } else {
        glyph.into_any_element()
    };
    let el = div()
        .id(id)
        .group(id)
        .debug_selector(move || id.to_owned())
        .flex()
        .flex_shrink_0()
        .items_center()
        .justify_center()
        .size(px(30.))
        .rounded(px(R_ITEM))
        .tooltip(tooltip(label))
        .child(glyph);
    if enabled {
        let handler: Handler = Box::new(on_click);
        el.cursor_pointer().hover(|s| s.bg(t.hover)).on_click(move |e, w, cx| handler(e, w, cx))
    } else {
        el
    }
}

/// A message box: a tone-coloured dot, body text in the normal colour so it
/// stays readable, optional actions underneath. Problems (warn, bad) tint
/// the box too; everything else stays neutral.
pub fn notice(t: &Theme, tone: Tone, text: impl Into<SharedString>) -> gpui::Div {
    let c = tone_color(t, tone);
    let loud = matches!(tone, Tone::Warn | Tone::Bad);
    div()
        .flex()
        .flex_col()
        .gap(px(10.))
        .px(px(14.))
        .py(px(12.))
        .rounded(px(R_M))
        .bg(if loud { tint(c, 0.08) } else { t.panel.into() })
        .border_1()
        .border_color(if loud { tint(c, 0.45) } else { t.line.into() })
        .child(div().flex().items_start().gap(px(10.)).child(div().mt(px(6.)).child(dot(t, tone))).child(
            div().flex_1().min_w_0().text_size(px(13.)).line_height(px(19.)).text_color(t.text).child(text.into()),
        ))
}

/// Small uppercase-free caption above a block (diffs, logs).
pub fn caption(t: &Theme, text: impl Into<SharedString>) -> impl IntoElement {
    div().text_size(px(12.)).text_color(t.mute).child(text.into())
}

/// The device's bar colour for a usage percentage: green below 50 %,
/// accent (the device's amber) below 80 %, red from 80 %.
pub fn usage_tone(pct: u32) -> Tone {
    match pct {
        0..50 => Tone::Ok,
        50..80 => Tone::Accent,
        _ => Tone::Bad,
    }
}

/// A rounded progress bar, `frac` 0..1.
pub fn bar(t: &Theme, frac: f32, color: Rgba) -> impl IntoElement {
    div()
        .h(px(8.))
        .w_full()
        .rounded(px(R_PILL))
        .bg(t.track)
        .child(div().h_full().w(gpui::relative(frac.clamp(0., 1.))).rounded(px(R_PILL)).bg(color))
}

/// Semantic tone for pills and dots.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tone {
    /// Neutral.
    Plain,
    /// Clawd / in progress.
    Accent,
    /// Good.
    Ok,
    /// Needs a look.
    Warn,
    /// Broken.
    Bad,
}

/// Colour for a tone.
pub fn tone_color(t: &Theme, tone: Tone) -> Rgba {
    match tone {
        Tone::Plain => t.dim,
        Tone::Accent => t.accent,
        Tone::Ok => t.ok,
        Tone::Warn => t.warn,
        Tone::Bad => t.bad,
    }
}

fn tint(c: Rgba, alpha: f32) -> Hsla {
    let mut h: Hsla = c.into();
    h.a = alpha;
    h
}

/// A small status chip.
pub fn pill(t: &Theme, text: impl Into<SharedString>, tone: Tone) -> impl IntoElement {
    let c = tone_color(t, tone);
    div()
        .flex()
        .flex_shrink_0()
        .items_center()
        .gap(px(6.))
        .px(px(10.))
        .py(px(3.))
        .rounded(px(R_PILL))
        .bg(tint(c, if tone == Tone::Plain { 0.12 } else { 0.16 }))
        .text_color(if tone == Tone::Plain { t.dim } else { c })
        .text_size(px(12.))
        .whitespace_nowrap()
        .child(div().size(px(6.)).rounded(px(R_PILL)).bg(c))
        .child(text.into())
}

/// A round status dot.
pub fn dot(t: &Theme, tone: Tone) -> impl IntoElement {
    div().flex_shrink_0().size(px(8.)).rounded(px(R_PILL)).bg(tone_color(t, tone))
}

/// An on/off switch.
pub fn toggle(
    t: &Theme,
    id: impl Into<ElementId>,
    on: bool,
    enabled: bool,
    on_click: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
) -> impl IntoElement {
    let track = if on { t.accent } else { t.track };
    let knob = if on { t.accent_ink } else { t.dim };
    let id: ElementId = id.into();
    let selector = id.to_string();
    let el = div()
        .id(id)
        .debug_selector(|| selector)
        .flex()
        .flex_shrink_0()
        .items_center()
        .w(px(38.))
        .h(px(22.))
        .p(px(3.))
        .rounded(px(R_PILL))
        .bg(track)
        .when(on, |d| d.justify_end())
        .child(div().size(px(16.)).rounded(px(R_PILL)).bg(knob));
    if enabled {
        let handler: Handler = Box::new(on_click);
        el.cursor_pointer().on_click(move |e, w, cx| handler(e, w, cx))
    } else {
        el.opacity(0.5)
    }
}

/// A checkbox with a label and one line of help; the whole row toggles.
pub fn checkbox(
    t: &Theme,
    id: &'static str,
    label: &'static str,
    help: &'static str,
    on: bool,
    on_click: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
) -> impl IntoElement {
    let handler: Handler = Box::new(on_click);
    div()
        .id(id)
        .debug_selector(move || id.to_owned())
        .flex()
        .items_start()
        .gap(px(10.))
        .py(px(6.))
        .cursor_pointer()
        .on_click(move |e, w, cx| handler(e, w, cx))
        .child(
            div()
                .flex()
                .flex_shrink_0()
                .items_center()
                .justify_center()
                .mt(px(1.))
                .size(px(18.))
                .rounded(px(5.))
                .border_1()
                .border_color(if on { t.accent } else { t.line })
                .bg(if on { t.accent } else { t.surface })
                .when(on, |d| d.child(icon("check", 13., t.accent_ink))),
        )
        .child(
            div()
                .flex()
                .flex_col()
                .gap(px(1.))
                .child(div().text_size(px(14.)).text_color(t.text).child(label))
                .child(div().text_size(px(12.5)).text_color(t.mute).child(help)),
        )
}

/// A row of mutually exclusive choices (stands in for a select).
pub fn segmented<V: Clone + PartialEq + 'static>(
    t: &Theme,
    id: &str,
    options: &[(V, &str)],
    selected: &V,
    enabled: bool,
    on_select: impl Fn(V, &mut Window, &mut App) + 'static,
) -> impl IntoElement {
    let on_select = std::rc::Rc::new(on_select);
    let mut row = div()
        .flex()
        .flex_shrink_0()
        .p(px(2.))
        .gap(px(2.))
        .rounded(px(R_ITEM))
        .bg(t.panel)
        .border_1()
        .border_color(t.line);
    for (i, (value, label)) in options.iter().enumerate() {
        let on = value == selected;
        let v = value.clone();
        let cb = on_select.clone();
        let mut seg = div()
            .id(SharedString::from(format!("{id}-{i}")))
            .debug_selector(|| format!("{id}-{i}"))
            .px(px(10.))
            .py(px(4.))
            .rounded(px(R_S))
            .text_size(px(13.))
            .whitespace_nowrap()
            .text_color(match (enabled, on) {
                (false, _) => t.mute,
                (true, true) => t.text,
                (true, false) => t.dim,
            })
            .when(on, |d| d.bg(if enabled { t.selected } else { t.hover }))
            .child(SharedString::from(label.to_string()));
        if enabled && !on {
            seg = seg.cursor_pointer().hover(|s| s.text_color(t.text)).on_click(move |_, w, cx| cb(v.clone(), w, cx));
        }
        row = row.child(seg);
    }
    row
}

/// A settings row: label + help on the left, the control on the right.
pub fn field(
    t: &Theme,
    label: impl Into<SharedString>,
    help: impl Into<SharedString>,
    control: impl IntoElement,
) -> impl IntoElement {
    div()
        .flex()
        .items_center()
        .gap(px(16.))
        .py(px(12.))
        .border_b_1()
        .border_color(t.line)
        .child(
            div()
                .flex()
                .flex_col()
                .flex_1()
                .min_w_0()
                .gap(px(3.))
                .child(div().text_size(px(14.)).text_color(t.text).child(label.into()))
                .child(div().text_size(px(12.5)).text_color(t.mute).child(help.into())),
        )
        .child(div().flex_shrink_0().child(control))
}

/// A settings row whose control is too wide to sit beside the text: label,
/// help, then the control underneath.
pub fn field_stacked(
    t: &Theme,
    label: impl Into<SharedString>,
    help: impl Into<SharedString>,
    control: impl IntoElement,
) -> impl IntoElement {
    div()
        .flex()
        .flex_col()
        .gap(px(3.))
        .py(px(12.))
        .border_b_1()
        .border_color(t.line)
        .child(div().text_size(px(14.)).text_color(t.text).child(label.into()))
        .child(div().text_size(px(12.5)).text_color(t.mute).child(help.into()))
        .child(div().flex().pt(px(8.)).child(control))
}

/// Section heading in the display face.
pub fn heading(t: &Theme, text: impl Into<SharedString>) -> impl IntoElement {
    div().font_family(FONT_DISPLAY).text_size(px(20.)).text_color(t.text).child(text.into())
}

/// A heading inside a pane.
pub fn subheading(t: &Theme, text: impl Into<SharedString>) -> impl IntoElement {
    div().text_size(px(15.)).text_color(t.text).child(text.into())
}

/// Secondary paragraph text.
pub fn para(t: &Theme, text: impl Into<SharedString>) -> impl IntoElement {
    div().text_size(px(13.5)).line_height(px(20.)).text_color(t.dim).child(text.into())
}

/// Inline data in the mono face.
pub fn mono(t: &Theme, text: impl Into<SharedString>) -> impl IntoElement {
    div().font_family(FONT_MONO).text_size(px(12.5)).text_color(t.dim).child(text.into())
}

/// A panel card.
pub fn card(t: &Theme) -> gpui::Div {
    div().flex().flex_col().p(px(16.)).gap(px(10.)).rounded(px(R_M)).bg(t.panel)
}

/// Key/value rows (`dl`).
/// Values that don't fit are cut with an ellipsis (paths, addresses).
pub fn kv(t: &Theme, rows: Vec<(&'static str, SharedString)>) -> impl IntoElement {
    let mut d = div().flex().flex_col().gap(px(6.)).flex_1().min_w_0();
    for (k, v) in rows {
        d = d.child(
            div()
                .flex()
                .gap(px(12.))
                .child(div().w(px(76.)).flex_shrink_0().text_size(px(13.)).text_color(t.mute).child(k))
                .child(div().flex_1().min_w_0().truncate().text_size(px(13.)).text_color(t.text).child(v)),
        );
    }
    d
}

/// The official idle Clawd at `cell` px per cell (whole cells, no smoothing).
pub fn clawd(cell: f32) -> impl IntoElement {
    div().flex_shrink_0().w(px(cell * IDLE_W as f32)).h(px(cell * IDLE_H as f32)).child(
        canvas(
            |_, _, _| (),
            move |bounds: Bounds<gpui::Pixels>, _, window, _| {
                for r in 0..IDLE_H {
                    for c in 0..IDLE_W {
                        let v = idle_cell(c, r);
                        if v == 0 {
                            continue;
                        }
                        let origin = bounds.origin + point(px(c as f32 * cell), px(r as f32 * cell));
                        window.paint_quad(fill(Bounds { origin, size: size(px(cell), px(cell)) }, rgb(cell_rgb(v))));
                    }
                }
            },
        )
        .size_full(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn usage_colours_follow_the_device() {
        // firmware/src/ui.cpp pct_color(): <50 green, <80 amber, else red.
        assert_eq!(usage_tone(0), Tone::Ok);
        assert_eq!(usage_tone(49), Tone::Ok);
        assert_eq!(usage_tone(50), Tone::Accent);
        assert_eq!(usage_tone(79), Tone::Accent);
        assert_eq!(usage_tone(80), Tone::Bad);
        assert_eq!(usage_tone(100), Tone::Bad);
    }
}
