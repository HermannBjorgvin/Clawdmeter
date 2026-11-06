//! The process's handle on the Bluetooth stack: one btleplug `Manager` and
//! one `Adapter`, shared by the reconnect loop, the wakeup watcher and every
//! link.
//!
//! btleplug's stacks never give back what creating these takes. On Linux a
//! `Manager` is a D-Bus system-bus connection whose I/O task btleplug spawns
//! detached, so dropping the manager never closes it; a manager per connect
//! attempt leaked one connection per attempt until the per-user limit (256)
//! starved the whole session. On macOS every `adapters()` call starts a
//! CoreBluetooth thread with its own `CBCentralManager` that never exits, and
//! on Windows every listing builds a new radio watcher. So both are made
//! once and kept, and replaced only when they stop working:
//!
//! * the manager, once its D-Bus connection is proven dead: a listing fails,
//!   a throwaway connection shows that a new one would fare better, and a
//!   retry on the old one fails too (bluetoothd may just have come back);
//! * the adapter, on Linux and Windows, when it reads as off: BlueZ reports
//!   an unplugged adapter (and any failed read) as off, so the adapters are
//!   listed again, and a different first one replaces it. On macOS off just
//!   means off, and listing is what leaks.
//!
//! [`Bluetooth::replaced`] counts both, so the wakeup watcher reopens its
//! stream on the new ones, and [`Bluetooth::retire`] decides whether a
//! finished BlueZ stream may be dropped.

use std::sync::Arc;

use btleplug::api::{BDAddr, Central, CentralState, Manager as _};
use btleplug::platform::{Adapter, Manager};
use tokio::sync::{watch, Mutex};

use crate::LinkError;

/// The shared stack handle; clones share one manager and adapter. On Linux
/// the manager's I/O task runs on the runtime of the
/// [`adapter`](Self::adapter) call that made it, so keep a handle to one
/// runtime and pass clones around.
#[derive(Clone)]
pub struct Bluetooth {
    shared: Arc<Shared<Btleplug>>,
}

/// A handle with nothing created yet; the first lookup creates the manager
/// and adapter.
impl Default for Bluetooth {
    fn default() -> Self {
        Bluetooth { shared: Arc::new(Shared::new(Btleplug)) }
    }
}

impl Bluetooth {
    /// The adapter, if it is powered.
    pub async fn adapter(&self) -> Result<Adapter, LinkError> {
        self.shared.adapter_at().await.map(|(a, _)| a)
    }

    /// The adapter and the [`Replacements`] count it belongs to.
    pub(crate) async fn adapter_at(&self) -> Result<(Adapter, Replacements), LinkError> {
        self.shared.adapter_at().await
    }

    /// Changes whenever the manager or the adapter is replaced.
    pub(crate) fn replaced(&self) -> watch::Receiver<Replacements> {
        self.shared.replaced.subscribe()
    }

    /// Drop a stream whose adapter was looked up when `lost_at` connections
    /// had been lost, once that is safe; see [`Shared::retire`]. Other
    /// stacks have no D-Bus connection, so they drop it at once.
    pub(crate) async fn retire<T>(&self, stream: T, lost_at: u64) {
        if cfg!(target_os = "linux") {
            self.shared.retire(stream, lost_at).await;
        }
    }
}

/// How often the manager and the adapter have been replaced.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub(crate) struct Replacements {
    /// Managers dropped because their D-Bus connection was dead.
    pub(crate) lost_connections: u64,
    /// Adapters listed and kept.
    pub(crate) adapters: u64,
}

/// What [`Shared`] needs from a Bluetooth stack (btleplug, or a fake in
/// tests).
trait Stack {
    type Manager: Send + Sync;
    type Adapter: Clone + Send + Sync;
    /// Tells adapters apart.
    type Identity: PartialEq;
    /// Listing adapters costs nothing lasting, so an adapter that reads as
    /// off is listed again in case it was unplugged.
    fn relist_when_off(&self) -> bool;
    async fn manager(&self) -> Result<Self::Manager, String>;
    async fn adapters(&self, manager: &Self::Manager) -> Result<Vec<Self::Adapter>, String>;
    /// After a failed listing: would a new connection fare better?
    async fn connection_lost(&self) -> bool;
    async fn state(&self, adapter: &Self::Adapter) -> CentralState;
    async fn identity(&self, adapter: &Self::Adapter) -> Self::Identity;
}

