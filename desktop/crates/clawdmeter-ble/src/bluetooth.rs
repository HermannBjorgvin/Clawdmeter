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
//!   (the system bus restarted). Errors that came back over a live
//!   connection, such as bluetoothd not running, keep it. As a backstop for
//!   a dead connection that fails some other way, [`BACKSTOP_FAILURES`]
//!   unexplained failures in a row (timeouts, unrecognised errors) replace
//!   it too, keeping each manager at least [`BACKSTOP_FIRST`], doubling
//!   each time until a listing succeeds: a bus that hangs for good costs at
//!   most one connection per 10 min, then 20, 40, … (7 in the first day).
//!   A stopped bluetoothd answers (`ServiceUnknown`), so it costs none;
//! * the adapter when it can't be reached, or (Linux and Windows, where
//!   listing is cheap) when it reads as off; BlueZ reports an unplugged
//!   adapter as off, so the next one is picked up.
//!
//! The wakeup watcher reopens its event stream whenever either is replaced
//! ([`Bluetooth::replaced`]); BlueZ streams never end on their own. It also
//! skips the stack's report of a connect the daemon made itself
//! ([`Bluetooth::expect_own_connect`]), which would otherwise read as the
//! device coming back.

use std::collections::HashSet;
use std::sync::Arc;
use std::time::{Duration, Instant};

use btleplug::api::{Central, CentralState, Manager as _};
use btleplug::platform::{Adapter, Manager};
use tokio::sync::{watch, Mutex};

use crate::LinkError;

/// Unexplained listing failures in a row before the backstop replaces the
/// manager.
pub const BACKSTOP_FAILURES: u32 = 5;
/// Shortest time a manager is kept before the backstop may replace it; it
/// doubles with each backstop replacement until a listing succeeds.
pub const BACKSTOP_FIRST: Duration = Duration::from_secs(10 * 60);

/// The shared stack handle; clones share one manager and adapter. On Linux
/// the D-Bus connection's I/O task runs on the runtime of the first
/// [`Bluetooth::adapter`] call, so make that call from a runtime that lives
/// as long as the handle (the daemon's).
#[derive(Clone)]
pub struct Bluetooth {
    shared: Arc<Shared<Btleplug>>,
    own: Arc<OwnConnects>,
}

/// A handle with nothing created yet; the first [`Bluetooth::adapter`] call
/// creates the manager and adapter.
impl Default for Bluetooth {
    fn default() -> Self {
        Bluetooth { shared: Arc::new(Shared::new(Btleplug)), own: Arc::default() }
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

    /// The daemon is about to connect to the device with this key: the
    /// stack's report of that connect is not a wakeup.
    pub(crate) fn expect_own_connect(&self, key: &str) {
        self.own.expect(key);
    }

    /// A new attempt starts: reports still expected from the last one never
    /// came (its connect failed before the link came up, or the watcher
    /// wasn't listening), so don't swallow a real one. A failed connect
    /// keeps its record until then: BlueZ can report the link up and fail
    /// the call right after, and that report must not cut the backoff.
    pub(crate) fn clear_own_connects(&self) {
        self.own.clear();
    }

    /// Is this report of a connect to `key` the daemon's own? Consumes it.
    pub(crate) fn take_own_connect(&self, key: &str) -> bool {
        self.own.take(key)
    }
}

/// Keys of devices the daemon is connecting itself, whose connect report
/// hasn't arrived yet.
#[derive(Default)]
struct OwnConnects(std::sync::Mutex<HashSet<String>>);

impl OwnConnects {
    fn keys(&self) -> std::sync::MutexGuard<'_, HashSet<String>> {
        self.0.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn expect(&self, key: &str) {
        self.keys().insert(key.to_ascii_uppercase());
    }

    fn clear(&self) {
        self.keys().clear();
    }

    fn take(&self, key: &str) -> bool {
        self.keys().remove(&key.to_ascii_uppercase())
    }
}

/// How often the manager and the adapter have been replaced.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub(crate) struct Replacements {
    /// Managers dropped because their D-Bus connection was (or may have
    /// been) gone.
    pub(crate) lost_managers: u64,
    /// Adapters listed and kept.
    pub(crate) adapters: u64,
}

