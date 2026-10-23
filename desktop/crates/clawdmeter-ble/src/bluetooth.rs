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
//! * the manager when its D-Bus connection is gone (the system bus
//!   restarted); errors on a live connection, such as bluetoothd not
//!   running, keep it;
//! * the adapter when it can't be reached, or (Linux and Windows, where
//!   listing is cheap) when it reads as off; BlueZ reports an unplugged
//!   adapter as off, so the next one is picked up.
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

    /// Changes whenever the manager or adapter is replaced.
    pub(crate) fn replaced(&self) -> watch::Receiver<Replacements> {
        self.shared.generation.subscribe()
    }
}

/// How often the manager and the adapter have been replaced.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub(crate) struct Replacements {
    /// Managers dropped because their D-Bus connection was gone.
    pub(crate) lost_managers: u64,
    /// Adapters listed and kept.
    pub(crate) adapters: u64,
}

/// A failed adapter listing.
struct ListError {
    message: String,
    /// The manager's connection is gone; a new manager is needed.
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
    async fn state(&self, adapter: &Self::Adapter) -> Result<CentralState, String>;
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

    async fn state(&self, adapter: &Adapter) -> Result<CentralState, String> {
        adapter.adapter_state().await.map_err(|e| e.to_string())
    }
}

/// True when a BlueZ call failed because the D-Bus connection itself is gone.
/// btleplug hands bluez-async's error over boxed, so this matches the D-Bus
/// error in its `Debug` text (`… <message> (<error name>)`). Only two cases
/// count: `Disconnected`, and dbus-rs's own "Failed to send message", which
/// it returns once the connection's I/O task has ended. `Timeout` and
/// `NoReply` also come from a hung or crashing bluetoothd on a live bus, and
/// `ServiceUnknown` means bluetoothd isn't running; replacing the manager on
/// those would open a new connection per retry.
fn connection_lost(e: &btleplug::Error) -> bool {
    let text = format!("{e:?}");
    text.contains("(org.freedesktop.DBus.Error.Disconnected)")
        || text.contains("Failed to send message (org.freedesktop.DBus.Error.Failed)")
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
        Shared {
            stack,
            slot: Mutex::new(Slot { manager: None, adapter: None }),
            generation: watch::Sender::new(Replacements::default()),
        }
    }

    async fn adapter(&self) -> Result<S::Adapter, LinkError> {
        let kept = self.slot.lock().await.adapter.clone();
        if let Some(a) = &kept {
            match self.stack.state(a).await {
                Ok(CentralState::PoweredOff) if !self.stack.relist_when_off() => return Err(powered_off()),
                Ok(CentralState::PoweredOff) => {}
                Ok(_) => return Ok(S::Adapter::clone(a)),
                Err(e) => log::debug!("Bluetooth adapter unreachable ({e}); listing adapters again"),
            }
        }
        let adapter = self.relist(kept.as_ref()).await?;
        if let Ok(CentralState::PoweredOff) = self.stack.state(&adapter).await {
            return Err(powered_off());
        }
        Ok(S::Adapter::clone(&adapter))
    }

    /// Replace `stale` (the kept adapter the caller found unusable, if any)
    /// with the first one listed. Holds the lock throughout, so lookups that
    /// fail together make one adapter between them.
    async fn relist(&self, stale: Option<&Arc<S::Adapter>>) -> Result<Arc<S::Adapter>, LinkError> {
        let mut slot = self.slot.lock().await;
        if let Some(a) = &slot.adapter {
            if !stale.is_some_and(|s| Arc::ptr_eq(s, a)) {
                // Another lookup listed one while this one waited.
                return Ok(Arc::clone(a));
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
        match self.stack.adapters(&manager).await {
            Ok(list) => {
                let a = Arc::new(list.into_iter().next().ok_or_else(|| LinkError::NoAdapter("none present".into()))?);
                slot.adapter = Some(Arc::clone(&a));
                self.generation.send_modify(|g| g.adapters += 1);
                Ok(a)
            }
            Err(e) => {
                if e.connection_lost {
                    log::info!("Bluetooth: lost the system bus connection ({}); reconnecting", e.message);
                    slot.manager = None;
                    self.generation.send_modify(|g| g.lost_managers += 1);
                }
                Err(LinkError::NoAdapter(e.message))
            }
        }
    }
}

fn powered_off() -> LinkError {
    LinkError::NoAdapter("adapter is off".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

    /// A list failure the fake can be told to return.
    #[derive(Clone, Copy, PartialEq)]
    enum Fail {
        /// The bus connection is gone.
        Lost,
        /// bluetoothd isn't running (on a live connection).
        NoService,
    }

    /// Counts the managers and listings made. `adapters` lists the plugged-in
    /// adapters as (id, powered); like btleplug on Linux and Windows, an
    /// adapter that is gone reads as off. `unreachable` makes `state` fail
    /// (macOS: the CoreBluetooth thread is gone). `manager_failures` makes
    /// the first managers fail (no system bus yet); `fail` makes listings on
    /// the managers made so far fail.
    #[derive(Default)]
    struct Fake {
        relist_when_off: bool,
        managers: AtomicUsize,
        lists: AtomicUsize,
        manager_failures: AtomicUsize,
        unreachable: AtomicBool,
        adapters: std::sync::Mutex<Vec<(usize, bool)>>,
        fail: std::sync::Mutex<Option<(Fail, usize)>>,
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

        fn fail(&self, how: Option<Fail>) {
            *self.fail.lock().unwrap() = how.map(|h| (h, self.managers.load(Ordering::SeqCst)));
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
            let fail = *self.fail.lock().unwrap();
            match fail {
                Some((Fail::Lost, upto)) if *manager <= upto => {
                    Err(ListError { message: "Failed to send message".into(), connection_lost: true })
                }
                Some((Fail::NoService, _)) => {
                    Err(ListError { message: "org.bluez not provided".into(), connection_lost: false })
                }
                _ => Ok(self.adapters.lock().unwrap().iter().map(|a| a.0).collect()),
            }
        }

        async fn state(&self, adapter: &usize) -> Result<CentralState, String> {
            tokio::task::yield_now().await;
            if self.unreachable.load(Ordering::SeqCst) {
                return Err("adapter thread gone".into());
            }
            let powered = self.adapters.lock().unwrap().iter().any(|a| a == &(*adapter, true));
            Ok(if powered { CentralState::PoweredOn } else { CentralState::PoweredOff })
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
    async fn an_unreachable_adapter_is_listed_again_once() {
        let s = Shared::new(Fake::mac(&[(0, true)]));
        assert_eq!(s.adapter().await.ok(), Some(0));
        s.stack.unreachable.store(true, Ordering::SeqCst);
        s.stack.plug(&[(1, true)]);
        // The fresh adapter's own state check fails too, but it is kept.
        assert_eq!(s.adapter().await.ok(), Some(1));
        s.stack.unreachable.store(false, Ordering::SeqCst);
        for _ in 0..10 {
            assert_eq!(s.adapter().await.ok(), Some(1));
        }
        assert_eq!((managers(&s), lists(&s)), (1, 2));
    }

    #[tokio::test]
    async fn the_watcher_hears_only_replacements() {
        let s = Shared::new(Fake::new(&[(0, true), (1, true)]));
        assert_eq!(s.adapter().await.ok(), Some(0));
        let replaced = s.generation.subscribe();
        for _ in 0..10 {
            assert_eq!(s.adapter().await.ok(), Some(0));
        }
        assert!(!replaced.has_changed().unwrap());
        s.stack.plug(&[(1, true)]);
        assert_eq!(s.adapter().await.ok(), Some(1));
        assert!(replaced.has_changed().unwrap());
        assert_eq!(replaced.borrow().lost_managers, 0);
    }

    #[tokio::test]
    async fn bluetoothd_down_keeps_the_manager() {
        let s = Shared::new(Fake::new(&[]));
        s.stack.fail(Some(Fail::NoService));
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
        s.stack.fail(Some(Fail::Lost));
        // The watcher and the reconnect loop both hit the dead connection.
        let (a, b) = tokio::join!(s.adapter(), s.adapter());
        assert!(a.is_err() && b.is_err());
        assert_eq!(s.generation.borrow().lost_managers, 1);
        s.stack.plug(&[(0, true)]);
        for _ in 0..50 {
            assert_eq!(s.adapter().await.ok(), Some(0));
        }
        assert_eq!(managers(&s), 2);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn lost_connection_errors() {
        // As dbus-rs and libdbus report them; btleplug boxes bluez-async's
        // error, which prints the D-Bus error's Debug text.
        let err = |name: &str, msg: &str| btleplug::Error::Other(Box::new(dbus::Error::new_custom(name, msg)));
        assert!(connection_lost(&err("org.freedesktop.DBus.Error.Failed", "Failed to send message")));
        assert!(connection_lost(&err("org.freedesktop.DBus.Error.Disconnected", "Connection is closed")));
        assert!(!connection_lost(&err(
            "org.freedesktop.DBus.Error.ServiceUnknown",
            "The name org.bluez was not provided by any .service files"
        )));
        assert!(!connection_lost(&err("org.freedesktop.DBus.Error.Timeout", "Timeout waiting for reply")));
        assert!(!connection_lost(&err("org.freedesktop.DBus.Error.NoReply", "Message recipient disconnected")));
        assert!(!connection_lost(&err("org.bluez.Error.Failed", "Failed to send message")));
        assert!(!connection_lost(&btleplug::Error::NoAdapterAvailable));
    }
}
