//! Native window frames where the platform draws them.
//!
//! GPUI 0.2.2 asks for server decorations by default: a native title bar on
//! macOS and Windows (`TitlebarOptions::appears_transparent` false), the
//! window manager's frame on X11 (`_MOTIF_WM_HINTS`), and on Wayland the
//! compositor's frame through `zxdg_decoration_manager_v1` when the
//! compositor offers it (KDE, Sway, Hyprland, …). A compositor without that
//! protocol (GNOME's Mutter, Weston) draws no frame, but gpui still records
//! the request as server-side, so `Window::window_decorations()` can't tell
//! us. [`wanted`] asks the compositor itself before the window opens and
//! picks client decorations there; the app then draws its own header bar
//! ([`super::chrome`]). A compositor that has the protocol yet answers
//! client-side flips gpui to `Decorations::Client`, which `chrome` reads at
//! render time, so that case draws our bar too.
//!
//! Our bar's buttons follow the desktop's layout, as a GTK app's do:
//! GNOME's `button-layout`, followed through the portal on a background
//! thread ([`follow_button_layout`]), so a change in GNOME Tweaks shows at
//! once.

use std::sync::{Arc, LazyLock, OnceLock};

use gpui::WindowDecorations;

/// A window button the header bar can show.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Control {
    /// Minimize the window.
    Minimize,
    /// Maximize the window, or restore it when it's maximized.
    Maximize,
    /// Close the window.
    Close,
}

/// The header bar's buttons at each end.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ButtonLayout {
    /// At the left end, left to right.
    pub left: Vec<Control>,
    /// At the right end, left to right.
    pub right: Vec<Control>,
}

impl Default for ButtonLayout {
    /// GTK's default (`menu:minimize,maximize,close`), for a desktop that
    /// doesn't say. GNOME's own default, `appmenu:close`, comes from the
    /// portal.
    fn default() -> Self {
        ButtonLayout { left: Vec::new(), right: vec![Control::Minimize, Control::Maximize, Control::Close] }
    }
}

impl ButtonLayout {
    /// From a GNOME `button-layout` (GTK `gtk-decoration-layout`) string:
    /// the names before the colon go on the left, the rest on the right.
    /// Spaces around names are ignored. Names other than minimize, maximize
    /// and close (`appmenu`, `icon`, `menu`, `spacer`) are skipped, and so
    /// are repeats, as Mutter does.
    pub fn parse(layout: &str) -> Self {
        let (left, right) = layout.split_once(':').unwrap_or((layout, ""));
        let mut seen = Vec::new();
        let mut side = |names: &str| -> Vec<Control> {
            names
                .split(',')
                .filter_map(|name| match name.trim() {
                    "minimize" => Some(Control::Minimize),
                    "maximize" => Some(Control::Maximize),
                    "close" => Some(Control::Close),
                    _ => None,
                })
                .filter(|c| {
                    if seen.contains(c) {
                        return false;
                    }
                    seen.push(*c);
                    true
                })
                .collect()
        };
        let left = side(left);
        ButtonLayout { left, right: side(right) }
    }
}

/// A GNOME `button-layout` string that replaces the desktop's, so reference
/// screenshots come out the same on any machine. Empty counts as unset
/// (`:` is the layout with no buttons).
const LAYOUT_ENV: &str = "CLAWDMETER_BUTTON_LAYOUT";

/// How long the window waits for the portal's first answer before opening
/// with GTK's default: a running portal answers in milliseconds; one still
/// starting at login answers later, and the bar changes then.
#[cfg(target_os = "linux")]
const FIRST_ANSWER: std::time::Duration = std::time::Duration::from_millis(250);

static DEFAULT: LazyLock<Arc<ButtonLayout>> = LazyLock::new(|| Arc::new(ButtonLayout::default()));

/// Where the bar's layout comes from.
enum Source {
    /// [`LAYOUT_ENV`], or GTK's default where there's no portal.
    Fixed(Arc<ButtonLayout>),
    /// GNOME's setting, kept current.
    #[cfg(target_os = "linux")]
    Portal(clawdmeter_core::platform::SettingWatch<Arc<ButtonLayout>>),
}

static SOURCE: OnceLock<Source> = OnceLock::new();

/// Start following the desktop's button layout, for a window about to open
/// with `decorations`. The portal is followed wherever our bar can show:
/// client decorations, or any Wayland compositor, since one that offers
/// server decorations may still answer client-side. Call it before GPUI
/// starts, so the first portal read overlaps the start-up;
/// [`await_button_layout`] then waits for it.
pub fn follow_button_layout(decorations: WindowDecorations) {
    SOURCE.get_or_init(|| {
        if let Some(fixed) = std::env::var(LAYOUT_ENV).ok().filter(|v| !v.trim().is_empty()) {
            log::info!("header bar buttons: {fixed:?} from {LAYOUT_ENV}");
            return Source::Fixed(Arc::new(ButtonLayout::parse(&fixed)));
        }
        #[cfg(target_os = "linux")]
        if matches!(decorations, WindowDecorations::Client) || gpui::guess_compositor() == "Wayland" {
            return Source::Portal(clawdmeter_core::platform::SettingWatch::spawn(
                "org.gnome.desktop.wm.preferences",
                "button-layout",
                |read| {
                    log::debug!("header bar buttons: {read:?} from the portal (None: GTK's default)");
                    Arc::new(read.map(ButtonLayout::parse).unwrap_or_default())
                },
            ));
        }
        #[cfg(not(target_os = "linux"))]
        let _ = decorations;
        Source::Fixed(DEFAULT.clone())
    });
}

