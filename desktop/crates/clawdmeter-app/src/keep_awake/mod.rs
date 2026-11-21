//! Keep the computer awake with the lid closed (`keep_awake`, the tray's
//! submenu): the OS half.
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
//!
//! Windows and macOS change something that outlives the process, so two
//! more things put it back when the process ends without its quit path:
//!
//! - the end of the OS session (Windows: `WM_ENDSESSION`) or of the app by
//!   AppKit (macOS: logout or a Quit from outside, which calls `exit`) asks
//!   the thread to let go and waits for it (`Handle::session_ender`);
//! - a guard, a child process started with each hold ([`GUARD_ARG`]), puts
//!   things back when the app dies without a word: a crash or a kill.
//!
//! What neither covers (a power cut, both processes killed at once) is
//! healed by the next start.

#[cfg(target_os = "linux")]
mod linux;
#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "windows")]
mod windows;

use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use clawdmeter_core::keep_awake::{Backend, Clock, Keeper, Left, Power, Readings, Status, Wish};
use clawdmeter_core::{unix_now, Paths};

/// How often the power source and the hold are looked at again.
const POLL: Duration = Duration::from_secs(15);

/// After the OS said the session is ending, nothing is held for this long.
/// A process still alive after it wasn't ended after all (a cancelled
/// shutdown, an installer that asked the app to close and gave up), and
/// holds again.
#[cfg(any(target_os = "windows", target_os = "macos", test))]
const SESSION_END_GRACE: Duration = Duration::from_secs(60);

/// The argument the app starts its guard with: `clawdmeter keep-awake-guard
/// <run>` ([`guard_main`]).
pub const GUARD_ARG: &str = "keep-awake-guard";

enum Msg {
    /// The setting changed, or the same choice was picked again: what it
    /// asks now, and the handle's count of these ([`Report`]).
    Want(Wish, u64),
    /// The OS session is ending (Windows: logoff or shutdown; macOS: AppKit
    /// is exiting the app), and the process with it, with no quit: let go
    /// and say when done.
    #[cfg(any(target_os = "windows", target_os = "macos", test))]
    SessionEnd(mpsc::Sender<()>),
    /// Quit: let go and end the thread.
    Stop,
}

/// What the thread last did, for the menu.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Report {
    /// What the keeper is doing.
    pub status: Status,
    /// What is left of a hold that ends by itself.
    pub left: Option<Left>,
    /// Which change of the setting this answers: a report from before the
    /// latest [`Handle::set`] or [`Handle::pick`] says nothing about it.
    change: u64,
}

#[cfg(test)]
impl Report {
    /// A report as the thread would write it (tests of the menu's text).
    pub fn of(status: Status, left: Option<Left>) -> Report {
        Report { status, left, change: 0 }
    }
}

/// The keep-awake thread: tell it the setting, read its status, and shut it
/// down on quit.
pub struct Handle {
    tx: mpsc::Sender<Msg>,
    report: Arc<Mutex<Report>>,
    /// The newest sign of Claude working (unix seconds; 0: none), for the
    /// thread to read on its own poll: a running turn moves it every tick,
    /// which must not wake the thread.
    work: Arc<AtomicI64>,
    wish: Wish,
    /// How many times the wish has changed.
    change: u64,
    thread: Option<JoinHandle<()>>,
}

impl Handle {
    /// Start the thread with what the saved setting asks. Its first tick
    /// deals with what an earlier run left behind, whatever the setting.
    pub fn spawn(paths: &Paths, wish: Wish) -> Handle {
        let handle = Self::spawn_with(platform_backend(paths), platform_power, wish);
        #[cfg(target_os = "windows")]
        windows::on_session_end(handle.session_ender());
        #[cfg(target_os = "macos")]
        macos::on_exit(handle.session_ender());
        handle
    }