/// Why listing adapters failed.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(not(target_os = "linux"), allow(dead_code))] // only BlueZ errors are told apart
enum Failure {
    /// The manager's connection is gone.
    ConnectionLost,
    /// The error came back over a live connection (bluetoothd not running,
    /// a BlueZ error): the manager is fine.
    Answered,
    /// No answer, or an error this code doesn't recognise. Counts towards
    /// the backstop.
    Unexplained,
}

/// A failed adapter listing.
struct ListError {
    message: String,
    failure: Failure,
}

/// What [`Shared`] needs from a Bluetooth stack (btleplug, or a fake in
/// tests).
trait Stack {
    type Manager: Send + Sync;
    type Adapter: Clone + Send + Sync;
    /// Listing adapters costs nothing lasting, so an adapter that reads as
    /// off is listed again in case it was unplugged.
    fn relist_when_off(&self) -> bool;
    fn now(&self) -> Instant;
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

    fn now(&self) -> Instant {
        Instant::now()
    }

    async fn manager(&self) -> Result<Manager, String> {
        Manager::new().await.map_err(|e| e.to_string())
    }

    async fn adapters(&self, manager: &Manager) -> Result<Vec<Adapter>, ListError> {
        manager.adapters().await.map_err(|e| ListError { message: e.to_string(), failure: failure(&e) })
    }

    async fn state(&self, adapter: &Adapter) -> Result<CentralState, String> {
        adapter.adapter_state().await.map_err(|e| e.to_string())
    }
}

/// Classify a BlueZ listing error. btleplug boxes bluez-async's error into
/// `Error::Other`; its D-Bus errors carry the error name.
#[cfg(target_os = "linux")]
fn failure(e: &btleplug::Error) -> Failure {
    let btleplug::Error::Other(inner) = e else { return Failure::Unexplained };
    match inner.downcast_ref::<bluez_async::BluetoothError>() {
        Some(bluez_async::BluetoothError::DbusError(d)) => dbus_failure(d),
        // Parsed a reply: the connection works.
        Some(_) => Failure::Answered,
        None => Failure::Unexplained,
    }
}

/// Elsewhere the manager holds nothing, and replacing it buys nothing.
#[cfg(not(target_os = "linux"))]
fn failure(_: &btleplug::Error) -> Failure {
    Failure::Answered
}

/// `Disconnected`, and dbus-rs's own "Failed to send message" (returned once
/// the connection's I/O task has ended), mean the connection is gone.
/// `Timeout` and `NoReply` are also what a hung or crashing bluetoothd gives
/// on a live bus, so they only count towards the backstop. Any other error
/// was sent back over the bus (`ServiceUnknown`: bluetoothd isn't running).
#[cfg(target_os = "linux")]
fn dbus_failure(e: &dbus::Error) -> Failure {
    match e.name() {
        Some("org.freedesktop.DBus.Error.Disconnected") => Failure::ConnectionLost,
        Some("org.freedesktop.DBus.Error.Failed") if e.message() == Some("Failed to send message") => {
            Failure::ConnectionLost
        }
        Some(
            "org.freedesktop.DBus.Error.Timeout"
            | "org.freedesktop.DBus.Error.NoReply"
            | "org.freedesktop.DBus.Error.Failed",
        )
        | None => Failure::Unexplained,
        Some(_) => Failure::Answered,
    }
}

struct Shared<S: Stack> {
    stack: S,
    slot: Mutex<Slot<S>>,
    generation: watch::Sender<Replacements>,
}

struct Slot<S: Stack> {
    manager: Option<Arc<S::Manager>>,
    adapter: Option<Arc<S::Adapter>>,
    /// When the kept manager was made.
    manager_since: Instant,
    /// Unexplained listing failures in a row.
    unexplained: u32,
    /// How long a manager is kept before the backstop may replace it.
    backstop_after: Duration,
}