/// Wait up to `FIRST_ANSWER` (250 ms) for the portal's first answer, so the
/// window usually opens with the right buttons. Only a window that asks for
/// client decorations waits: elsewhere the bar shows only if the compositor
/// insists, and the answer is drawn when it lands.
pub fn await_button_layout(decorations: WindowDecorations) {
    #[cfg(target_os = "linux")]
    if let (WindowDecorations::Client, Some(Source::Portal(watch))) = (decorations, SOURCE.get()) {
        match watch.wait(FIRST_ANSWER) {
            Some(layout) => log::info!("header bar buttons: {layout:?}"),
            None => log::info!("header bar buttons: no portal answer yet; GTK's default until it comes"),
        }
    }
    #[cfg(not(target_os = "linux"))]
    let _ = decorations;
}

/// The layout to draw now: the override, the portal's latest answer, or
/// GTK's default.
pub fn button_layout() -> Arc<ButtonLayout> {
    match SOURCE.get() {
        Some(Source::Fixed(layout)) => layout.clone(),
        #[cfg(target_os = "linux")]
        Some(Source::Portal(watch)) => watch.get().unwrap_or_else(|| DEFAULT.clone()),
        None => DEFAULT.clone(),
    }
}

/// The decorations to ask for. `CLAWDMETER_DECORATIONS=client|server`
/// overrides the choice (screenshots of our header bar under X11).
pub fn wanted() -> WindowDecorations {
    match std::env::var("CLAWDMETER_DECORATIONS").as_deref() {
        Ok("client") => return WindowDecorations::Client,
        Ok("server") => return WindowDecorations::Server,
        _ => {}
    }
    #[cfg(target_os = "linux")]
    if gpui::guess_compositor() == "Wayland" && wayland::server_decorations() == Some(false) {
        return WindowDecorations::Client;
    }
    WindowDecorations::Server
}

#[cfg(target_os = "linux")]
mod wayland {
    use wayland_client::protocol::wl_registry;
    use wayland_client::{Connection, Dispatch, QueueHandle};

    /// The xdg-decoration global.
    const DECORATION_MANAGER: &str = "zxdg_decoration_manager_v1";

    #[derive(Default)]
    struct Globals(Vec<String>);

    impl Dispatch<wl_registry::WlRegistry, ()> for Globals {
        fn event(
            state: &mut Self,
            _: &wl_registry::WlRegistry,
            event: wl_registry::Event,
            _: &(),
            _: &Connection,
            _: &QueueHandle<Self>,
        ) {
            if let wl_registry::Event::Global { interface, .. } = event {
                state.0.push(interface);
            }
        }
    }

    /// Whether the compositor offers server-side decorations; `None` when
    /// it can't be asked (gpui will fail to connect too, or fall back to X11).
    pub fn server_decorations() -> Option<bool> {
        let conn = Connection::connect_to_env().ok()?;
        let mut queue = conn.new_event_queue();
        conn.display().get_registry(&queue.handle(), ());
        let mut globals = Globals::default();
        queue.roundtrip(&mut globals).ok()?;
        let has = globals.0.iter().any(|g| g == DECORATION_MANAGER);
        log::info!("wayland: {} server-side decorations", if has { "compositor offers" } else { "no" });
        Some(has)
    }
}

#[cfg(test)]
mod tests {
    use super::Control::{Close, Maximize, Minimize};
    use super::*;

    fn layout(left: &[Control], right: &[Control]) -> ButtonLayout {
        ButtonLayout { left: left.to_vec(), right: right.to_vec() }
    }

    #[test]
    fn button_layouts() {
        assert_eq!(ButtonLayout::parse("appmenu:close"), layout(&[], &[Close]));
        assert_eq!(ButtonLayout::parse(":minimize,maximize,close"), layout(&[], &[Minimize, Maximize, Close]));
        assert_eq!(ButtonLayout::parse("close,minimize,maximize:appmenu"), layout(&[Close, Minimize, Maximize], &[]));
        assert_eq!(ButtonLayout::parse("icon,spacer,minimize:close"), layout(&[Minimize], &[Close]));
        assert_eq!(ButtonLayout::parse(" minimize , close :"), layout(&[Minimize, Close], &[]));
        // No colon: everything is on the left, as GTK reads it.
        assert_eq!(ButtonLayout::parse("close"), layout(&[Close], &[]));
        // A button shows once, where it's first named.
        assert_eq!(ButtonLayout::parse("close:minimize,close,minimize"), layout(&[Close], &[Minimize]));
        // An empty layout has no buttons, as GNOME draws it.
        assert_eq!(ButtonLayout::parse(""), layout(&[], &[]));
        assert_eq!(ButtonLayout::parse(":"), layout(&[], &[]));
    }

    #[test]
    fn unread_layout_is_gtk_default() {
        assert_eq!(ButtonLayout::default(), ButtonLayout::parse("menu:minimize,maximize,close"));
    }
}
