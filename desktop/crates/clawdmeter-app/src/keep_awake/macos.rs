//! macOS: the IOKit half of [`clawdmeter_core::keep_awake::clamshell`].
//!
//! - [`RootDomain`] sets the power root domain's lid flag
//!   (`kPMSetClamshellSleepState`, a method of its user client that asks for
//!   no privilege or entitlement). It is not a documented API: on some Macs
//!   and macOS versions it is reported to have no effect, which is why the
//!   menu item says "experimental".
//! - [`Assertion`] is a public `PreventUserIdleSystemSleep` assertion, which
//!   holds off idle sleep and goes with the process. It can't hold off the
//!   lid; the flag does that.
//! - [`power`] reads `pmset -g batt` (a read; nothing is changed).
//! - [`on_exit`] clears the flag when AppKit exits the app (logout, or a
//!   Quit from outside), which skips the tray's quit path.
//! - A guard process ([`super::guard_main`]) clears it when the app is
//!   killed or crashes.

use std::ffi::{c_char, c_int, c_void};
use std::process::Command;
use std::ptr;
use std::sync::Mutex;

use clawdmeter_core::keep_awake::clamshell::{self, Lid, LidHold};
use clawdmeter_core::keep_awake::{Backend, Check, Power, RestoreFile};
use clawdmeter_core::Paths;
use objc2::rc::Retained;
use objc2_foundation::NSString;

/// A Mach port: an IOKit object or connection.
type Port = u32;
/// `kern_return_t` / `IOReturn`: 0 is success.
type Return = i32;

/// `kPMSetClamshellSleepState`, in the root domain user client's method
/// table (IOKit's `IOPMLibDefs.h`): one scalar in, 1 to stop the lid from
/// sleeping the Mac and 0 to let it.
const SET_CLAMSHELL_SLEEP_STATE: u32 = 12;
/// `kIOPMAssertionLevelOn`.
const ASSERTION_ON: u32 = 255;

#[link(name = "IOKit", kind = "framework")]
extern "C" {
    fn IOServiceMatching(name: *const c_char) -> *mut c_void;
    fn IOServiceGetMatchingService(main_port: Port, matching: *mut c_void) -> Port;
    fn IOServiceOpen(service: Port, owning_task: Port, kind: u32, connect: *mut Port) -> Return;
    fn IOServiceClose(connect: Port) -> Return;
    fn IOObjectRelease(object: Port) -> Return;
    fn IOConnectCallScalarMethod(
        connect: Port,
        selector: u32,
        input: *const u64,
        input_count: u32,
        output: *mut u64,
        output_count: *mut u32,
    ) -> Return;
    fn IOPMAssertionCreateWithName(kind: *const c_void, level: u32, name: *const c_void, id: *mut u32) -> Return;
    fn IOPMAssertionRelease(id: u32) -> Return;
}

extern "C" {
    /// This task's port (what C's `mach_task_self()` expands to).
    #[link_name = "mach_task_self_"]
    static MACH_TASK_SELF: Port;

    /// libc's: run `callback` when the process calls `exit`.
    fn atexit(callback: extern "C" fn()) -> c_int;
}

/// The power root domain's lid flag.
struct RootDomain;

impl Lid for RootDomain {
    fn set_sleep_disabled(&mut self, disabled: bool) -> Result<(), String> {
        // SAFETY: plain IOKit calls. IOServiceGetMatchingService consumes the
        // matching dictionary; the service and the connection are this
        // function's own and released on every path.
        unsafe {
            let matching = IOServiceMatching(c"IOPMrootDomain".as_ptr());
            if matching.is_null() {
                return Err("no IOPMrootDomain".into());
            }
            let service = IOServiceGetMatchingService(0, matching);
            if service == 0 {
                return Err("no IOPMrootDomain".into());
            }
            let mut connect: Port = 0;
            let opened = IOServiceOpen(service, MACH_TASK_SELF, 0, &mut connect);
            IOObjectRelease(service);
            if opened != 0 {
                return Err(format!("IOServiceOpen failed ({opened:#x})"));
            }
            let input = [u64::from(disabled)];
            let set = IOConnectCallScalarMethod(
                connect,
                SET_CLAMSHELL_SLEEP_STATE,
                input.as_ptr(),
                1,
                ptr::null_mut(),
                ptr::null_mut(),
            );
            IOServiceClose(connect);
            if set != 0 {
                return Err(format!("setting the lid flag failed ({set:#x})"));
            }
            Ok(())
        }
    }
}

/// A power assertion that holds off idle sleep until it is dropped. macOS
/// drops it with the process.
struct Assertion(u32);