    /// [`Handle::spawn`] over any backend and power reading (tests).
    /// `power` is one reading; `None` when the reading failed.
    fn spawn_with<B: Backend + Send + 'static>(backend: B, power: fn() -> Option<Power>, wish: Wish) -> Handle {
        let (tx, rx) = mpsc::channel();
        let report = Arc::new(Mutex::new(Report::default()));
        let work = Arc::new(AtomicI64::new(0));
        let (shared, seen_work) = (report.clone(), work.clone());
        let thread = std::thread::Builder::new()
            .name("clawdmeter-keep-awake".into())
            .spawn(move || {
                let mut keeper = Keeper::new(backend);
                let (mut wish, mut change) = (wish, 0);
                let mut readings = Readings::default();
                // The OS session is ending: nothing is held until then.
                #[cfg_attr(not(any(target_os = "windows", target_os = "macos", test)), allow(unused_mut))]
                let mut ended: Option<Instant> = None;
                loop {
                    let asked = if ended.is_none_or(|until| Instant::now() >= until) { wish } else { Wish::Off };
                    // Nothing to read while it's off: no battery polling for
                    // a feature nobody switched on. What was read before it
                    // went off is forgotten, so hours later a reading that
                    // fails doesn't stand on one from then.
                    let now_power = if asked == Wish::Off {
                        readings = Readings::default();
                        Power::UNKNOWN
                    } else {
                        readings.settle(power())
                    };
                    let clock = Clock { now: Instant::now(), unix: unix_now() };
                    let work_at = Some(seen_work.load(Ordering::Relaxed)).filter(|at| *at != 0);
                    let status = keeper.run(asked, now_power, clock, work_at).clone();
                    let left = keeper.left(clock);
                    if let Ok(mut r) = shared.lock() {
                        *r = Report { status, left, change };
                    }
                    match rx.recv_timeout(POLL) {
                        Ok(Msg::Want(w, n)) => {
                            // The same wish again is a new pick of it.
                            if w == wish {
                                keeper.start_over();
                            }
                            (wish, change) = (w, n);
                        }
                        #[cfg(any(target_os = "windows", target_os = "macos", test))]
                        Ok(Msg::SessionEnd(done)) => {
                            ended = Some(Instant::now() + SESSION_END_GRACE);
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
        Handle { tx, report, work, wish, change: 0, thread }
    }

    /// What the setting asks now; a change is acted on at once.
    pub fn set(&mut self, wish: Wish) {
        if self.wish != wish {
            self.pick(wish);
        }
    }

    /// A choice the user just made, acted on at once. Picking what is
    /// already on starts it over: "Until Claude stops working" gets its
    /// full idle time again, and a choice that just ended holds again.
    pub fn pick(&mut self, wish: Wish) {
        self.wish = wish;
        self.change += 1;
        let _ = self.tx.send(Msg::Want(wish, self.change));
    }

    /// The newest sign of Claude working the engine has (unix seconds), for
    /// "until Claude stops working". Read on the thread's next poll.
    pub fn note_work(&self, at: Option<i64>) {
        self.work.store(at.unwrap_or(0), Ordering::Relaxed);
    }

    /// What the thread last did about what the setting asks now. Until it
    /// has looked at the latest change there is nothing to say: the note
    /// of the choice before isn't shown under the new one.
    pub fn report(&self) -> Report {
        let report = self.report.lock().map(|r| r.clone()).unwrap_or_default();
        if report.change == self.change {
            report
        } else {
            Report { change: self.change, ..Report::default() }
        }
    }

    /// The thread has looked at the latest change of the setting.
    #[cfg(test)]
    fn answered(&self) -> bool {
        self.report.lock().is_ok_and(|r| r.change == self.change)
    }

    /// What the keeper is doing.
    pub fn status(&self) -> Status {
        self.report().status
    }

    /// The time of what the setting asks now is up (a timer's deadline, or
    /// Claude idle for long enough): the caller switches the setting off.
    /// Never true of a wish the thread hasn't looked at yet.
    pub fn ended(&self) -> bool {
        self.report().status == Status::Ended
    }

    /// Let go and wait for the thread, so the OS is as it was before the
    /// process exits.
    pub fn shutdown(mut self) {
        self.stop();
    }

    /// What to call when the OS session ends: it lets go and returns once
    /// the OS is as it was (or after a few seconds, so a stuck call can't
    /// hold up the logoff).
    #[cfg(any(target_os = "windows", target_os = "macos", test))]
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

/// The guard: `clawdmeter keep-awake-guard <run>`, a child the app starts
/// with each hold that changes something lasting (Windows, macOS). It waits
/// on its stdin, a pipe from the app. A byte means the app let go itself:
/// nothing to do. The pipe closing with no byte means the app died holding
/// (a crash, a kill, an exit that skipped the release), and the guard puts
/// the OS back from the restore record, if the record is still that run's
/// (`run`, the app's `run_id`): a later run's hold is not its to undo.
pub fn guard_main(paths: &Paths, run: Option<&str>) -> i32 {
    // A guard started by hand names no run and heals nothing.
    if let (true, Some(run)) = (app_died(std::io::stdin()), run) {
        // Nobody to tell if it fails: the next start tries again.
        let _ = platform_backend(paths).heal_for(run);
    }
    0
}

/// Whether the guard's pipe says the app died: closed with nothing written.
/// A byte is the app's dismissal, and a read error isn't a death.
fn app_died(mut pipe: impl std::io::Read) -> bool {
    let mut word = [0u8; 1];
    matches!(pipe.read(&mut word), Ok(0))
}

/// The lock every process takes around reading the restore record, acting
/// on it and rewriting it: `<state>/keep-awake-restore.lock`, held while
/// the returned file is open. With it a guard's "is this record my run's?
/// then restore it" and a new run's hold can't interleave: whichever comes
/// second sees what the first left. A holder has it for a few file and
/// power calls, so the wait is bounded ([`LOCK_WAIT`]): a holder stuck in
/// one must not hang this process's quit. `None` (the lock can't be had, or
/// not in time): go on without, as the record's owner is still checked on
/// the copy acted on.
#[cfg(any(target_os = "windows", target_os = "macos", test))]
fn record_lock(state_dir: &std::path::Path) -> Option<std::fs::File> {
    let open = std::fs::create_dir_all(state_dir).and_then(|()| {
        let path = state_dir.join("keep-awake-restore.lock");
        std::fs::OpenOptions::new().create(true).truncate(false).write(true).open(path)
    });
    let file = open.map_err(|e| log::debug!("keep awake: no lock on the restore record ({e})")).ok()?;
    let give_up = Instant::now() + LOCK_WAIT;
    loop {
        match file.try_lock() {
            Ok(()) => return Some(file),
            Err(std::fs::TryLockError::WouldBlock) if Instant::now() < give_up => {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(e) => {
                log::warn!("keep awake: going on without the restore record's lock ({e})");
                return None;
            }
        }
    }
}

/// How long [`record_lock`] waits for another process to let go of it.
#[cfg(any(target_os = "windows", target_os = "macos", test))]
const LOCK_WAIT: Duration = Duration::from_secs(3);

/// A name for this run of the app, for its restore records and its guard.
#[cfg(any(target_os = "windows", target_os = "macos"))]
fn run_id() -> String {
    format!("{}-{}", std::process::id(), unix_now())
}

/// The app's end of its guard ([`guard_main`]).
#[cfg(any(target_os = "windows", target_os = "macos"))]
struct Guard(Option<std::process::Child>);

#[cfg(any(target_os = "windows", target_os = "macos"))]
impl Guard {
    /// Start a guard for the records of `run`. Without one a crash is
    /// healed by the next start.
    fn start(run: &str) -> Option<Guard> {
        use std::process::{Command, Stdio};
        let child = std::env::current_exe().and_then(|exe| {
            let mut guard = Command::new(exe);
            guard.arg(GUARD_ARG).arg(run).stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null());
            // A group of its own: launchd signals what is left of a dead
            // job's process group, and the guard's work starts at that death.
            #[cfg(unix)]
            std::os::unix::process::CommandExt::process_group(&mut guard, 0);
            guard.spawn()
        });
        match child {
            Ok(child) => Some(Guard(Some(child))),
            Err(e) => {
                log::warn!("keep awake: no guard process ({e}); a crash is healed on the next start");
                None
            }
        }
    }

    /// The app let go itself: tell the guard so, and it leaves.
    fn dismiss(mut self) {
        use std::io::Write;
        if let Some(stdin) = self.0.as_mut().and_then(|child| child.stdin.as_mut()) {
            let _ = stdin.write_all(b"q");
        }
    }
}

#[cfg(any(target_os = "windows", target_os = "macos"))]
impl Drop for Guard {
    /// Closes the pipe. A guard that wasn't dismissed takes that as the
    /// app's death and puts the OS back, which is right for a hold dropped
    /// any other way.
    fn drop(&mut self) {
        if let Some(mut child) = self.0.take() {
            drop(child.stdin.take());
            // Reaped off this thread: the guard may be busy restoring.
            let _ = std::thread::Builder::new().name("clawdmeter-guard-reap".into()).spawn(move || {
                let _ = child.wait();
            });
        }
    }
}

#[cfg(target_os = "linux")]
fn platform_backend(_paths: &Paths) -> impl Backend + Send + 'static {
    linux::Logind::default()
}

#[cfg(target_os = "linux")]
fn platform_power() -> Option<Power> {
    linux::power()
}

#[cfg(target_os = "windows")]
fn platform_backend(paths: &Paths) -> impl Backend + Send + 'static {
    windows::PowerPlan::new(paths)
}

#[cfg(target_os = "windows")]
fn platform_power() -> Option<Power> {
    windows::power()
}

#[cfg(target_os = "macos")]
fn platform_backend(paths: &Paths) -> impl Backend + Send + 'static {
    macos::Clamshell::new(paths)
}

#[cfg(target_os = "macos")]
fn platform_power() -> Option<Power> {
    macos::power()
}

/// An OS with nothing to hold: the menu says so.
#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
struct Unsupported;

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
impl Backend for Unsupported {
    fn hold(&mut self) -> Result<(), String> {
        Err("not supported on this system".into())
    }
    fn release(&mut self) -> Result<(), String> {
        Ok(())
    }
}

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
fn platform_backend(_paths: &Paths) -> impl Backend + Send + 'static {
    Unsupported
}

#[cfg(not(any(target_os = "linux", target_os = "windows", target_os = "macos")))]
fn platform_power() -> Option<Power> {
    Some(Power::UNKNOWN)
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::AtomicUsize;
    use std::time::Instant;

    use super::*;

    /// Counts holds and releases; nothing of the OS is touched.
    struct Counting(Arc<AtomicUsize>, Arc<AtomicUsize>);

    impl Backend for Counting {
        fn hold(&mut self) -> Result<(), String> {
            self.0.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
        fn release(&mut self) -> Result<(), String> {
            self.1.fetch_add(1, Ordering::SeqCst);
            Ok(())
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
        let mut h = Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Some(Power::UNKNOWN), Wish::Off);
        assert_eq!(h.status(), Status::Off);
        h.set(Wish::Forever);
        wait_for(|| h.status() == Status::Holding);
        assert_eq!((holds.load(Ordering::SeqCst), releases.load(Ordering::SeqCst)), (1, 0));
        h.set(Wish::Off);
        wait_for(|| h.answered() && h.status() == Status::Off);
        assert_eq!(releases.load(Ordering::SeqCst), 1, "unticking lets go at once, not at the next poll");
        h.set(Wish::Forever);
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
        let h = Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Some(Power::UNKNOWN), Wish::Forever);
        wait_for(|| h.status() == Status::Holding);
        drop(h);
        assert_eq!((holds.load(Ordering::SeqCst), releases.load(Ordering::SeqCst)), (1, 1));
    }

    #[test]
    fn the_end_of_the_os_session_lets_go_before_it_returns() {
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let mut h =
            Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Some(Power::UNKNOWN), Wish::Forever);
        wait_for(|| h.status() == Status::Holding);
        let end = h.session_ender();
        end();
        assert_eq!(releases.load(Ordering::SeqCst), 1, "released by the time the session-end call returns");
        wait_for(|| h.status() == Status::Off);
        // The process is about to be ended: nothing is taken again for now.
        h.set(Wish::Off);
        h.set(Wish::Forever);
        std::thread::sleep(Duration::from_millis(50));
        assert_eq!((holds.load(Ordering::SeqCst), h.status()), (1, Status::Off));
    }