struct Btleplug;

impl Stack for Btleplug {
    type Manager = Manager;
    type Adapter = Adapter;
    /// The id and modalias on BlueZ; the radio's address on WinRT.
    type Identity = (Option<String>, Option<BDAddr>);

    fn relist_when_off(&self) -> bool {
        !cfg!(target_os = "macos")
    }

    async fn manager(&self) -> Result<Manager, String> {
        Manager::new().await.map_err(|e| e.to_string())
    }

    async fn adapters(&self, manager: &Manager) -> Result<Vec<Adapter>, String> {
        manager.adapters().await.map_err(|e| e.to_string())
    }

    async fn connection_lost(&self) -> bool {
        connection_lost().await
    }

    /// btleplug 0.13 can't fail this: BlueZ reports any error as off, WinRT
    /// can't fail, and CoreBluetooth only if its thread died (the thread
    /// never ends otherwise).
    async fn state(&self, adapter: &Adapter) -> CentralState {
        adapter.adapter_state().await.unwrap_or(CentralState::Unknown)
    }

    async fn identity(&self, adapter: &Adapter) -> Self::Identity {
        (adapter.adapter_info().await.ok(), adapter.adapter_address().await.ok().flatten())
    }
}

/// Is the manager's D-Bus connection gone, or would a fresh one hit the
/// same error? btleplug boxes bluez-async's error out of reach (its
/// `source()` chain skips the `dbus::Error` inside), so instead of reading
/// the error this asks the bus itself over a throwaway connection; dbus-rs
/// closes those on drop, so the probe leaks nothing. Only a bus or
/// bluetoothd that demonstrably answers proves the manager's connection
/// the dead one; anything else errs on the side of keeping it, since a
/// missed loss only delays recovery while a spurious one leaks a live
/// connection.
#[cfg(target_os = "linux")]
async fn connection_lost() -> bool {
    let reach =
        tokio::task::spawn_blocking(crate::bluez::reach).await.unwrap_or(crate::bluez::Reach::BluetoothUnreachable);
    lost_on(reach)
}

/// No system-bus connection to lose elsewhere.
#[cfg(not(target_os = "linux"))]
async fn connection_lost() -> bool {
    false
}

/// Which [`crate::bluez::Reach`] results mean the manager's connection is
/// dead.
#[cfg(target_os = "linux")]
fn lost_on(reach: crate::bluez::Reach) -> bool {
    matches!(reach, crate::bluez::Reach::Reached | crate::bluez::Reach::BusUnreachable)
}

struct Shared<S: Stack> {
    stack: S,
    slot: Mutex<Slot<S>>,
    replaced: watch::Sender<Replacements>,
}

struct Slot<S: Stack> {
    manager: Option<Arc<S::Manager>>,
    adapter: Option<Arc<S::Adapter>>,
}

impl<S: Stack> Shared<S> {
    fn new(stack: S) -> Self {
        let slot = Slot { manager: None, adapter: None };
        Shared { stack, slot: Mutex::new(slot), replaced: watch::Sender::new(Replacements::default()) }
    }

    /// The adapter, with the [`Replacements`] count read together with it
    /// (under the lock), so the watcher can tell whether it is still current.
    /// Only listing holds the lock while it waits on the stack.
    async fn adapter_at(&self) -> Result<(S::Adapter, Replacements), LinkError> {
        let (kept, current) = {
            let slot = self.slot.lock().await;
            (slot.adapter.clone(), *self.replaced.borrow())
        };
        if let Some(a) = &kept {
            // WinRT reads a disabled or removed radio as unknown.
            match self.stack.state(a).await {
                CentralState::PoweredOn => return Ok((S::Adapter::clone(a), current)),
                _ if self.stack.relist_when_off() => {}
                CentralState::PoweredOff => return Err(powered_off()),
                _ => return Ok((S::Adapter::clone(a), current)),
            }
        }
        let (adapter, current) = self.relist(kept.as_ref()).await?;
        if self.stack.state(&adapter).await == CentralState::PoweredOff {
            return Err(powered_off());
        }
        Ok((S::Adapter::clone(&adapter), current))
    }

