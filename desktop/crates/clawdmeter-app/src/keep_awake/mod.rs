//! Keep the computer awake with the lid closed (`keep_awake`, the tray's
//! checkbox): the OS half.
//!
//! [`clawdmeter_core::keep_awake`] decides when to hold and when to let go
//! (the battery floor); this module has the thread that runs it and one
//! [`Backend`] and power reading per OS:
//!
//! - Linux (`linux`): a systemd-logind `handle-lid-switch` inhibitor lock,
//!   which logind drops when the process dies, and the desktop's own suspend
//!   inhibitor, which goes with the D-Bus connection. Nothing persistent is
//!   changed.
//! - Windows (`windows`): the active power plan's lid action set to "Do
//!   nothing", recorded first and put back after
//!   ([`clawdmeter_core::keep_awake::plan`]), and a power request for idle
//!   sleep.
//! - macOS (`macos`): the kernel's lid flag, marked first and cleared after
//!   ([`clawdmeter_core::keep_awake::clamshell`]), and a power assertion
//!   for idle sleep.
//!
//! The thread owns every OS call, so a slow D-Bus or power call never holds
//! up the tray's tick. Quit joins it ([`Handle::shutdown`]), so the hold is
//! released before the process exits.

#[cfg(target_os = "linux")]
mod linux;
#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "windows")]
mod windows;

use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use clawdmeter_core::keep_awake::{Backend, Keeper, Power, Status};
use clawdmeter_core::{unix_now, Paths};

/// How often the power source and the hold are looked at again.
const POLL: Duration = Duration::from_secs(15);

enum Msg {
    /// The setting changed.
    Want(bool),
    /// The OS session is ending (Windows: logoff or shutdown), and the
    /// process with it, with no quit: let go for good and say when done.
    #[cfg(any(target_os = "windows", test))]
    SessionEnd(mpsc::Sender<()>),
    /// Quit: let go and end the thread.
    Stop,
}

/// The keep-awake thread: tell it the setting, read its status, and shut it
/// down on quit.
pub struct Handle {
    tx: mpsc::Sender<Msg>,
    status: Arc<Mutex<Status>>,
    wanted: bool,
    thread: Option<JoinHandle<()>>,
}

impl Handle {
    /// Start the thread with the setting as saved. Its first tick deals
    /// with what an earlier run left behind, whatever the setting.
    pub fn spawn(paths: &Paths, wanted: bool) -> Handle {
        let handle = Self::spawn_with(platform_backend(paths), platform_power, wanted);
        #[cfg(target_os = "windows")]
        windows::on_session_end(handle.session_ender());
        handle
    }

    /// [`Handle::spawn`] over any backend and power reading (tests).
    fn spawn_with<B: Backend + Send + 'static>(backend: B, power: fn() -> Power, wanted: bool) -> Handle {
        let (tx, rx) = mpsc::channel();
        let status = Arc::new(Mutex::new(Status::Off));
        let shared = status.clone();
        let thread = std::thread::Builder::new()
            .name("clawdmeter-keep-awake".into())
            .spawn(move || {
                let mut keeper = Keeper::new(backend);
                let mut wanted = wanted;
                // The OS session is over: nothing is held again.
                #[cfg_attr(not(any(target_os = "windows", test)), allow(unused_mut))]
                let mut ended = false;
                loop {
                    let hold = wanted && !ended;
                    // Nothing to read while it's off: no battery polling for
                    // a feature nobody switched on.
                    let now_power = if hold { power() } else { Power::UNKNOWN };
                    let status = keeper.tick(hold, now_power, unix_now()).clone();
                    if let Ok(mut s) = shared.lock() {
                        *s = status;
                    }
                    match rx.recv_timeout(POLL) {
                        Ok(Msg::Want(w)) => wanted = w,
                        #[cfg(any(target_os = "windows", test))]
                        Ok(Msg::SessionEnd(done)) => {
                            ended = true;
                            keeper.stop();
                            let _ = done.send(());
                        }
                        Err(RecvTimeoutError::Timeout) => {}
                        // A handle dropped without a shutdown is a quit too.
                        Ok(Msg::Stop) | Err(RecvTimeoutError::Disconnected) => break,
                    }
                }
                keeper.stop();
            })
            .map_err(|e| log::error!("keep awake: couldn't start its thread: {e}"))
            .ok();
        Handle { tx, status, wanted, thread }
    }

    /// The setting as it is now; a change is acted on at once.
    pub fn set(&mut self, wanted: bool) {
        if self.wanted != wanted {
            self.wanted = wanted;
            let _ = self.tx.send(Msg::Want(wanted));
        }
    }

    /// What the thread last did.
    pub fn status(&self) -> Status {
        self.status.lock().map(|s| s.clone()).unwrap_or_default()
    }

    /// Let go and wait for the thread, so the OS is as it was before the
    /// process exits.
    pub fn shutdown(mut self) {
        self.stop();
    }

    /// What to call when the OS session ends: it lets go and returns once
    /// the OS is as it was (or after a few seconds, so a stuck call can't
    /// hold up the logoff).
    #[cfg(any(target_os = "windows", test))]
    fn session_ender(&self) -> impl Fn() + Send + 'static {
        let tx = self.tx.clone();
        move || {
            let (done, wait) = mpsc::channel();
            if tx.send(Msg::SessionEnd(done)).is_ok() {
                let _ = wait.recv_timeout(Duration::from_secs(4));
            }
        }
    }

    fn stop(&mut self) {
        let _ = self.tx.send(Msg::Stop);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(target_os = "linux")]
