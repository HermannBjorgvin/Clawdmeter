//! The app window: one GPUI process (`clawdmeter --window [page]`) with a
//! sidebar of pages and the first-run setup wizard. It exits when the window
//! closes; see [`crate::ipc`] for how it talks to the tray process.

pub mod app;
pub mod decorations;
pub mod flasher;
pub mod overview;
pub mod settings;
pub mod setup;
pub mod theme;
pub mod update;
pub mod widgets;

use std::borrow::Cow;
use std::time::Duration;

use clawdmeter_core::{unix_now, Paths};
use gpui::{
    canvas, div, point, prelude::*, px, size, App, Application, AssetSource, Bounds, CursorStyle, Decorations,
    HitboxBehavior, MouseButton, Pixels, Point, ResizeEdge, SharedString, Size, TitlebarOptions, Window, WindowBounds,
    WindowOptions,
};

use crate::ipc::{self, AppCommand, StatusSnapshot};
use crate::Page;
use decorations::Control;
use theme::{Theme, FONT_BODY};

/// The three faces the firmware fonts are generated from.
const FONTS: [&[u8]; 3] = [
    include_bytes!("../../../../../assets/TiemposText-400-Regular.otf"),
    include_bytes!("../../../../../assets/StyreneB-Regular.otf"),
    include_bytes!("../../../../../assets/DejaVuSansMono.ttf"),
];

/// How often the window re-reads the tray process's status.
pub const REFRESH: Duration = Duration::from_millis(500);

/// The window's size, and the smallest it can be made.
const SIZE: (f32, f32) = (880., 640.);
const MIN_SIZE: (f32, f32) = (720., 540.);

/// Images bundled into the binary, served by path. Board art goes through
/// `img()` (GPUI's resvg path, full colour); the Lucide icons through
/// `svg()`, which draws them as a mask in the text colour.
pub struct Assets;

macro_rules! bundled {
    ($($dir:literal: [$($name:literal),* $(,)?]),* $(,)?) => {
        &[$($((
            concat!($dir, "/", $name, ".svg"),
            include_bytes!(concat!("../../assets/", $dir, "/", $name, ".svg")),
        ),)*)*]
    };
}

const ASSETS: &[(&str, &[u8])] = bundled![
    "boards": [
        "waveshare_amoled_216",
        "waveshare_amoled_216_c6",
        "waveshare_amoled_18",
        "waveshare_amoled_18_c6",
        "waveshare_amoled_206",
        "waveshare_lcd_154",
        "waveshare_lcd_4",
        "waveshare_lcd_147",
        "waveshare_lcd_21",
        "lilygo_t4s3",
        "waveshare_lcd_183",
        "esp32_s3_4848s040",
        "waveshare_amoled_143",
        "waveshare_amoled_132",
        "esp32_2432s024c",
        "esp32_2432s024c_landscape",
        "esp32_2432s028r",
        "esp32_2432s028r_landscape",
        "m5sticks3",
        "m5stack_fire",
        "lilygo_t_display_s3",
        "waveshare_lcd_2",
        "waveshare_epaper_154",
        "xingzhi_cube_183",
        "freenove_s3_display_nontouch",
        "sensecap_indicator_d1l",
        "spotpear_s3_154",
    ],
    "icons": [
        "arrow-down-to-line",
        "gauge",
        "tablet",
        "zap",
        "users",
        "webhook",
        "settings",
        "rotate-ccw",
        "refresh-cw",
        "x",
        "minus",
        "square",
        "copy",
        "chevron-right",
        "chevron-down",
        "check",
        "mic",
        "keyboard",
        "arrow-right-to-line",
        "corner-down-left",
        "play",
        "skip-back",
        "skip-forward",
        "volume-1",
        "volume-2",
        "volume-x",
        "sun",
        "ban",
        "linux",
    ],
];

impl Assets {
    /// The bytes of a bundled asset.
    pub fn get(path: &str) -> Option<&'static [u8]> {
        ASSETS.iter().find(|(p, _)| *p == path).map(|(_, b)| *b)
    }
}