    /// List the adapters, and replace `stale` (the kept adapter the caller
    /// found off, if any) if the first one listed is a different one. Holds
    /// the lock throughout, so lookups that fail together list once between
    /// them.
    async fn relist(&self, stale: Option<&Arc<S::Adapter>>) -> Result<(Arc<S::Adapter>, Replacements), LinkError> {
        let mut slot = self.slot.lock().await;
        let kept = slot.adapter.clone();
        if let Some(a) = &kept {
            if !stale.is_some_and(|s| Arc::ptr_eq(s, a)) {
                // Another lookup listed one while this one waited.
                return Ok((Arc::clone(a), *self.replaced.borrow()));
            }
        }
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
                if !self.stack.connection_lost().await {
                    return Err(LinkError::NoAdapter(e));
                }
                // The probe can find bluetoothd answering again just after
                // this call hit it restarting, so ask this connection once
                // more: a dead one fails again at once, a live one answers.
                match self.stack.adapters(&manager).await {
                    Ok(list) => list,
                    Err(_) => {
                        log::info!("Bluetooth: the system bus connection is dead ({e}); opening a new one");
                        *slot = Slot { manager: None, adapter: None };
                        self.replaced.send_modify(|r| r.lost_connections += 1);
                        return Err(LinkError::NoAdapter(e));
                    }
                }
            }
        };
        let Some(first) = list.into_iter().next() else {
            slot.adapter = None;
            return Err(LinkError::NoAdapter("none present".into()));
        };
        // The same adapter keeps the stream on it open, unless it is a
        // stale object reading otherwise than a fresh one (WinRT's radio of
        // a replugged dongle).
        if let Some(a) = &kept {
            if self.stack.identity(a).await == self.stack.identity(&first).await
                && self.stack.state(a).await == self.stack.state(&first).await
            {
                return Ok((Arc::clone(a), *self.replaced.borrow()));
            }
        }
        let a = Arc::new(first);
        slot.adapter = Some(Arc::clone(&a));
        self.replaced.send_modify(|r| r.adapters += 1);
        Ok((a, *self.replaced.borrow()))
    }

    /// Drop `stream` once a listing on its connection answers, or leak it
    /// once that connection is counted lost: bluez-async's stream drop
    /// unwraps a `RemoveMatch` call on the connection, which panics on a
    /// dead one, and release builds abort (panic = "abort"). A leaked stream
    /// is heap only, its socket already gone; leaking one on a live
    /// connection would keep its match rule, and queue every signal it
    /// matches, for good. Until either is known (bluetoothd not answering),
    /// it asks again on every replacement and every [`RETIRE_RECHECK`].
    async fn retire<T>(&self, stream: T, lost_at: u64) {
        let mut replaced = self.replaced.subscribe();
        loop {
            if replaced.borrow_and_update().lost_connections != lost_at {
                std::mem::forget(stream);
                return;
            }
            if self.answers(lost_at).await {
                return;
            }
            tokio::select! {
                _ = replaced.changed() => {}
                () = tokio::time::sleep(RETIRE_RECHECK) => {}
            }
        }
    }

    /// Does the connection that was current when `lost_at` connections had
    /// been lost still answer? The count is read after the listing: a lookup
    /// that replaced that dead manager meanwhile has counted it by then.
    async fn answers(&self, lost_at: u64) -> bool {
        let manager = self.slot.lock().await.manager.clone();
        let Some(m) = manager else { return false };
        self.stack.adapters(&m).await.is_ok() && self.replaced.borrow().lost_connections == lost_at
    }
}

