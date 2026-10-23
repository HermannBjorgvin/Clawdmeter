//! The process's handle on the Bluetooth stack: one btleplug `Manager` and
//! one `Adapter`, shared by the reconnect loop and the wakeup watcher.
//!
//! btleplug expects a long-lived manager and adapter, and its stacks never
//! give back what creating them takes: on Linux every `Manager::new()` opens
//! a system D-Bus connection whose I/O task outlives the `Manager` (the bus
//! caps a user at 256 connections), and on macOS every `adapters()` call
//! starts a CoreBluetooth thread with its own `CBCentralManager` that never
//! exits. So both are made once and kept. They are replaced only on a real
//! failure:
//!
//! * the manager when listing fails because its D-Bus connection is gone
//!   (the system bus restarted). Every other error keeps it: a stopped or
//!   hung bluetoothd answers or times out over a live connection, and a new
//!   connection to the same bus wouldn't help;
//! * the adapter, on Linux and Windows (where listing is cheap), when it
//!   reads as off: BlueZ reports an unplugged adapter as off, so the next one
//!   is picked up. On macOS off just means off.
//!
//! The wakeup watcher reopens its event stream whenever either is replaced
//! ([`Bluetooth::replaced`]); BlueZ streams never end on their own.

use std::sync::Arc;

use btleplug::api::{Central, CentralState, Manager as _};
use btleplug::platform::{Adapter, Manager};
use tokio::sync::{watch, Mutex};

use crate::LinkError;

/// The shared stack handle; clones share one manager and adapter. On Linux
/// the D-Bus connection's I/O task runs on the runtime of the first
/// [`Bluetooth::adapter`] call, so make that call from a runtime that lives
/// as long as the handle (the daemon's).
#[derive(Clone)]
pub struct Bluetooth {
    shared: Arc<Shared<Btleplug>>,
}

/// A handle with nothing created yet; the first [`Bluetooth::adapter`] call
/// creates the manager and adapter.
impl Default for Bluetooth {
    fn default() -> Self {
        Bluetooth { shared: Arc::new(Shared::new(Btleplug)) }
    }
}

impl Bluetooth {
    /// The adapter, if it is powered.
    pub async fn adapter(&self) -> Result<Adapter, LinkError> {
        self.shared.adapter().await
    }

    /// The adapter and the [`Replacements`] count it belongs to.
    pub(crate) async fn adapter_at(&self) -> Result<(Adapter, Replacements), LinkError> {
        self.shared.adapter_at().await
    }

    /// Changes whenever the manager or adapter is replaced.
    pub(crate) fn replaced(&self) -> watch::Receiver<Replacements> {
        self.shared.generation.subscribe()
    }
}

/// How often the manager and the adapter have been replaced.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub(crate) struct Replacements {
    /// Managers dropped because their D-Bus connection was gone.
    pub(crate) lost_connections: u64,
    /// Adapters listed and kept.
    pub(crate) adapters: u64,
}

/// A failed adapter listing.
struct ListError {
    message: String,
    /// The manager's D-Bus connection is gone.
    connection_lost: bool,
}

/// What [`Shared`] needs from a Bluetooth stack (btleplug, or a fake in
/// tests).
trait Stack {
    type Manager: Send + Sync;
    type Adapter: Clone + Send + Sync;
    /// Listing adapters costs nothing lasting, so an adapter that reads as
    /// off is listed again in case it was unplugged.
    fn relist_when_off(&self) -> bool;
    async fn manager(&self) -> Result<Self::Manager, String>;
    async fn adapters(&self, manager: &Self::Manager) -> Result<Vec<Self::Adapter>, ListError>;
    async fn state(&self, adapter: &Self::Adapter) -> CentralState;
}

struct Btleplug;

impl Stack for Btleplug {
    type Manager = Manager;
    type Adapter = Adapter;

    fn relist_when_off(&self) -> bool {
        !cfg!(target_os = "macos")
    }

    async fn manager(&self) -> Result<Manager, String> {
        Manager::new().await.map_err(|e| e.to_string())
    }

    async fn adapters(&self, manager: &Manager) -> Result<Vec<Adapter>, ListError> {
        manager.adapters().await.map_err(|e| ListError { message: e.to_string(), connection_lost: connection_lost(&e) })
    }

    /// btleplug 0.13 can't fail this: BlueZ reports any error as off, WinRT
    /// can't fail, and CoreBluetooth only if its thread died (the thread
    /// never ends otherwise).
    async fn state(&self, adapter: &Adapter) -> CentralState {
        adapter.adapter_state().await.unwrap_or(CentralState::Unknown)
    }
}