impl AssetSource for Assets {
    fn load(&self, path: &str) -> gpui::Result<Option<Cow<'static, [u8]>>> {
        Ok(Assets::get(path).map(Cow::Borrowed))
    }

    fn list(&self, path: &str) -> gpui::Result<Vec<SharedString>> {
        Ok(ASSETS.iter().filter(|(p, _)| p.starts_with(path)).map(|(p, _)| SharedString::from(*p)).collect())
    }
}

/// Options for the window.
#[derive(Debug, Clone, Default)]
pub struct WindowArgs {
    /// Flash page: a local release `manifest.json` to use instead of GitHub.
    pub manifest: Option<std::path::PathBuf>,
}

/// A child view asks the window to show another page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Navigate(pub Page);

/// Open the app window on `page` and run until it closes. If the window is
/// already open (another process holds the window lock), hand it the page
/// and return.
pub fn run(page: Option<Page>, paths: Paths, args: WindowArgs) -> i32 {
    let lock = match ipc::acquire_window_lock(&paths) {
        Ok(Some(f)) => Some(f),
        Ok(None) => {
            log::info!("the Clawdmeter window is already open; bringing it to the front");
            let _ = ipc::send_window_command(&paths, page.unwrap_or(Page::Overview));
            return 0;
        }
        Err(e) => {
            log::warn!("window lock unavailable ({e}); continuing");
            None
        }
    };
    // Requests aimed at a window that has since closed.
    let _ = ipc::take_window_commands(&paths);
    let page = match page {
        Some(p) => p,
        None if crate::onboarding::needed(&paths) => Page::Setup,
        None => Page::Overview,
    };
    open(move |window, cx| app::AppView::new(paths.clone(), page, args.manifest.clone(), window, cx));
    drop(lock);
    0
}

/// Run GPUI with the bundled fonts and images, open the app window with the
/// root `build` makes, and return when it closes.
pub fn open(build: impl FnOnce(&mut Window, &mut gpui::Context<app::AppView>) -> app::AppView + 'static) {
    open_with_height(None, build)
}

/// [`open`] with the window `height` px tall instead of the default (the
/// demo's `--height`, for screenshots of a whole page).
pub fn open_with_height(
    height: Option<f32>,
    build: impl FnOnce(&mut Window, &mut gpui::Context<app::AppView>) -> app::AppView + 'static,
) {
    let height = height.unwrap_or(SIZE.1).max(MIN_SIZE.1);
    let window_decorations = decorations::wanted();
    decorations::follow_button_layout(window_decorations);
    Application::new().with_assets(Assets).run(move |cx: &mut App| {
        if let Err(e) = cx.text_system().add_fonts(FONTS.iter().map(|f| Cow::Borrowed(*f)).collect()) {
            log::warn!("bundled fonts not registered ({e}); using system fonts");
        }
        // macOS keeps apps alive with no windows; this one is the window.
        cx.on_window_closed(|cx| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();
        decorations::await_button_layout(window_decorations);
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(SIZE.0), px(height)), cx))),
            titlebar: Some(TitlebarOptions { title: Some("Clawdmeter".into()), ..Default::default() }),
            app_id: Some("clawdmeter".into()),
            is_resizable: true,
            window_min_size: Some(size(px(MIN_SIZE.0), px(MIN_SIZE.1))),
            window_decorations: Some(window_decorations),
            ..Default::default()
        };
        if let Err(e) = cx.open_window(options, move |w, cx| cx.new(|cx| build(w, cx))) {
            log::error!("couldn't open the window: {e}");
            cx.quit();
        }
        cx.activate(true);
    });
}

/// Grab width for the resize edges of a client-drawn frame.
const RESIZE_EDGE: f32 = 6.;

fn resize_edge(pos: Point<Pixels>, size: Size<Pixels>) -> Option<ResizeEdge> {
    let e = px(RESIZE_EDGE);
    let (top, bottom) = (pos.y < e, pos.y > size.height - e);
    let (left, right) = (pos.x < e, pos.x > size.width - e);
    Some(match (top, bottom, left, right) {
        (true, _, true, _) => ResizeEdge::TopLeft,
        (true, _, _, true) => ResizeEdge::TopRight,
        (_, true, true, _) => ResizeEdge::BottomLeft,
        (_, true, _, true) => ResizeEdge::BottomRight,
        (true, ..) => ResizeEdge::Top,
        (_, true, ..) => ResizeEdge::Bottom,
        (_, _, true, _) => ResizeEdge::Left,
        (_, _, _, true) => ResizeEdge::Right,
        _ => return None,
    })
}