impl<S: Stack> Shared<S> {
    fn new(stack: S) -> Self {
        let slot = Slot {
            manager: None,
            adapter: None,
            manager_since: stack.now(),
            unexplained: 0,
            backstop_after: BACKSTOP_FIRST,
        };
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
                Ok(CentralState::PoweredOff) if !self.stack.relist_when_off() => return Err(powered_off()),
                Ok(CentralState::PoweredOff) => {}
                Ok(_) => return Ok((S::Adapter::clone(a), current)),
                Err(e) => log::debug!("Bluetooth adapter unreachable ({e}); listing adapters again"),
            }
        }
        let (adapter, current) = self.relist(kept.as_ref()).await?;
        if let Ok(CentralState::PoweredOff) = self.stack.state(&adapter).await {
            return Err(powered_off());
        }
        Ok((S::Adapter::clone(&adapter), current))
    }

    /// Replace `stale` (the kept adapter the caller found unusable, if any)
    /// with the first one listed. Holds the lock throughout, so lookups that
    /// fail together make one adapter between them.
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
                slot.manager_since = self.stack.now();
                m
            }
        };
        let list = match self.stack.adapters(&manager).await {
            Ok(list) => list,
            Err(e) => {
                self.listing_failed(&mut slot, &e);
                return Err(LinkError::NoAdapter(e.message));
            }
        };
        slot.unexplained = 0;
        slot.backstop_after = BACKSTOP_FIRST;
        let a = Arc::new(list.into_iter().next().ok_or_else(|| LinkError::NoAdapter("none present".into()))?);
        slot.adapter = Some(Arc::clone(&a));
        self.generation.send_modify(|g| g.adapters += 1);
        Ok((a, *self.generation.borrow()))
    }

    fn listing_failed(&self, slot: &mut Slot<S>, e: &ListError) {
        let replace = match e.failure {
            Failure::ConnectionLost => {
                log::info!("Bluetooth: lost the system bus connection ({}); reconnecting", e.message);
                true
            }
            Failure::Answered => {
                slot.unexplained = 0;
                false
            }
            Failure::Unexplained => {
                slot.unexplained += 1;
                let due = slot.unexplained >= BACKSTOP_FAILURES
                    && self.stack.now().duration_since(slot.manager_since) >= slot.backstop_after;
                if due {
                    log::info!(
                        "Bluetooth: adapter listing keeps failing ({}); reconnecting to the system bus",
                        e.message
                    );
                    slot.backstop_after *= 2;
                }
                due
            }
        };
        if replace {
            slot.manager = None;
            slot.unexplained = 0;
            self.generation.send_modify(|g| g.lost_managers += 1);
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
        /// No answer (a hung bluetoothd, or a dead bus failing oddly).
        Hung,
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
        /// Minutes since the fake's clock started.
        minutes: AtomicUsize,
        start: std::sync::OnceLock<Instant>,
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

        /// Fail every listing, on any manager.
        fn fail_all(&self, how: Fail) {
            *self.fail.lock().unwrap() = Some((how, usize::MAX));
        }

        fn wait_minutes(&self, m: usize) {
            self.minutes.fetch_add(m, Ordering::SeqCst);
        }
    }

    impl Stack for Fake {
        /// The manager's generation (1 = the first made).
        type Manager = usize;
        type Adapter = usize;

        fn relist_when_off(&self) -> bool {
            self.relist_when_off
        }

        fn now(&self) -> Instant {
            let start = *self.start.get_or_init(Instant::now);
            start + Duration::from_secs(60 * self.minutes.load(Ordering::SeqCst) as u64)
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
                    Err(ListError { message: "Failed to send message".into(), failure: Failure::ConnectionLost })
                }
                Some((Fail::Hung, upto)) if *manager <= upto => {
                    Err(ListError { message: "Timeout waiting for reply".into(), failure: Failure::Unexplained })
                }
                Some((Fail::NoService, _)) => {
                    Err(ListError { message: "org.bluez not provided".into(), failure: Failure::Answered })
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

    #[tokio::test]
    async fn the_backstop_replaces_a_silent_manager_rarely() {
        let s = Shared::new(Fake::new(&[]));
        s.stack.fail_all(Fail::Hung);
        // A fresh manager is kept for BACKSTOP_FIRST however often it fails.
        for _ in 0..50 {
            assert!(s.adapter().await.is_err());
        }
        assert_eq!(managers(&s), 1);
        s.stack.wait_minutes(10);
        for _ in 0..BACKSTOP_FAILURES {
            assert!(s.adapter().await.is_err());
        }
        assert_eq!(s.generation.borrow().lost_managers, 1);
        // The next one waits twice as long.
        for _ in 0..50 {
            assert!(s.adapter().await.is_err());
        }
        s.stack.wait_minutes(19);
        for _ in 0..50 {
            assert!(s.adapter().await.is_err());
        }
        assert_eq!(managers(&s), 2);
        s.stack.wait_minutes(1);
        for _ in 0..BACKSTOP_FAILURES {
            assert!(s.adapter().await.is_err());
        }
        assert_eq!(s.generation.borrow().lost_managers, 2);
        // A day of a hung bus: one manager per 10, 20, 40, … min.
        for _ in 0..(24 * 60) {
            s.stack.wait_minutes(1);
            assert!(s.adapter().await.is_err());
        }
        assert_eq!(s.generation.borrow().lost_managers, 7);
    }

    #[tokio::test]
    async fn the_backstop_ignores_answered_failures() {
        let s = Shared::new(Fake::new(&[]));
        s.stack.fail_all(Fail::NoService);
        for _ in 0..(24 * 60) {
            s.stack.wait_minutes(1);
            assert!(s.adapter().await.is_err());
        }
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn the_lookup_says_which_generation_it_belongs_to() {
        let s = Shared::new(Fake::new(&[(0, true), (1, true)]));
        let (a, at) = s.adapter_at().await.unwrap();
        assert_eq!((a, at), (0, *s.generation.borrow()));
        s.stack.plug(&[(1, true)]);
        let (a, at) = s.adapter_at().await.unwrap();
        assert_eq!((a, at.adapters), (1, 2));
        assert_eq!(at, *s.generation.borrow());
    }

    #[test]
    fn own_connects_are_skipped_once() {
        let own = OwnConnects::default();
        own.expect("aa:bb:cc:dd:ee:ff");
        assert!(!own.take("11:22:33:44:55:66"), "another device");
        assert!(own.take("AA:BB:CC:DD:EE:FF"));
        assert!(!own.take("AA:BB:CC:DD:EE:FF"), "a later connect is the device's own");
        own.expect("AA:BB:CC:DD:EE:FF");
        own.clear();
        assert!(!own.take("AA:BB:CC:DD:EE:FF"), "a new attempt started");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn listing_failures() {
        // Wrapped as btleplug wraps them: bluez-async's error, boxed.
        let dbus_err = |name: &str, msg: &str| {
            failure(&btleplug::Error::from(bluez_async::BluetoothError::DbusError(dbus::Error::new_custom(name, msg))))
        };
        assert_eq!(dbus_err("org.freedesktop.DBus.Error.Failed", "Failed to send message"), Failure::ConnectionLost);
        assert_eq!(
            dbus_err("org.freedesktop.DBus.Error.Disconnected", "Connection is closed"),
            Failure::ConnectionLost
        );
        assert_eq!(
            dbus_err("org.freedesktop.DBus.Error.ServiceUnknown", "The name org.bluez was not provided"),
            Failure::Answered
        );
        assert_eq!(dbus_err("org.bluez.Error.Failed", "Failed to send message"), Failure::Answered);
        assert_eq!(dbus_err("org.freedesktop.DBus.Error.Timeout", "Timeout waiting for reply"), Failure::Unexplained);
        assert_eq!(
            dbus_err("org.freedesktop.DBus.Error.NoReply", "Message recipient disconnected"),
            Failure::Unexplained
        );
        assert_eq!(dbus_err("org.freedesktop.DBus.Error.Failed", "Out of memory"), Failure::Unexplained);
        assert_eq!(
            failure(&btleplug::Error::from(bluez_async::BluetoothError::NoBluetoothAdapters)),
            Failure::Answered
        );
        assert_eq!(failure(&btleplug::Error::NoAdapterAvailable), Failure::Unexplained);
    }
}