fn platform_backend(_paths: &Paths) -> impl Backend + Send + 'static {
    linux::Logind::default()
}

#[cfg(target_os = "linux")]
fn platform_power() -> Power {
    linux::power()
}

#[cfg(target_os = "windows")]
fn platform_backend(paths: &Paths) -> impl Backend + Send + 'static {
    windows::PowerPlan::new(paths)
}

#[cfg(target_os = "windows")]
fn platform_power() -> Power {
    windows::power()
}

#[cfg(target_os = "macos")]
fn platform_backend(paths: &Paths) -> impl Backend + Send + 'static {
    macos::Clamshell::new(paths)
}

#[cfg(target_os = "macos")]
fn platform_power() -> Power {
    macos::power()
}

/// An OS with nothing to hold: the checkbox says so.
#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
struct Unsupported;

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
impl Backend for Unsupported {
    fn hold(&mut self) -> Result<(), String> {
        Err("not supported on this system".into())
    }
    fn release(&mut self) {}
}

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
fn platform_backend(_paths: &Paths) -> impl Backend + Send + 'static {
    Unsupported
}

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
fn platform_power() -> Power {
    Power::UNKNOWN
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::time::Instant;

    use super::*;

    /// Counts holds and releases; nothing of the OS is touched.
    struct Counting(Arc<AtomicUsize>, Arc<AtomicUsize>);

    impl Backend for Counting {
        fn hold(&mut self) -> Result<(), String> {
            self.0.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
        fn release(&mut self) {
            self.1.fetch_add(1, Ordering::SeqCst);
        }
    }

    fn wait_for(what: impl Fn() -> bool) {
        let start = Instant::now();
        while !what() {
            assert!(start.elapsed() < Duration::from_secs(5), "timed out");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    #[test]
    fn the_thread_follows_the_setting_and_releases_on_quit() {
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let mut h = Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Power::UNKNOWN, false);
        assert_eq!(h.status(), Status::Off);
        h.set(true);
        wait_for(|| h.status() == Status::Holding);
        assert_eq!((holds.load(Ordering::SeqCst), releases.load(Ordering::SeqCst)), (1, 0));
        h.set(false);
        wait_for(|| h.status() == Status::Off);
        assert_eq!(releases.load(Ordering::SeqCst), 1, "unticking lets go at once, not at the next poll");
        h.set(true);
        wait_for(|| h.status() == Status::Holding);
        h.shutdown();
        assert_eq!(
            (holds.load(Ordering::SeqCst), releases.load(Ordering::SeqCst)),
            (2, 2),
            "quit waits for the release"
        );
    }

    #[test]
    fn a_saved_setting_holds_from_the_start_and_a_dropped_handle_lets_go() {
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let h = Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Power::UNKNOWN, true);
        wait_for(|| h.status() == Status::Holding);
        drop(h);
        assert_eq!((holds.load(Ordering::SeqCst), releases.load(Ordering::SeqCst)), (1, 1));
    }

    #[test]
    fn the_end_of_the_os_session_lets_go_before_it_returns() {
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let mut h = Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Power::UNKNOWN, true);
        wait_for(|| h.status() == Status::Holding);
        let end = h.session_ender();
        end();
        assert_eq!(releases.load(Ordering::SeqCst), 1, "released by the time the session-end call returns");
        wait_for(|| h.status() == Status::Off);
        // The process is about to be ended: nothing is taken again.
        h.set(false);
        h.set(true);
        std::thread::sleep(Duration::from_millis(50));
        assert_eq!((holds.load(Ordering::SeqCst), h.status()), (1, Status::Off));
    }

    #[test]
    fn low_battery_shows_as_paused() {
        use clawdmeter_core::keep_awake::Pause;
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let low = || Power { external: Some(false), battery_pct: Some(7) };
        let h = Handle::spawn_with(Counting(holds.clone(), releases), low, true);
        wait_for(|| h.status() == Status::Paused(Pause::BatteryLow(7)));
        assert_eq!(holds.load(Ordering::SeqCst), 0, "never held under the floor");
    }
}
