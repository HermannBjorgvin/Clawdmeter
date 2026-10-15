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

use gpui::WindowDecorations;

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
