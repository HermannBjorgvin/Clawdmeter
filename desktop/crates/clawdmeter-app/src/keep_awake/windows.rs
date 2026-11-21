//! Windows: the Win32 half of [`clawdmeter_core::keep_awake::plan`].
//!
//! - [`WinPlan`] reads and writes the power plan through powrprof. The
//!   user's own plan is theirs to edit, so this needs no admin rights; a
//!   group policy on the lid action is reported, not worked around.
//! - [`Request`] is a `PowerRequestSystemRequired` power request, which
//!   holds off idle sleep and goes with the process. It can't hold off the
//!   lid; the plan does that.
//! - [`on_session_end`] restores the plan when Windows logs off or shuts
//!   down, which ends the process without running its quit path.

use std::ptr;
use std::sync::Mutex;

use clawdmeter_core::keep_awake::plan::{self, Plan, PlanHold, Setting, Source};
use clawdmeter_core::keep_awake::{Backend, Power, RestoreFile};
use clawdmeter_core::Paths;
use windows_sys::core::GUID;
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, LocalFree, ERROR_ACCESS_DISABLED_BY_POLICY, ERROR_SUCCESS, HANDLE, HWND,
    INVALID_HANDLE_VALUE, LPARAM, LRESULT, WPARAM,
};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::System::Power::{
    GetSystemPowerStatus, PowerClearRequest, PowerCreateRequest, PowerGetActiveScheme, PowerReadACValueIndex,
    PowerReadDCValueIndex, PowerRequestSystemRequired, PowerSetActiveScheme, PowerSetRequest, PowerSettingAccessCheck,
    PowerWriteACValueIndex, PowerWriteDCValueIndex, ACCESS_AC_POWER_SETTING_INDEX, ACCESS_DC_POWER_SETTING_INDEX,
    SYSTEM_POWER_STATUS,
};
use windows_sys::Win32::System::Threading::{POWER_REQUEST_CONTEXT_SIMPLE_STRING, REASON_CONTEXT, REASON_CONTEXT_0};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DispatchMessageW, GetMessageW, RegisterClassW, MSG, WM_ENDSESSION,
    WM_QUERYENDSESSION, WNDCLASSW,
};

/// "Power buttons and lid" and its "Lid close action".
const SUB_BUTTONS: GUID = GUID::from_u128(0x4f971e89_eebd_4455_a8de_9e59040e7347);
const LID_ACTION: GUID = GUID::from_u128(0x5ca83367_6e45_459f_a27b_476b1d01c936);
/// "Sleep" and its "Sleep after".
const SUB_SLEEP: GUID = GUID::from_u128(0x238c9fa8_0aad_41ed_83f4_97be242c8f20);
const SLEEP_AFTER: GUID = GUID::from_u128(0x29f6c1db_86da_48c5_9fdb_f2b67b1f44da);

/// What the power request and the session-end window say they are for.
const WHY: &str = "Keep awake with the lid closed";

/// A setting's subgroup and its own id.
fn ids(setting: Setting) -> (GUID, GUID) {
    match setting {
        Setting::LidAction => (SUB_BUTTONS, LID_ACTION),
        Setting::SleepAfter => (SUB_SLEEP, SLEEP_AFTER),
    }
}

fn guid(text: &str) -> Result<GUID, String> {
    let (data1, data2, data3, data4) = plan::guid_fields(text).ok_or_else(|| format!("not a power plan: {text}"))?;
    Ok(GUID { data1, data2, data3, data4 })
}