/// Is this listing error the manager's D-Bus connection being gone? btleplug
/// boxes bluez-async's error into `Error::Other`; its D-Bus error carries the
/// name. `Disconnected`, or dbus-rs's own "Failed to send message" (what a
/// dead connection returns once its first calls have timed out), mean gone.
/// Anything else came back over a live bus (`ServiceUnknown`: bluetoothd
/// isn't running) or timed out on one.
#[cfg(target_os = "linux")]
fn connection_lost(e: &btleplug::Error) -> bool {
    let btleplug::Error::Other(inner) = e else { return false };
    let Some(bluez_async::BluetoothError::DbusError(d)) = inner.downcast_ref::<bluez_async::BluetoothError>() else {
        return false;
    };
    match d.name() {
        Some("org.freedesktop.DBus.Error.Disconnected") => true,
        Some("org.freedesktop.DBus.Error.Failed") => d.message() == Some("Failed to send message"),
        _ => false,
    }
}

/// Elsewhere the manager holds nothing to lose.
#[cfg(not(target_os = "linux"))]
fn connection_lost(_: &btleplug::Error) -> bool {
    false
}

struct Shared<S: Stack> {
    stack: S,
    slot: Mutex<Slot<S>>,
    generation: watch::Sender<Replacements>,
}

struct Slot<S: Stack> {
    manager: Option<Arc<S::Manager>>,
    adapter: Option<Arc<S::Adapter>>,
}

impl<S: Stack> Shared<S> {
    fn new(stack: S) -> Self {
        let slot = Slot { manager: None, adapter: None };
        Shared { stack, slot: Mutex::new(slot), generation: watch::Sender::new(Replacements::default()) }
    }

    async fn adapter(&self) -> Result<S::Adapter, LinkError> {
        self.adapter_at().await.map(|(a, _)| a)
    }

    /// The adapter, with the [`Replacements`] count read together with it
    /// (under the lock), so the watcher can tell whether it is still current.
    async fn adapter_at(&self) -> Result<(S::Adapter, Replacements), LinkError> {
        let (kept, current) = {
            let slot = self.slot.lock().await;
            (slot.adapter.clone(), *self.generation.borrow())
        };
        if let Some(a) = &kept {
            match self.stack.state(a).await {
                CentralState::PoweredOff if !self.stack.relist_when_off() => return Err(powered_off()),
                CentralState::PoweredOff => {}
                _ => return Ok((S::Adapter::clone(a), current)),
            }
        }
        let (adapter, current) = self.relist(kept.as_ref()).await?;
        if self.stack.state(&adapter).await == CentralState::PoweredOff {
            return Err(powered_off());
        }
        Ok((S::Adapter::clone(&adapter), current))
    }

    /// Replace `stale` (the kept adapter the caller found off, if any) with
    /// the first one listed. Holds the lock throughout, so lookups that fail
    /// together make one adapter between them.
    async fn relist(&self, stale: Option<&Arc<S::Adapter>>) -> Result<(Arc<S::Adapter>, Replacements), LinkError> {
        let mut slot = self.slot.lock().await;
        if let Some(a) = &slot.adapter {
            if !stale.is_some_and(|s| Arc::ptr_eq(s, a)) {
                // Another lookup listed one while this one waited.
                return Ok((Arc::clone(a), *self.generation.borrow()));
            }
        }
        slot.adapter = None;
        let manager = match &slot.manager {
            Some(m) => Arc::clone(m),
            None => {
                let m = Arc::new(self.stack.manager().await.map_err(LinkError::NoAdapter)?);
                slot.manager = Some(Arc::clone(&m));
                m
            }
        };
        let list = match self.stack.adapters(&manager).await {
            Ok(list) => list,
            Err(e) => {
                if e.connection_lost {
                    log::info!("Bluetooth: lost the system bus connection ({}); reconnecting", e.message);
                    slot.manager = None;
                    self.generation.send_modify(|g| g.lost_connections += 1);
                }
                return Err(LinkError::NoAdapter(e.message));
            }
        };
        let a = Arc::new(list.into_iter().next().ok_or_else(|| LinkError::NoAdapter("none present".into()))?);
        slot.adapter = Some(Arc::clone(&a));
        self.generation.send_modify(|g| g.adapters += 1);
        Ok((a, *self.generation.borrow()))
    }
}

