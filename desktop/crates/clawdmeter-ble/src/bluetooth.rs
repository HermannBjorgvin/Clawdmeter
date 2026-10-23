//! The process's handle on the Bluetooth stack: one btleplug `Manager`,
//! shared by the reconnect loop and the wakeup watcher.
//!
//! btleplug's stacks never give back what creating them takes: on Linux
//! every `Manager::new()` opens a system D-Bus connection whose I/O task
//! outlives the `Manager` (the bus caps a user at 256 connections), and on
//! macOS every `adapters()` call starts a CoreBluetooth thread that never
//! exits. So the manager is created once (retried only while creating it
//! fails). Off macOS, each lookup asks it for the adapters again, which is
//! one D-Bus call on Linux, so an unplugged adapter gives way to the next;
//! on macOS the one system adapter is kept.

use std::sync::Arc;

use btleplug::api::{Central, CentralState, Manager as _};
use btleplug::platform::{Adapter, Manager};
use tokio::sync::Mutex;

use crate::LinkError;

/// The shared stack handle; clones share one manager. On Linux the D-Bus
/// connection's I/O task runs on the runtime of the first
/// [`Bluetooth::adapter`] call, so make that call from a runtime that lives
/// as long as the handle (the daemon's).
#[derive(Clone, Default)]
pub struct Bluetooth {
    shared: Arc<Shared<Btleplug>>,
}

impl Bluetooth {
    /// A handle with nothing created yet; the first [`Bluetooth::adapter`]
    /// call creates the manager.
    pub fn new() -> Self {
        Self::default()
    }

    /// The first adapter, if it is powered.
    pub async fn adapter(&self) -> Result<Adapter, LinkError> {
        self.shared.adapter().await
    }
}

/// What [`Shared`] needs from a Bluetooth stack (btleplug, or a fake in
/// tests).
trait Stack {
    type Manager: Send;
    type Adapter: Clone + Send;
    /// Keep the first adapter rather than listing them on every lookup.
    fn keeps_adapter(&self) -> bool;
    async fn manager(&self) -> Result<Self::Manager, String>;
    async fn adapters(&self, manager: &Self::Manager) -> Result<Vec<Self::Adapter>, String>;
    async fn state(&self, adapter: &Self::Adapter) -> Result<CentralState, String>;
}

#[derive(Default)]
struct Btleplug;

impl Stack for Btleplug {
    type Manager = Manager;
    type Adapter = Adapter;

    fn keeps_adapter(&self) -> bool {
        cfg!(target_os = "macos")
    }

    async fn manager(&self) -> Result<Manager, String> {
        Manager::new().await.map_err(|e| e.to_string())
    }

    async fn adapters(&self, manager: &Manager) -> Result<Vec<Adapter>, String> {
        manager.adapters().await.map_err(|e| e.to_string())
    }

    async fn state(&self, adapter: &Adapter) -> Result<CentralState, String> {
        adapter.adapter_state().await.map_err(|e| e.to_string())
    }
}

#[derive(Default)]
struct Shared<S: Stack> {
    stack: S,
    state: Mutex<State<S>>,
}

struct State<S: Stack> {
    manager: Option<S::Manager>,
    adapter: Option<S::Adapter>,
}

impl<S: Stack> Default for State<S> {
    fn default() -> Self {
        State { manager: None, adapter: None }
    }
}

impl<S: Stack> Shared<S> {
    async fn adapter(&self) -> Result<S::Adapter, LinkError> {
        let mut state = self.state.lock().await;
        if let Some(adapter) = &state.adapter {
            match self.stack.state(adapter).await {
                Ok(CentralState::PoweredOff) => return Err(powered_off()),
                Ok(_) => return Ok(adapter.clone()),
                Err(e) => {
                    log::debug!("Bluetooth adapter unreachable ({e}); looking again");
                    state.adapter = None;
                }
            }
        }
        if state.manager.is_none() {
            state.manager = Some(self.stack.manager().await.map_err(LinkError::NoAdapter)?);
        }
        let manager = state.manager.as_ref().expect("set above");
        let adapters = self.stack.adapters(manager).await.map_err(LinkError::NoAdapter)?;
        let adapter = adapters.into_iter().next().ok_or_else(|| LinkError::NoAdapter("none present".into()))?;
        if self.stack.keeps_adapter() {
            state.adapter = Some(adapter.clone());
        }
        if let Ok(CentralState::PoweredOff) = self.stack.state(&adapter).await {
            return Err(powered_off());
        }
        Ok(adapter)
    }
}

