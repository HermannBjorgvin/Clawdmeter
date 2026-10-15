//! The main-thread loop the tray icon needs, calling `tick` about every
//! 200 ms until it returns false, then `on_quit`.
//!
//! - Linux: the tray is a D-Bus StatusNotifierItem served by `ksni` on its
//!   own thread, so the main thread only has to tick.
//! - Windows: a Win32 message loop with a thread timer (tray-icon's hidden
//!   window lives on this thread).
//! - macOS: NSApplication (accessory policy: no Dock icon) with a GCD timer
//!   hopping onto the main queue.

use std::time::Duration;

pub(super) type Tick = Box<dyn FnMut() -> bool>;
pub(super) type OnQuit = Box<dyn FnOnce()>;

const INTERVAL: Duration = Duration::from_millis(200);

#[cfg(not(any(target_os = "windows", target_os = "macos")))]
pub(super) fn run(mut tick: Tick, on_quit: OnQuit) {
    while tick() {
        std::thread::sleep(INTERVAL);
    }
    on_quit();
}

#[cfg(target_os = "windows")]
pub(super) fn run(mut tick: Tick, on_quit: OnQuit) {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        DispatchMessageW, GetMessageW, KillTimer, PostQuitMessage, SetTimer, TranslateMessage, MSG, WM_TIMER,
    };
    // SAFETY: plain Win32 message loop on the thread that owns the tray
    // icon's window; MSG is a POD written by GetMessageW.
    unsafe {
        let timer = SetTimer(std::ptr::null_mut(), 0, INTERVAL.as_millis() as u32, None);
        let mut msg: MSG = std::mem::zeroed();
        let mut running = tick();
        if !running {
            PostQuitMessage(0);
        }
        while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
            if msg.message == WM_TIMER && msg.hwnd.is_null() {
                if running && !tick() {
                    running = false;
                    PostQuitMessage(0);
                }
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        KillTimer(std::ptr::null_mut(), timer);
    }
    on_quit();
}

#[cfg(target_os = "macos")]
pub(super) fn run(tick: Tick, on_quit: OnQuit) {
    use std::cell::RefCell;

    use objc2::MainThreadMarker;
    use objc2_app_kit::{NSApplication, NSApplicationActivationPolicy};

    thread_local! {
        static STATE: RefCell<Option<(Tick, OnQuit)>> = const { RefCell::new(None) };
    }

    fn step() {
        let quit = STATE.with(|s| {
            let mut s = s.borrow_mut();
            let keep_going = match s.as_mut() {
                Some((tick, _)) => tick(),
                None => return None,
            };
            if keep_going {
                None
            } else {
                s.take().map(|(_, q)| q)
            }
        });
        if let Some(q) = quit {
            q();
            std::process::exit(0);
        }
    }

    let mtm = MainThreadMarker::new().expect("tray runs on the main thread");
    STATE.with(|s| *s.borrow_mut() = Some((tick, on_quit)));
    let app = NSApplication::sharedApplication(mtm);
    app.setActivationPolicy(NSApplicationActivationPolicy::Accessory);
    std::thread::spawn(|| loop {
        std::thread::sleep(INTERVAL);
        dispatch2::DispatchQueue::main().exec_async(step);
    });
    app.run();
}