fn check(code: u32, call: &str) -> Result<(), String> {
    if code == ERROR_SUCCESS {
        Ok(())
    } else {
        Err(format!("{call} failed (error {code})"))
    }
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

/// The power plans, through powrprof.
struct WinPlan;

impl Plan for WinPlan {
    fn active(&mut self) -> Result<String, String> {
        let mut scheme: *mut GUID = ptr::null_mut();
        // SAFETY: the call writes one LocalAlloc'd GUID to `scheme`, read
        // once and freed here.
        unsafe {
            check(PowerGetActiveScheme(ptr::null_mut(), &mut scheme), "PowerGetActiveScheme")?;
            if scheme.is_null() {
                return Err("PowerGetActiveScheme returned no plan".into());
            }
            let g = *scheme;
            LocalFree(scheme.cast());
            Ok(plan::guid_text(g.data1, g.data2, g.data3, g.data4))
        }
    }

    fn read(&mut self, scheme: &str, setting: Setting, source: Source) -> Result<u32, String> {
        let (scheme, (sub, id)) = (guid(scheme)?, ids(setting));
        let mut value = 0u32;
        // SAFETY: every pointer is to a local that outlives the call.
        let code = unsafe {
            match source {
                Source::Ac => PowerReadACValueIndex(ptr::null_mut(), &scheme, &sub, &id, &mut value),
                Source::Dc => PowerReadDCValueIndex(ptr::null_mut(), &scheme, &sub, &id, &mut value),
            }
        };
        check(code, "reading the power plan").map(|()| value)
    }

    fn write(&mut self, scheme: &str, setting: Setting, source: Source, value: u32) -> Result<(), String> {
        let (scheme, (sub, id)) = (guid(scheme)?, ids(setting));
        // SAFETY: every pointer is to a local that outlives the call.
        let code = unsafe {
            match source {
                Source::Ac => PowerWriteACValueIndex(ptr::null_mut(), &scheme, &sub, &id, value),
                Source::Dc => PowerWriteDCValueIndex(ptr::null_mut(), &scheme, &sub, &id, value),
            }
        };
        check(code, "writing the power plan")
    }

    fn apply(&mut self, scheme: &str) -> Result<(), String> {
        let scheme = guid(scheme)?;
        // SAFETY: the pointer is to a local that outlives the call.
        check(unsafe { PowerSetActiveScheme(ptr::null_mut(), &scheme) }, "PowerSetActiveScheme")
    }

    fn locked(&mut self, setting: Setting, source: Source) -> bool {
        let (_, id) = ids(setting);
        let flags = match source {
            Source::Ac => ACCESS_AC_POWER_SETTING_INDEX,
            Source::Dc => ACCESS_DC_POWER_SETTING_INDEX,
        };
        // SAFETY: the pointer is to a local that outlives the call.
        unsafe { PowerSettingAccessCheck(flags, &id) == ERROR_ACCESS_DISABLED_BY_POLICY }
    }
}

/// A power request that holds off idle sleep until it is dropped. Windows
/// drops it with the process.
struct Request(HANDLE);

// SAFETY: a power request handle belongs to the process, not to a thread.
unsafe impl Send for Request {}

impl Request {
    fn take() -> Result<Request, String> {
        let mut why = wide(WHY);
        let context = REASON_CONTEXT {
            Version: 0,
            Flags: POWER_REQUEST_CONTEXT_SIMPLE_STRING,
            Reason: REASON_CONTEXT_0 { SimpleReasonString: why.as_mut_ptr() },
        };
        // SAFETY: `context` and the string it points to outlive the call,
        // which copies them; the handle is closed on every path.
        unsafe {
            let handle = PowerCreateRequest(&context);
            if handle.is_null() || handle == INVALID_HANDLE_VALUE {
                return Err(format!("PowerCreateRequest failed (error {})", GetLastError()));
            }
            if PowerSetRequest(handle, PowerRequestSystemRequired) == 0 {
                let code = GetLastError();
                CloseHandle(handle);
                return Err(format!("PowerSetRequest failed (error {code})"));
            }
            Ok(Request(handle))
        }
    }
}

impl Drop for Request {
    fn drop(&mut self) {
        // SAFETY: the handle is this request's own, open until here.
        unsafe {
            PowerClearRequest(self.0, PowerRequestSystemRequired);
            CloseHandle(self.0);
        }
    }
}

/// The Windows backend: the power plan for the lid, a power request for
/// idle sleep.
pub(super) struct PowerPlan {
    plan: PlanHold<WinPlan>,
    request: Option<Request>,
}

impl PowerPlan {
    pub(super) fn new(paths: &Paths) -> Self {
        PowerPlan { plan: PlanHold::new(WinPlan, RestoreFile::in_dir(&paths.state_dir)), request: None }
    }

    fn restore(&mut self) {
        if let Err(e) = self.plan.restore() {
            log::error!("keep awake: couldn't put the power plan back ({e}); it's tried again on the next start");
        }
    }
}

impl Backend for PowerPlan {
    fn heal(&mut self) {
        self.restore();
    }

    fn hold(&mut self) -> Result<(), String> {
        self.plan.hold()?;
        // The lid is held without it; idle sleep is then the plan's own
        // timeout.
        self.request = Request::take().map_err(|e| log::warn!("keep awake: idle sleep isn't held off ({e})")).ok();
        Ok(())
    }

    fn holding(&mut self) -> bool {
        self.plan.holding()
    }

    fn release(&mut self) {
        self.request = None;
        self.restore();
    }
}

/// The computer's power, from `GetSystemPowerStatus`.
pub(super) fn power() -> Power {
    // SAFETY: SYSTEM_POWER_STATUS is plain data the call fills in.
    unsafe {
        let mut status: SYSTEM_POWER_STATUS = std::mem::zeroed();
        if GetSystemPowerStatus(&mut status) == 0 {
            return Power::UNKNOWN;
        }
        plan::power(status.ACLineStatus, status.BatteryFlag, status.BatteryLifePercent)
    }
}

/// What to run when the Windows session ends.
static ON_END: Mutex<Option<Box<dyn Fn() + Send>>> = Mutex::new(None);

/// Run `f` when the user logs off or Windows shuts down, and wait for it:
/// Windows ends the process after that with no quit. It runs on a thread of
/// its own, which owns a hidden top-level window (a message-only window
/// isn't told about the end of the session).
pub(super) fn on_session_end(f: impl Fn() + Send + 'static) {
    if let Ok(mut slot) = ON_END.lock() {
        *slot = Some(Box::new(f));
    }
    let spawned = std::thread::Builder::new().name("clawdmeter-session-end".into()).spawn(|| {
        let class = wide("ClawdmeterSessionEnd");
        // SAFETY: a plain window class, window and message loop, all on this
        // thread; the class name outlives them, and MSG and WNDCLASSW are
        // plain data.
        unsafe {
            let instance = GetModuleHandleW(ptr::null());
            let mut wc: WNDCLASSW = std::mem::zeroed();
            wc.lpfnWndProc = Some(session_proc);
            wc.hInstance = instance;
            wc.lpszClassName = class.as_ptr();
            if RegisterClassW(&wc) == 0 {
                log::warn!("keep awake: no session-end window (RegisterClassW, error {})", GetLastError());
                return;
            }
            // Never shown: no WS_VISIBLE, no size.
            let window = CreateWindowExW(
                0,
                class.as_ptr(),
                class.as_ptr(),
                0,
                0,
                0,
                0,
                0,
                ptr::null_mut(),
                ptr::null_mut(),
                instance,
                ptr::null(),
            );
            if window.is_null() {
                log::warn!("keep awake: no session-end window (CreateWindowExW, error {})", GetLastError());
                return;
            }
            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, ptr::null_mut(), 0, 0) > 0 {
                DispatchMessageW(&msg);
            }
        }
    });
    if let Err(e) = spawned {
        log::warn!("keep awake: no session-end thread ({e}); a logoff is healed on the next start");
    }
}

unsafe extern "system" fn session_proc(window: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        // Fine by this app: the session may end.
        WM_QUERYENDSESSION => 1,
        WM_ENDSESSION => {
            // wparam: the session really is ending (nobody cancelled it).
            if wparam != 0 {
                if let Ok(slot) = ON_END.lock() {
                    if let Some(f) = slot.as_ref() {
                        f();
                    }
                }
            }
            0
        }
        // SAFETY: the arguments are the ones this procedure was called with.
        _ => unsafe { DefWindowProcW(window, msg, wparam, lparam) },
    }
}
