//! Is there a tray to put an icon in? GNOME only shows StatusNotifierItems
//! with the AppIndicator extension, which provides the watcher service.

use std::time::Duration;

use dbus::blocking::Connection;

/// The well-known name the tray host registers.
pub const WATCHER: &str = "org.kde.StatusNotifierWatcher";

/// True when something on the session bus owns the watcher name.
pub fn watcher_present() -> bool {
    let Ok(conn) = Connection::new_session() else { return false };
    let proxy = conn.with_proxy("org.freedesktop.DBus", "/org/freedesktop/DBus", Duration::from_secs(2));
    proxy
        .method_call::<(bool,), _, _, _>("org.freedesktop.DBus", "NameHasOwner", (WATCHER,))
        .map(|(b,)| b)
        .unwrap_or(false)
}