impl Assertion {
    fn take() -> Result<Assertion, String> {
        let kind = NSString::from_str("PreventUserIdleSystemSleep");
        let name = NSString::from_str("Clawdmeter: keep awake with the lid closed");
        let mut id = 0u32;
        // SAFETY: NSString is toll-free bridged to CFString, and both
        // strings outlive the call.
        let made = unsafe {
            IOPMAssertionCreateWithName(
                Retained::as_ptr(&kind).cast(),
                ASSERTION_ON,
                Retained::as_ptr(&name).cast(),
                &mut id,
            )
        };
        if made != 0 {
            return Err(format!("IOPMAssertionCreateWithName failed ({made:#x})"));
        }
        Ok(Assertion(id))
    }
}

impl Drop for Assertion {
    fn drop(&mut self) {
        // SAFETY: the id is this assertion's own, held until here.
        unsafe {
            IOPMAssertionRelease(self.0);
        }
    }
}

/// The macOS backend: the lid flag for the lid, an assertion for idle sleep.
pub(super) struct Clamshell {
    lid: LidHold<RootDomain>,
    idle: Option<Assertion>,
    /// Clears the flag if this process dies holding.
    guard: Option<super::Guard>,
    /// This run's name, in its restore records and its guard's argument.
    run: String,
    /// Where the marker and its lock are.
    state_dir: std::path::PathBuf,
}

impl Clamshell {
    pub(super) fn new(paths: &Paths) -> Self {
        let run = super::run_id();
        let file = RestoreFile::in_dir(&paths.state_dir).owned_by(run.clone());
        Clamshell {
            lid: LidHold::new(RootDomain, file),
            idle: None,
            guard: None,
            run,
            state_dir: paths.state_dir.clone(),
        }
    }
}

impl Backend for Clamshell {
    fn heal(&mut self) -> Result<(), String> {
        let _lock = super::record_lock(&self.state_dir);
        self.lid.restore()
    }

    fn heal_for(&mut self, run: &str) -> Result<(), String> {
        let _lock = super::record_lock(&self.state_dir);
        self.lid.restore_for(Some(run))
    }

    fn hold(&mut self) -> Result<(), String> {
        // The guard first, so no moment of the flag is without one.
        if self.guard.is_none() {
            self.guard = super::Guard::start(&self.run);
        }
        let _lock = super::record_lock(&self.state_dir);
        self.lid.hold()?;
        // The lid is held without it; idle sleep is then the Mac's own
        // timeout.
        self.idle = Assertion::take().map_err(|e| log::warn!("keep awake: idle sleep isn't held off ({e})")).ok();
        Ok(())
    }

    fn caveat(&self) -> Option<String> {
        Some("experimental".into())
    }

    fn check(&mut self) -> Check {
        self.lid.renew()
    }

    fn release(&mut self) -> Result<(), String> {
        self.idle = None;
        let _lock = super::record_lock(&self.state_dir);
        // A failed clear keeps its guard: if this process dies before the
        // next try, the guard makes one.
        self.lid.restore()?;
        if let Some(guard) = self.guard.take() {
            guard.dismiss();
        }
        Ok(())
    }
}

/// What to run when the process exits.
static ON_EXIT: Mutex<Option<Box<dyn Fn() + Send>>> = Mutex::new(None);

/// Run `f` when the process calls `exit` without the tray's quit path.
/// AppKit does on logout and on a Quit from outside (the Dock, Activity
/// Monitor, an Apple event): `-[NSApplication terminate:]` ends in `exit`,
/// and nothing after `NSApplication::run` ever runs.
pub(super) fn on_exit(f: impl Fn() + Send + 'static) {
    extern "C" fn run() {
        // try_lock: `exit` may come from any thread, and never waits here.
        if let Ok(slot) = ON_EXIT.try_lock() {
            if let Some(f) = slot.as_ref() {
                f();
            }
        }
    }
    if let Ok(mut slot) = ON_EXIT.lock() {
        *slot = Some(Box::new(f));
    }
    // SAFETY: `run` is a plain function that lives as long as the process.
    if unsafe { atexit(run) } != 0 {
        log::warn!("keep awake: no exit hook; a Quit from outside the app is left to the guard");
    }
}

/// The Mac's power, from `pmset -g batt`. A read that fails counts as on
/// battery with no charge reading, which pauses: it is not a desktop.
pub(super) fn power() -> Power {
    match Command::new("/usr/bin/pmset").args(["-g", "batt"]).output() {
        Ok(out) if out.status.success() => clamshell::power_from_pmset(&String::from_utf8_lossy(&out.stdout)),
        other => {
            log::debug!("keep awake: pmset -g batt failed: {other:?}");
            Power { external: Some(false), battery_pct: None }
        }
    }
}