fn powered_off() -> LinkError {
    LinkError::NoAdapter("adapter is off".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    /// Counts what the real stack would create. `adapters` lists the
    /// plugged-in adapters as (id, powered); like btleplug on Linux and
    /// Windows, an adapter that is gone reads as powered off.
    /// `manager_failures` makes the first managers fail (no system bus yet).
    #[derive(Default)]
    struct Fake {
        keep: bool,
        managers: AtomicUsize,
        lists: AtomicUsize,
        manager_failures: AtomicUsize,
        adapters: std::sync::Mutex<Vec<(usize, bool)>>,
    }

    impl Fake {
        fn new(keep: bool, adapters: &[(usize, bool)]) -> Self {
            Fake { keep, adapters: std::sync::Mutex::new(adapters.to_vec()), ..Fake::default() }
        }

        fn plug(&self, adapters: &[(usize, bool)]) {
            *self.adapters.lock().unwrap() = adapters.to_vec();
        }
    }

    impl Stack for Fake {
        type Manager = ();
        type Adapter = usize;

        fn keeps_adapter(&self) -> bool {
            self.keep
        }

        async fn manager(&self) -> Result<(), String> {
            if self.manager_failures.load(Ordering::SeqCst) > 0 {
                self.manager_failures.fetch_sub(1, Ordering::SeqCst);
                return Err("no system bus".into());
            }
            self.managers.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }

        async fn adapters(&self, _: &()) -> Result<Vec<usize>, String> {
            self.lists.fetch_add(1, Ordering::SeqCst);
            Ok(self.adapters.lock().unwrap().iter().map(|a| a.0).collect())
        }

        async fn state(&self, adapter: &usize) -> Result<CentralState, String> {
            let powered = self.adapters.lock().unwrap().iter().any(|a| a == &(*adapter, true));
            Ok(if powered { CentralState::PoweredOn } else { CentralState::PoweredOff })
        }
    }

    fn shared(stack: Fake) -> Shared<Fake> {
        Shared { stack, state: Mutex::default() }
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
    async fn reconnects_reuse_one_manager() {
        for keep in [false, true] {
            let s = shared(Fake::new(keep, &[(0, true)]));
            for _ in 0..50 {
                assert_eq!(s.adapter().await.ok(), Some(0));
            }
            assert_eq!(managers(&s), 1);
            assert_eq!(lists(&s), if keep { 1 } else { 50 });
        }
    }

    #[tokio::test]
    async fn bluetooth_off_keeps_the_manager() {
        for keep in [false, true] {
            let s = shared(Fake::new(keep, &[(0, false)]));
            for _ in 0..50 {
                assert!(is_off(s.adapter().await));
            }
            s.stack.plug(&[(0, true)]);
            assert_eq!(s.adapter().await.ok(), Some(0));
            assert_eq!(managers(&s), 1);
            assert_eq!(lists(&s), if keep { 1 } else { 51 });
        }
    }

    #[tokio::test]
    async fn no_adapter_asks_the_same_manager_again() {
        for keep in [false, true] {
            let s = shared(Fake::new(keep, &[]));
            for _ in 0..50 {
                assert!(matches!(s.adapter().await, Err(LinkError::NoAdapter(_))));
            }
            s.stack.plug(&[(7, true)]);
            assert_eq!(s.adapter().await.ok(), Some(7));
            assert_eq!(managers(&s), 1);
        }
    }

    #[tokio::test]
    async fn a_failed_manager_is_retried_until_one_exists() {
        let s = shared(Fake::new(false, &[(0, true)]));
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
        let s = shared(Fake::new(false, &[(0, true), (1, true)]));
        assert_eq!(s.adapter().await.ok(), Some(0));
        s.stack.plug(&[(1, true)]);
        assert_eq!(s.adapter().await.ok(), Some(1));
        assert_eq!(managers(&s), 1);
    }
}