fn resize_cursor(edge: ResizeEdge) -> CursorStyle {
    match edge {
        ResizeEdge::Top | ResizeEdge::Bottom => CursorStyle::ResizeUpDown,
        ResizeEdge::Left | ResizeEdge::Right => CursorStyle::ResizeLeftRight,
        ResizeEdge::TopLeft | ResizeEdge::BottomRight => CursorStyle::ResizeUpLeftDownRight,
        ResizeEdge::TopRight | ResizeEdge::BottomLeft => CursorStyle::ResizeUpRightDownLeft,
    }
}

/// Height of our own header bar (Adwaita's).
pub(crate) const HEADER_H: f32 = 46.;

/// Gap between the header bar's round buttons (Adwaita's button padding and
/// spacing).
const BUTTON_GAP: f32 = 13.;

/// One round header-bar button, or `None` when the compositor says it can't
/// do what the button asks (gpui 0.2.2 always says it can). Maximize turns
/// into Restore while the window is maximized.
fn window_button(t: &Theme, window: &Window, control: Control) -> Option<impl IntoElement> {
    let can = window.window_controls();
    let (id, icon, size, tip) = match control {
        Control::Minimize if can.minimize => ("minimize", "minus", 16., "Minimize window"),
        Control::Maximize if can.maximize && window.is_maximized() => ("maximize", "copy", 13., "Restore window"),
        Control::Maximize if can.maximize => ("maximize", "square", 12., "Maximize window"),
        Control::Close => ("close", "x", 16., "Close window"),
        _ => return None,
    };
    let wash = |a: f32| {
        let mut c: gpui::Hsla = t.text.into();
        c.a = a;
        c
    };
    Some(
        div()
            .id(id)
            .flex()
            .items_center()
            .justify_center()
            .size(px(24.))
            .rounded(px(theme::R_PILL))
            .bg(wash(0.1))
            .cursor_pointer()
            .hover(|s| s.bg(wash(0.15)))
            .active(|s| s.bg(wash(0.3)))
            .tooltip(widgets::tooltip(tip))
            .child(widgets::icon(icon, size, t.text))
            .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
            .on_click(move |_, window, _| match control {
                Control::Minimize => window.minimize_window(),
                Control::Maximize => window.zoom_window(),
                Control::Close => window.remove_window(),
            }),
    )
}