/// How often [`Shared::retire`] asks again whether a connection answers.
const RETIRE_RECHECK: std::time::Duration = std::time::Duration::from_secs(5);

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
    /// managers fail (no system bus yet); `failing` makes listings fail.
    #[derive(Default)]
    struct Fake {
        relist_when_off: bool,
        managers: AtomicUsize,
        lists: AtomicUsize,
        manager_failures: AtomicUsize,
        adapters: std::sync::Mutex<Vec<(usize, bool)>>,
        failing: std::sync::Mutex<Option<Failing>>,
        /// The adapter that reads as unknown.
        unknown: std::sync::Mutex<Option<usize>>,
    }

    /// Listings on managers up to `upto` fail, `left` more times; the probe
    /// then says `lost`.
    #[derive(Clone, Copy)]
    struct Failing {
        upto: usize,
        left: usize,
        lost: bool,
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

        /// bluetoothd stopped or hung: every listing fails over a live bus.
        fn bluetoothd_down(&self) {
            *self.failing.lock().unwrap() = Some(Failing { upto: usize::MAX, left: usize::MAX, lost: false });
        }

        /// The bus restarted: the managers made so far are dead.
        fn bus_restarted(&self) {
            let upto = self.managers.load(Ordering::SeqCst);
            *self.failing.lock().unwrap() = Some(Failing { upto, left: usize::MAX, lost: true });
        }

        /// bluetoothd restarting: one listing fails, then it answers.
        fn bluetoothd_restarting(&self) {
            *self.failing.lock().unwrap() = Some(Failing { upto: usize::MAX, left: 1, lost: true });
        }

        fn recover(&self) {
            *self.failing.lock().unwrap() = None;
        }
    }

    impl Stack for Fake {
        /// The manager's generation (1 = the first made).
        type Manager = usize;
        type Adapter = usize;
        type Identity = usize;

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

        async fn adapters(&self, manager: &usize) -> Result<Vec<usize>, String> {
            // Let a concurrent lookup reach the lock before this one returns.
            tokio::task::yield_now().await;
            self.lists.fetch_add(1, Ordering::SeqCst);
            let mut failing = self.failing.lock().unwrap();
            if let Some(f) = failing.as_mut().filter(|f| *manager <= f.upto && f.left > 0) {
                f.left -= 1;
                return Err("listing failed".into());
            }
            Ok(self.adapters.lock().unwrap().iter().map(|a| a.0).collect())
        }

        async fn connection_lost(&self) -> bool {
            self.failing.lock().unwrap().is_some_and(|f| f.lost)
        }

        async fn state(&self, adapter: &usize) -> CentralState {
            tokio::task::yield_now().await;
            if *self.unknown.lock().unwrap() == Some(*adapter) {
                return CentralState::Unknown;
            }
            let powered = self.adapters.lock().unwrap().iter().any(|a| a == &(*adapter, true));
            if powered {
                CentralState::PoweredOn
            } else {
                CentralState::PoweredOff
            }
        }

        /// Adapters 10 and 11 are one dongle, replugged.
        async fn identity(&self, adapter: &usize) -> usize {
            *adapter / 10
        }
    }

    async fn lookup(s: &Shared<Fake>) -> Result<usize, LinkError> {
        s.adapter_at().await.map(|(a, _)| a)
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
                assert_eq!(lookup(&s).await.ok(), Some(0));
            }
            assert_eq!((managers(&s), lists(&s)), (1, 1));
        }
    }

    #[tokio::test]
    async fn concurrent_first_lookups_list_once() {
        let s = Shared::new(Fake::mac(&[(0, true)]));
        let (a, b) = tokio::join!(lookup(&s), lookup(&s));
        assert_eq!((a.ok(), b.ok()), (Some(0), Some(0)));
        assert_eq!((managers(&s), lists(&s)), (1, 1));
    }

    #[tokio::test]
    async fn bluetooth_off_on_macos_keeps_the_adapter() {
        let s = Shared::new(Fake::mac(&[(0, false)]));
        for _ in 0..50 {
            assert!(is_off(lookup(&s).await));
        }
        s.stack.plug(&[(0, true)]);
        assert_eq!(lookup(&s).await.ok(), Some(0));
        assert_eq!((managers(&s), lists(&s)), (1, 1));
    }

    #[tokio::test]
    async fn bluetooth_off_on_linux_relists_but_keeps_the_same_adapter() {
        let s = Shared::new(Fake::new(&[(0, false)]));
        let replaced = s.replaced.subscribe();
        for _ in 0..50 {
            assert!(is_off(lookup(&s).await));
        }
        s.stack.plug(&[(0, true)]);
        assert_eq!(lookup(&s).await.ok(), Some(0));
        // Listed again each time, but never replaced: the watcher keeps its
        // stream.
        assert_eq!(*replaced.borrow(), Replacements { lost_connections: 0, adapters: 1 });
        assert_eq!(managers(&s), 1);
        // Back on: no more listings.
        let before = lists(&s);
        assert_eq!(lookup(&s).await.ok(), Some(0));
        assert_eq!(lists(&s), before);
    }

    #[tokio::test]
    async fn no_adapter_asks_the_same_manager_again() {
        let s = Shared::new(Fake::new(&[]));
        for _ in 0..50 {
            assert!(matches!(lookup(&s).await, Err(LinkError::NoAdapter(_))));
        }
        s.stack.plug(&[(7, true)]);
        assert_eq!(lookup(&s).await.ok(), Some(7));
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn a_failed_manager_is_retried_until_one_exists() {
        let s = Shared::new(Fake::new(&[(0, true)]));
        s.stack.manager_failures.store(3, Ordering::SeqCst);
        for _ in 0..3 {
            assert!(matches!(lookup(&s).await, Err(LinkError::NoAdapter(_))));
        }
        for _ in 0..10 {
            assert_eq!(lookup(&s).await.ok(), Some(0));
        }
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn an_unplugged_adapter_gives_way_to_the_next() {
        let s = Shared::new(Fake::new(&[(0, true), (1, true)]));
        let (a, at) = s.adapter_at().await.unwrap();
        assert_eq!((a, at), (0, *s.replaced.borrow()));
        s.stack.plug(&[(1, true)]);
        let (a, at) = s.adapter_at().await.unwrap();
        assert_eq!((a, at), (1, Replacements { lost_connections: 0, adapters: 2 }));
        assert_eq!(s.adapter_at().await.ok(), Some((1, at)));
        assert_eq!((managers(&s), lists(&s)), (1, 2));
    }

    #[tokio::test]
    async fn errors_over_a_live_bus_keep_the_manager() {
        let s = Shared::new(Fake::new(&[]));
        s.stack.bluetoothd_down();
        for _ in 0..50 {
            assert!(matches!(lookup(&s).await, Err(LinkError::NoAdapter(_))));
        }
        s.stack.recover();
        s.stack.plug(&[(0, true)]);
        assert_eq!(lookup(&s).await.ok(), Some(0));
        assert_eq!(managers(&s), 1);
    }

    #[tokio::test]
    async fn bluetoothd_answering_again_keeps_the_manager() {
        let s = Shared::new(Fake::new(&[(0, false)]));
        assert!(is_off(lookup(&s).await));
        s.stack.bluetoothd_restarting();
        s.stack.plug(&[(0, true)]);
        assert_eq!(lookup(&s).await.ok(), Some(0));
        assert_eq!((managers(&s), s.replaced.borrow().lost_connections), (1, 0));
    }

    #[tokio::test]
    async fn a_dead_connection_replaces_the_manager_once() {
        // The bus went away while the adapter was off (on Linux a dead
        // connection also reads as off), so lookups list again and fail.
        let s = Shared::new(Fake::new(&[(0, false)]));
        assert!(is_off(lookup(&s).await));
        s.stack.bus_restarted();
        // The watcher and the reconnect loop both hit the dead connection.
        let (a, b) = tokio::join!(lookup(&s), lookup(&s));
        assert!(a.is_err() && b.is_err());
        assert_eq!(s.replaced.borrow().lost_connections, 1);
        s.stack.plug(&[(0, true)]);
        for _ in 0..50 {
            assert_eq!(lookup(&s).await.ok(), Some(0));
        }
        assert_eq!(managers(&s), 2);
    }

    #[tokio::test]
    async fn only_a_connection_that_answers_drops_its_streams() {
        let s = Shared::new(Fake::new(&[(0, true)]));
        assert!(!s.answers(0).await, "no connection yet");
        lookup(&s).await.unwrap();
        assert!(s.answers(0).await);
        // Dead, but no lookup has noticed yet.
        s.stack.bus_restarted();
        assert!(!s.answers(0).await);
        // Noticed (the dead connection reads as off) and replaced: the old
        // count still says leak, the new connection answers.
        s.stack.plug(&[(0, false)]);
        assert!(lookup(&s).await.is_err());
        assert!(!s.answers(0).await);
        s.stack.plug(&[(0, true)]);
        lookup(&s).await.unwrap();
        assert!(!s.answers(0).await);
        assert!(s.answers(1).await);
    }

    #[tokio::test(start_paused = true)]
    async fn streams_wait_for_a_verdict_before_retiring() {
        let s = Arc::new(Shared::new(Fake::new(&[(0, true)])));
        lookup(&s).await.unwrap();
        let retire = |s: &Arc<Shared<Fake>>, stream: Arc<()>| {
            let s = Arc::clone(s);
            tokio::spawn(async move { s.retire(stream, 0).await })
        };
        // A live connection: dropped.
        let stream = Arc::new(());
        retire(&s, Arc::clone(&stream)).await.unwrap();
        assert_eq!(Arc::strong_count(&stream), 1);
        // bluetoothd down: held until it answers again, then dropped.
        s.stack.bluetoothd_down();
        let task = retire(&s, Arc::clone(&stream));
        tokio::time::sleep(RETIRE_RECHECK * 3).await;
        assert!(!task.is_finished());
        s.stack.recover();
        task.await.unwrap();
        assert_eq!(Arc::strong_count(&stream), 1);
        // A dead connection: leaked once a lookup counts it lost.
        s.stack.bus_restarted();
        let task = retire(&s, Arc::clone(&stream));
        tokio::time::sleep(RETIRE_RECHECK).await;
        s.stack.plug(&[(0, false)]);
        assert!(lookup(&s).await.is_err());
        task.await.unwrap();
        assert_eq!(Arc::strong_count(&stream), 2);
    }

    #[tokio::test]
    async fn an_unknown_state_lists_again_except_on_macos() {
        // WinRT reads a disabled or removed radio as unknown.
        let s = Shared::new(Fake::new(&[(0, true)]));
        lookup(&s).await.unwrap();
        s.stack.plug(&[(1, true)]);
        *s.stack.unknown.lock().unwrap() = Some(0);
        assert_eq!(lookup(&s).await.ok(), Some(1));
        // A replugged dongle: the stale object reads unknown, the fresh one
        // on.
        let s = Shared::new(Fake::new(&[(10, true)]));
        lookup(&s).await.unwrap();
        s.stack.plug(&[(11, true)]);
        *s.stack.unknown.lock().unwrap() = Some(10);
        assert_eq!(lookup(&s).await.ok(), Some(11));
        // CoreBluetooth reads unknown until it has settled.
        let s = Shared::new(Fake::mac(&[(0, true)]));
        lookup(&s).await.unwrap();
        *s.stack.unknown.lock().unwrap() = Some(0);
        assert_eq!(lookup(&s).await.ok(), Some(0));
        assert_eq!(lists(&s), 1);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn reach_results() {
        use crate::bluez::Reach::*;
        assert!(lost_on(Reached));
        assert!(lost_on(BusUnreachable));
        assert!(!lost_on(BluetoothUnreachable));
    }
}