fn powered_off() -> LinkError {
    LinkError::NoAdapter("adapter is off".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    /// Counts the managers and listings made. `adapters` lists the plugged-in
    /// adapters as (id, powered); like btleplug on Linux and Windows, an
    /// adapter that is gone reads as off. `manager_failures` makes the first
    /// managers fail (no system bus yet); `fail` makes listings fail, on the
    /// managers made so far if the connection is lost.
    #[derive(Default)]
    struct Fake {
        relist_when_off: bool,
        managers: AtomicUsize,
        lists: AtomicUsize,
        manager_failures: AtomicUsize,
        adapters: std::sync::Mutex<Vec<(usize, bool)>>,
        /// (connection lost, up to which manager).
        fail: std::sync::Mutex<Option<(bool, usize)>>,
    }

    impl Fake {
        /// Linux / Windows.
        fn new(adapters: &[(usize, bool)]) -> Self {
            Fake { relist_when_off: true, adapters: std::sync::Mutex::new(adapters.to_vec()), ..Fake::default() }
        }

        /// macOS: off is just off, and listing is what leaks.
        fn mac(adapters: &[(usize, bool)]) -> Self {
            Fake { relist_when_off: false, ..Fake::new(adapters) }
        }

        fn plug(&self, adapters: &[(usize, bool)]) {
            *self.adapters.lock().unwrap() = adapters.to_vec();
        }

        /// Listings fail: with the connection lost (on the managers made so
        /// far), or with an error over a live bus (on every manager).
        fn fail(&self, connection_lost: Option<bool>) {
            let upto = |lost| if lost { self.managers.load(Ordering::SeqCst) } else { usize::MAX };
            *self.fail.lock().unwrap() = connection_lost.map(|lost| (lost, upto(lost)));
        }
    }

    impl Stack for Fake {
        /// The manager's generation (1 = the first made).
        type Manager = usize;
        type Adapter = usize;

        fn relist_when_off(&self) -> bool {
            self.relist_when_off
        }

        async fn manager(&self) -> Result<usize, String> {
            if self.manager_failures.load(Ordering::SeqCst) > 0 {
                self.manager_failures.fetch_sub(1, Ordering::SeqCst);
                return Err("no system bus".into());
            }
            Ok(self.managers.fetch_add(1, Ordering::SeqCst) + 1)
        }

        async fn adapters(&self, manager: &usize) -> Result<Vec<usize>, ListError> {
            // Let a concurrent lookup reach the lock before this one returns.
            tokio::task::yield_now().await;
            self.lists.fetch_add(1, Ordering::SeqCst);
            match *self.fail.lock().unwrap() {
                Some((connection_lost, upto)) if *manager <= upto => {
                    Err(ListError { message: "listing failed".into(), connection_lost })
                }
                _ => Ok(self.adapters.lock().unwrap().iter().map(|a| a.0).collect()),
            }
        }

        async fn state(&self, adapter: &usize) -> CentralState {
            tokio::task::yield_now().await;
            let powered = self.adapters.lock().unwrap().iter().any(|a| a == &(*adapter, true));
            if powered {
                CentralState::PoweredOn
            } else {
                CentralState::PoweredOff
            }
        }
    }

    fn is_off(r: Result<usize, LinkError>) -> bool {
        matches!(r, Err(LinkError::NoAdapter(s)) if s == "adapter is off")
    }

    fn managers(s: &Shared<Fake>) -> usize {
        s.stack.managers.load(Ordering::SeqCst)
    }

    fn lists(s: &Shared<Fake>) -> usize {
        s.stack.lists.load(Ordering::SeqCst)
    }

    #[tokio::test]
    async fn reconnects_reuse_one_manager_and_adapter() {
        for s in [Shared::new(Fake::new(&[(0, true)])), Shared::new(Fake::mac(&[(0, true)]))] {
            for _ in 0..50 {
                assert_eq!(s.adapter().await.ok(), Some(0));
            }
            assert_eq!((managers(&s), lists(&s)), (1, 1));
        }
    }

    #[tokio::test]
    async fn concurrent_first_lookups_make_one_adapter() {
        let s = Shared::new(Fake::mac(&[(0, true)]));
        let (a, b) = tokio::join!(s.adapter(), s.adapter());
        assert_eq!((a.ok(), b.ok()), (Some(0), Some(0)));
        assert_eq!((managers(&s), lists(&s)), (1, 1));
    }

    #[tokio::test]
    async fn bluetooth_off_on_macos_keeps_the_adapter() {
        let s = Shared::new(Fake::mac(&[(0, false)]));
        for _ in 0..50 {
            assert!(is_off(s.adapter().await));
        }
        s.stack.plug(&[(0, true)]);
        assert_eq!(s.adapter().await.ok(), Some(0));
        assert_eq!((managers(&s), lists(&s)), (1, 1));
    }

    #[tokio::test]
    async fn bluetooth_off_on_linux_keeps_the_manager() {
        let s = Shared::new(Fake::new(&[(0, false)]));
        for _ in 0..50 {
            assert!(is_off(s.adapter().await));
        }
        s.stack.plug(&[(0, true)]);
        assert_eq!(s.adapter().await.ok(), Some(0));
        assert_eq!(managers(&s), 1);
        // Back on: no more listings.
        let before = lists(&s);
        assert_eq!(s.adapter().await.ok(), Some(0));
        assert_eq!(lists(&s), before);
    }

    #[tokio::test]
    async fn no_adapter_asks_the_same_manager_again() {
        let s = Shared::new(Fake::new(&[]));
        for _ in 0..50 {
            assert!(matches!(s.adapter().await, Err(LinkError::NoAdapter(_))));
        }
        s.stack.plug(&[(7, true)]);
        assert_eq!(s.adapter().await.ok(), Some(7));
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn a_failed_manager_is_retried_until_one_exists() {
        let s = Shared::new(Fake::new(&[(0, true)]));
        s.stack.manager_failures.store(3, Ordering::SeqCst);
        for _ in 0..3 {
            assert!(matches!(s.adapter().await, Err(LinkError::NoAdapter(_))));
        }
        for _ in 0..10 {
            assert_eq!(s.adapter().await.ok(), Some(0));
        }
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn an_unplugged_adapter_gives_way_to_the_next() {
        let s = Shared::new(Fake::new(&[(0, true), (1, true)]));
        assert_eq!(s.adapter().await.ok(), Some(0));
        s.stack.plug(&[(1, true)]);
        assert_eq!(s.adapter().await.ok(), Some(1));
        assert_eq!(s.adapter().await.ok(), Some(1));
        assert_eq!((managers(&s), lists(&s)), (1, 2));
    }

    #[tokio::test]
    async fn the_lookup_says_which_generation_it_belongs_to() {
        let s = Shared::new(Fake::new(&[(0, true), (1, true)]));
        let replaced = s.generation.subscribe();
        let (a, at) = s.adapter_at().await.unwrap();
        assert_eq!((a, at), (0, *s.generation.borrow()));
        for _ in 0..10 {
            assert_eq!(s.adapter_at().await.ok(), Some((0, at)));
        }
        s.stack.plug(&[(1, true)]);
        let (a, at) = s.adapter_at().await.unwrap();
        assert_eq!((a, at), (1, *s.generation.borrow()));
        assert_eq!((at.adapters, at.lost_connections), (2, 0));
        assert!(replaced.has_changed().unwrap());
    }

    #[tokio::test]
    async fn errors_over_a_live_bus_keep_the_manager() {
        // bluetoothd stopped (ServiceUnknown) or hung (timeouts).
        let s = Shared::new(Fake::new(&[]));
        s.stack.fail(Some(false));
        for _ in 0..50 {
            assert!(matches!(s.adapter().await, Err(LinkError::NoAdapter(_))));
        }
        s.stack.fail(None);
        s.stack.plug(&[(0, true)]);
        assert_eq!(s.adapter().await.ok(), Some(0));
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn a_lost_connection_replaces_the_manager_once() {
        // The bus went away while the adapter was off (on Linux a dead
        // connection also reads as off), so lookups list again and fail.
        let s = Shared::new(Fake::new(&[(0, false)]));
        assert!(is_off(s.adapter().await));
        s.stack.fail(Some(true));
        // The watcher and the reconnect loop both hit the dead connection.
        let (a, b) = tokio::join!(s.adapter(), s.adapter());
        assert!(a.is_err() && b.is_err());
        assert_eq!(s.generation.borrow().lost_connections, 1);
        s.stack.plug(&[(0, true)]);
        for _ in 0..50 {
            assert_eq!(s.adapter().await.ok(), Some(0));
        }
        assert_eq!(managers(&s), 2);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn lost_connections() {
        // Wrapped as btleplug wraps them: bluez-async's error, boxed.
        let dbus_err = |name: &str, msg: &str| {
            connection_lost(&btleplug::Error::from(bluez_async::BluetoothError::DbusError(dbus::Error::new_custom(
                name, msg,
            ))))
        };
        assert!(dbus_err("org.freedesktop.DBus.Error.Failed", "Failed to send message"));
        assert!(dbus_err("org.freedesktop.DBus.Error.Disconnected", "Connection is closed"));
        assert!(!dbus_err("org.freedesktop.DBus.Error.ServiceUnknown", "The name org.bluez was not provided"));
        assert!(!dbus_err("org.bluez.Error.Failed", "Failed to send message"));
        assert!(!dbus_err("org.freedesktop.DBus.Error.Timeout", "Timeout waiting for reply"));
        assert!(!dbus_err("org.freedesktop.DBus.Error.NoReply", "Message recipient disconnected"));
        assert!(!dbus_err("org.freedesktop.DBus.Error.Failed", "Out of memory"));
        assert!(!connection_lost(&btleplug::Error::from(bluez_async::BluetoothError::NoBluetoothAdapters)));
        assert!(!connection_lost(&btleplug::Error::NoAdapterAvailable));
    }
}