    #[test]
    fn the_guard_acts_only_on_its_apps_death() {
        struct Broken;
        impl std::io::Read for Broken {
            fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
                Err(std::io::Error::other("no pipe"))
            }
        }
        assert!(app_died(&b""[..]), "the pipe closed with nothing said: the app is gone");
        assert!(!app_died(&b"q"[..]), "dismissed: the app let go itself");
        assert!(!app_died(Broken), "an error isn't a death");
    }

    #[test]
    fn the_record_lock_lets_one_holder_in_at_a_time() {
        let dir = std::env::temp_dir().join(format!("cm-keep-awake-lock-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let first = record_lock(&dir).expect("the lock");
        let entered = Arc::new(AtomicUsize::new(0));
        let (seen, there) = (entered.clone(), dir.clone());
        // A guard, say, while the app is in the middle of a hold.
        let second = std::thread::spawn(move || {
            let lock = record_lock(&there).expect("the lock");
            seen.store(1, Ordering::SeqCst);
            drop(lock);
        });
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(entered.load(Ordering::SeqCst), 0, "it waits for the first to finish");
        drop(first);
        second.join().unwrap();
        assert_eq!(entered.load(Ordering::SeqCst), 1);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_timer_that_is_over_reports_ended_for_the_wish_it_was_asked() {
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        // A restart after the deadline: nothing is held, and the tray is
        // told to switch the setting off.
        let past = Wish::Until(unix_now() - 60);
        let mut h = Handle::spawn_with(Counting(holds.clone(), releases.clone()), || Some(Power::UNKNOWN), past);
        wait_for(|| h.ended());
        assert_eq!(holds.load(Ordering::SeqCst), 0);
        assert_eq!(h.report().left, None);
        // A new pick: the old "ended" says nothing about it.
        h.set(Wish::Until(unix_now() + 3600));
        assert!(!h.ended(), "not before the thread has looked at the new wish");
        wait_for(|| h.status() == Status::Holding);
        assert!(!h.ended());
        assert!(matches!(h.report().left, Some(Left::Time(secs)) if (3500..=3600).contains(&secs)), "{:?}", h.report());
        h.set(Wish::Off);
        wait_for(|| h.answered() && h.status() == Status::Off);
        assert_eq!((holds.load(Ordering::SeqCst), releases.load(Ordering::SeqCst)), (1, 1));
    }

    #[test]
    fn picking_an_ended_choice_again_holds_again() {
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let over = Wish::Until(unix_now() - 60);
        let mut h = Handle::spawn_with(Counting(holds.clone(), releases), || Some(Power::UNKNOWN), over);
        wait_for(|| h.ended());
        // The setting saying the same thing again changes nothing.
        h.set(over);
        assert!(h.ended());
        // A pick is acted on, also of what is already on: the old "ended"
        // doesn't answer it.
        h.pick(over);
        assert!(!h.ended(), "not before the thread has looked at the pick");
        wait_for(|| h.ended());
        h.pick(Wish::WhileClaudeWorks);
        wait_for(|| h.status() == Status::Holding);
        h.note_work(Some(5));
        h.pick(Wish::WhileClaudeWorks);
        wait_for(|| h.answered() && h.status() == Status::Holding);
        assert_eq!(holds.load(Ordering::SeqCst), 1, "the hold stood through the second pick");
    }

    // Plugged in when it was last on; switched on again much later, on
    // battery, with a reading that fails: nothing from back then stands in.
    #[test]
    fn a_reading_from_before_it_was_off_doesnt_stand_in_later() {
        use clawdmeter_core::keep_awake::Pause;
        static FAILING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
        fn power() -> Option<Power> {
            (!FAILING.load(Ordering::SeqCst)).then_some(Power { external: Some(true), battery_pct: Some(90) })
        }
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let mut h = Handle::spawn_with(Counting(holds.clone(), releases), power, Wish::Forever);
        wait_for(|| h.status() == Status::Holding);
        h.set(Wish::Off);
        wait_for(|| h.answered() && h.status() == Status::Off);
        FAILING.store(true, Ordering::SeqCst);
        h.set(Wish::Forever);
        wait_for(|| h.answered());
        assert_eq!(h.status(), Status::Paused(Pause::BatteryUnknown));
        assert_eq!(holds.load(Ordering::SeqCst), 1, "not held again on the old reading");
    }

    #[test]
    fn low_battery_shows_as_paused() {
        use clawdmeter_core::keep_awake::Pause;
        let (holds, releases) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let low = || Some(Power { external: Some(false), battery_pct: Some(7) });
        let h = Handle::spawn_with(Counting(holds.clone(), releases), low, Wish::Forever);
        wait_for(|| h.status() == Status::Paused(Pause::BatteryLow(7)));
        assert_eq!(holds.load(Ordering::SeqCst), 0, "never held under the floor");
    }
}