/// The window frame: nothing of ours when the platform draws a title bar,
/// otherwise (GNOME on Wayland, see [`decorations`]) a header bar in the
/// Adwaita style (centred title, round buttons where the desktop's button
/// layout puts them, double-click to maximize, right-click for the window
/// menu) plus resize edges.
pub fn chrome(t: &Theme, window: &mut Window, cx: &mut App, body: impl IntoElement) -> impl IntoElement {
    let client = matches!(window.window_decorations(), Decorations::Client { .. });
    // Set by a press on the bar itself, not on a button (they stop the
    // press) or a resize edge: only that press may turn into a window move.
    let move_armed = window.use_keyed_state("titlebar-move-armed", cx, |_, _| false);
    let window: &Window = window;
    let buttons = |side: &[Control]| {
        div()
            .flex()
            .items_center()
            .gap(px(BUTTON_GAP))
            .children(side.iter().filter_map(|&c| window_button(t, window, c)))
    };
    div()
        .id("frame")
        // Every press clears the flag before the bar sees it (capture runs
        // parent-first), so a press that ended off the bar, or whose release
        // the compositor took, can't leave a later click armed.
        .capture_any_mouse_down({
            let armed = move_armed.clone();
            move |_, _, cx| armed.update(cx, |a, _| *a = false)
        })
        .relative()
        .size_full()
        .flex()
        .flex_col()
        .bg(t.surface)
        .text_color(t.text)
        .font_family(FONT_BODY)
        .text_size(px(14.))
        .when(client, |d| {
            let layout = decorations::button_layout();
            d.child(
                div()
                    .id("titlebar")
                    .relative()
                    .flex()
                    .flex_shrink_0()
                    .items_center()
                    .justify_between()
                    .h(px(HEADER_H))
                    .px(px(11.))
                    .border_b_1()
                    .border_color(t.line)
                    .bg(t.bg)
                    // Start the move on the first drag motion, as Zed's title
                    // bar does: starting it from the press didn't move the
                    // window on GNOME Wayland. A press on a button never
                    // moves the window, so a click that wobbles still lands.
                    .on_mouse_move({
                        let armed = move_armed.clone();
                        move |e, window, cx| {
                            if *armed.read(cx) && e.pressed_button == Some(MouseButton::Left) {
                                armed.update(cx, |a, _| *a = false);
                                window.start_window_move();
                            }
                        }
                    })
                    .on_mouse_down(MouseButton::Left, {
                        let armed = move_armed.clone();
                        move |e, window, cx| {
                            if e.click_count == 2 {
                                window.zoom_window();
                            }
                            let on_edge = resize_edge(e.position, window.window_bounds().get_bounds().size).is_some();
                            armed.update(cx, |a, _| *a = e.click_count == 1 && !on_edge);
                        }
                    })
                    .on_mouse_up(MouseButton::Left, move |_, _, cx| move_armed.update(cx, |a, _| *a = false))
                    .on_mouse_down(MouseButton::Right, |e, window, _| window.show_window_menu(e.position))
                    .child(
                        div()
                            .absolute()
                            .inset_0()
                            .flex()
                            .items_center()
                            .justify_center()
                            .font_weight(gpui::FontWeight::BOLD)
                            .text_size(px(14.))
                            .child("Clawdmeter"),
                    )
                    .child(buttons(&layout.left))
                    .child(buttons(&layout.right)),
            )
        })
        .child(div().flex_1().min_h_0().child(body))
        .when(client, |d| {
            // Resize edges: a cursor hint along the border, and a press there
            // starts an interactive resize.
            d.child(
                canvas(
                    |_, window, _| {
                        window.insert_hitbox(
                            Bounds::new(point(px(0.), px(0.)), window.window_bounds().get_bounds().size),
                            HitboxBehavior::Normal,
                        )
                    },
                    |_, hitbox, window, _| {
                        let size = window.window_bounds().get_bounds().size;
                        if let Some(edge) = resize_edge(window.mouse_position(), size) {
                            window.set_cursor_style(resize_cursor(edge), &hitbox);
                        }
                    },
                )
                .size_full()
                .absolute()
                .top_0()
                .left_0(),
            )
            .on_mouse_move(|_, window, _| window.refresh())
            .on_mouse_down(MouseButton::Left, |e, window, cx| {
                if let Some(edge) = resize_edge(e.position, window.window_bounds().get_bounds().size) {
                    window.start_window_resize(edge);
                    cx.stop_propagation();
                }
            })
        })
}

/// Read the latest status (None when the tray process isn't running).
pub fn live_status(paths: &Paths) -> Option<StatusSnapshot> {
    ipc::read_status(paths).filter(|s| s.alive(unix_now()))
}

/// Ask the tray process to do something (nothing happens when it isn't
/// running; the pages say so where it matters).
pub fn command(paths: &Paths, cmd: AppCommand) {
    if live_status(paths).is_none() {
        log::info!("{}: Clawdmeter isn't running", cmd.encode());
        return;
    }
    if let Err(e) = ipc::send_command(paths, &cmd) {
        log::warn!("command {}: {e}", cmd.encode());
    }
}

/// Text for "the daemon isn't running" banners.
pub fn not_running_text() -> SharedString {
    "Start Clawdmeter from your app menu, or run clawdmeter in a terminal.".into()
}

/// Run blocking work off the UI thread, then hand the result to the view.
pub fn run_blocking<V: 'static, R: Send + 'static>(
    cx: &mut gpui::Context<V>,
    work: impl FnOnce() -> R + Send + 'static,
    done: impl FnOnce(&mut V, R, &mut gpui::Context<V>) + 'static,
) {
    let task = cx.background_executor().spawn(async move { work() });
    cx.spawn(async move |this, cx| {
        let r = task.await;
        let _ = this.update(cx, |v, cx| {
            done(v, r, cx);
            cx.notify();
        });
    })
    .detach();
}
