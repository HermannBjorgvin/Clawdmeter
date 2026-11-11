//! The window's root: a sidebar of pages, or the setup wizard filling the
//! window. Child views ask for another page with [`Navigate`]; the tray and
//! second launches do it through `window-commands` ([`crate::ipc`]).

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use clawdmeter_core::{Paths, Settings};
use clawdmeter_flash::release::{self, AppPackage};
use gpui::{div, prelude::*, px, relative, Context, Entity, SharedString, Window};

use super::flasher::FlasherView;
use super::overview::OverviewView;
use super::settings::SettingsView;
use super::setup::{SetupEvent, SetupView};
use super::theme::{Theme, R_ITEM, R_PILL};
use super::widgets::icon;
use super::{chrome, run_blocking, Navigate, REFRESH};
use crate::{ipc, Page};

/// Bytes of an app download so far, shared with the thread fetching it.
#[derive(Debug, Default)]
struct Progress {
    done: AtomicU64,
    /// 0 while unknown.
    total: AtomicU64,
}

/// Where the download of a newer app stands.
#[derive(Debug, Clone)]
enum AppUpdate {
    /// Not asked for.
    Idle,
    /// On its way.
    Downloading(Arc<Progress>),
    /// Downloaded and verified.
    Ready(PathBuf),
    /// The download or its checksum failed.
    Failed,
}

/// The app window.
pub struct AppView {
    paths: Paths,
    /// The package that updates this install; `None` where the release has
    /// none, and the footer then links to the release page.
    package: Option<AppPackage>,
    /// The download of a newer app, and the release tag it is for.
    update: AppUpdate,
    update_tag: Option<String>,
    /// Only release builds are offered one: a local build has no release
    /// version to compare (the firmware check skips `dev` boards likewise).
    offer_update: bool,
    /// `release_repo`, as read when the window opened or a download began.
    repo: String,
    page: Page,
    /// Dark appearance as last drawn.
    dark: Option<bool>,
    /// The setup wizard fills the window.
    setup: bool,
    overview: Entity<OverviewView>,
    settings: Entity<SettingsView>,
    flasher: Entity<FlasherView>,
    wizard: Entity<SetupView>,
}

impl AppView {
    /// Build every page and open on `page` (`Page::Setup`: the wizard).
    pub fn new(
        paths: Paths,
        page: Page,
        manifest: Option<PathBuf>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let overview = cx.new(|cx| OverviewView::new(paths.clone(), cx));
        let settings = cx.new(|cx| SettingsView::new(paths.clone(), Page::Device, cx));
        let flasher = cx.new(|cx| FlasherView::new(paths.clone(), manifest, cx));
        Self::build(paths, page, overview, settings, flasher, window, cx)
    }

    /// [`AppView::new`] from ready-made pages (tests and the demo swap the
    /// hardware out).
    pub fn build(
        paths: Paths,
        page: Page,
        overview: Entity<OverviewView>,
        settings: Entity<SettingsView>,
        flasher: Entity<FlasherView>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let wizard = cx.new(|cx| SetupView::new(paths.clone(), flasher.clone(), settings.clone(), cx));
        cx.subscribe(&overview, |v, _, n: &Navigate, cx| v.go(n.0, cx)).detach();
        cx.subscribe(&settings, |v, _, n: &Navigate, cx| v.go(n.0, cx)).detach();
        cx.subscribe(&flasher, |v, _, n: &Navigate, cx| v.go(n.0, cx)).detach();
        cx.subscribe(&wizard, |v, _, e: &SetupEvent, cx| match e {
            SetupEvent::Finished => {
                v.setup = false;
                v.go(Page::Overview, cx);
            }
        })
        .detach();
        // `show <page>` from the tray or a second launch.
        let p = paths.clone();
        cx.spawn_in(window, async move |this, cx| loop {
            cx.background_executor().timer(REFRESH).await;
            let pages = ipc::take_window_commands(&p);
            let r = this.update_in(cx, |v, window, cx| {
                if let Some(page) = pages.last() {
                    v.go(*page, cx);
                    window.activate_window();
                }
                // Light/dark can change after the first frame (on X11 it
                // arrives from the settings portal). Polled rather than
                // observed: GPUI 0.2.2's X11 client redraws inside the portal
                // callback when someone observes it, and panics.
                let dark = Theme::for_appearance(window.appearance()).dark;
                if v.dark != Some(dark) || matches!(v.update, AppUpdate::Downloading(_)) {
                    v.dark = Some(dark);
                    cx.notify();
                }
            });
            if r.is_err() {
                break;
            }
        })
        .detach();
        let mut v = AppView {
            paths,
            package: AppPackage::for_this_install(),
            update: AppUpdate::Idle,
            update_tag: None,
            offer_update: crate::is_release_build(),
            repo: String::new(),
            dark: None,
            page: Page::Overview,
            setup: false,
            overview,
            settings,
            flasher,
            wizard,
        };
        v.repo = v.release_repo();
        v.go(page, cx);
        v
    }

    /// The setup wizard.
    pub fn wizard(&self) -> &Entity<SetupView> {
        &self.wizard
    }

    /// The page on screen (`Page::Setup` while the wizard is up).
    pub fn page(&self) -> Page {
        if self.setup {
            Page::Setup
        } else {
            self.page
        }
    }

    /// Show `page`. `Page::Setup` starts the wizard over. While a board is
    /// being written the window stays put: other pages probe USB.
    pub fn go(&mut self, page: Page, cx: &mut Context<Self>) {
        if self.flasher.read(cx).flashing() && page != self.page() {
            return;
        }
        if page == Page::Setup {
            self.setup = true;
            self.wizard.update(cx, |w, cx| w.restart(cx));
        } else {
            if self.setup {
                // Leaving the wizard for a page counts as skipping it.
                self.wizard.update(cx, |w, cx| w.leave(cx));
            }
            self.setup = false;
            self.page = page;
            self.settings.update(cx, |s, cx| s.show(page, false, cx));
            self.flasher.update(cx, |f, cx| f.set_active(page == Page::Flash, cx));
        }
        cx.notify();
    }

    /// The repo releases come from (`release_repo`).
    fn release_repo(&self) -> String {
        Settings::load(&self.paths)
            .map(|s| s.release_repo)
            .unwrap_or_else(|_| clawdmeter_flash::DEFAULT_RELEASE_REPO.into())
    }

    /// Whether the footer offers a newer release (tests and the demo).
    pub fn offer_app_update(&mut self, on: bool) {
        self.offer_update = on;
    }

    /// Pretend this install updates from `package` (tests and the demo).
    pub fn set_update_package(&mut self, package: Option<AppPackage>) {
        self.package = package;
    }

    /// Put the download of release `tag` in a given state without fetching
    /// anything (the demo): `downloading`, `ready`, `failed`, else idle.
    pub fn show_update_state(&mut self, tag: &str, state: &str) {
        self.update_tag = Some(tag.to_owned());
        self.update = match state {
            "downloading" => {
                AppUpdate::Downloading(Arc::new(Progress { done: AtomicU64::new(42), total: AtomicU64::new(100) }))
            }
            "ready" => AppUpdate::Ready(PathBuf::new()),
            "failed" => AppUpdate::Failed,
            _ => AppUpdate::Idle,
        };
    }

    /// Download the app package of release `tag` to the Downloads folder,
    /// then hand it to the OS: an installer opens, any other package is
    /// shown in the file manager.
    fn download_update(&mut self, tag: String, cx: &mut Context<Self>) {
        let Some(package) = self.package else { return };
        if matches!(self.update, AppUpdate::Downloading(_)) {
            return;
        }
        let progress = Arc::new(Progress::default());
        self.update = AppUpdate::Downloading(progress.clone());
        self.update_tag = Some(tag.clone());
        self.repo = self.release_repo();
        let repo = self.repo.clone();
        // The Downloads folder, or the cache when there is none or it can't
        // be written (macOS asks before an app may use it).
        let downloads = self.paths.home.join("Downloads");
        let cache = self.paths.cache_dir.clone();
        run_blocking(
            cx,
            move || {
                let fetch = |dir: &std::path::Path| {
                    release::fetch_app_package(&repo, &tag, package, dir, |done, total| {
                        progress.done.store(done, Ordering::Relaxed);
                        progress.total.store(total, Ordering::Relaxed);
                    })
                };
                if !downloads.is_dir() {
                    return fetch(&cache);
                }
                match fetch(&downloads) {
                    Err(release::ReleaseError::Io(e)) => {
                        log::info!("app update: {}: {e}; using the cache", downloads.display());
                        fetch(&cache)
                    }
                    r => r,
                }
            },
            move |v, r, cx| match r {
                Ok(path) => {
                    log::info!("app update downloaded to {}", path.display());
                    open_package(package, &path, cx);
                    v.update = AppUpdate::Ready(path);
                }
                Err(e) => {
                    log::warn!("app update download: {e} ({})", e.detail().unwrap_or_default());
                    v.update = AppUpdate::Failed;
                }
            },
        );
        cx.notify();
    }

    /// The footer's update control, shown while a newer release is out: a
    /// button that downloads it (and fills as it arrives), and a link to the
    /// release notes.
    fn update_control(&self, t: &Theme, tag: String, cx: &mut Context<Self>) -> impl IntoElement {
        let version: SharedString = tag.trim_start_matches('v').to_owned().into();
        let page_url = release::release_page_url(&self.repo, &tag);
        // A download belongs to one release and a newer one starts over, but
        // only once the one on its way has landed.
        let same = self.update_tag.as_deref() == Some(tag.as_str());
        let state = if same || matches!(self.update, AppUpdate::Downloading(_)) {
            self.update.clone()
        } else {
            AppUpdate::Idle
        };
        let package = self.package;
        let (glyph, label, fill): (&str, SharedString, f32) = match (&state, package) {
            (_, None) => ("arrow-down-to-line", format!("Get {version}").into(), 0.),
            (AppUpdate::Idle, _) => ("arrow-down-to-line", format!("Download {version}").into(), 0.),
            (AppUpdate::Downloading(p), _) => {
                let (done, total) = (p.done.load(Ordering::Relaxed), p.total.load(Ordering::Relaxed));
                if total == 0 {
                    ("arrow-down-to-line", "Downloading".into(), 0.)
                } else {
                    let frac = (done as f32 / total as f32).clamp(0., 1.);
                    ("arrow-down-to-line", format!("Downloading {:.0}%", frac * 100.).into(), frac)
                }
            }
            (AppUpdate::Ready(_), Some(p)) if p.opens_installer() => ("check", "Open installer".into(), 0.),
            (AppUpdate::Ready(_), _) => ("check", "Show download".into(), 0.),
            (AppUpdate::Failed, _) => ("rotate-ccw", "Try again".into(), 0.),
        };
        let busy = matches!(state, AppUpdate::Downloading(_));
        let failed = matches!(state, AppUpdate::Failed) && package.is_some();
        let text = t.text;
        let url = page_url.clone();
        let button = div()
            .id("app-update")
            .group("app-update")
            .debug_selector(|| "app-update".into())
            .relative()
            .overflow_hidden()
            .flex()
            .items_center()
            .gap(px(8.))
            .h(px(28.))
            .px(px(9.))
            .rounded(px(R_ITEM))
            .border_1()
            .border_color(t.line)
            .bg(t.surface)
            .text_size(px(12.5))
            .text_color(t.text)
            .whitespace_nowrap()
            .child(div().absolute().top_0().left_0().h_full().w(relative(fill)).rounded(px(R_ITEM - 1.)).bg(t.selected))
            .child(icon(glyph, 14., t.dim).relative().group_hover("app-update", move |s| s.text_color(text)))
            .child(div().relative().child(label))
            .when(!busy, |d| {
                d.cursor_pointer().hover(|s| s.bg(t.hover)).on_click(cx.listener(move |v, _, _, cx| {
                    match (&state, v.package) {
                        (_, None) => cx.open_url(&url),
                        (AppUpdate::Ready(path), Some(p)) if path.exists() => open_package(p, path, cx),
                        _ => v.download_update(tag.clone(), cx),
                    }
                }))
            });
        let notes = div()
            .id("app-update-notes")
            .debug_selector(|| "app-update-notes".into())
            .text_color(t.dim)
            .cursor_pointer()
            .hover(|s| s.text_color(t.text).underline())
            .on_click(move |_, _, cx| cx.open_url(&page_url))
            .child(if failed { "Open the release page" } else { "What's new" });
        div()
            .flex()
            .flex_col()
            .gap(px(6.))
            .pt(px(4.))
            .child(button)
            .when(failed, |d| d.child(div().text_color(t.bad).child("Couldn't download it.")))
            // With no package the button already goes to the release page.
            .when(package.is_some(), |d| d.child(div().flex().child(notes)))
    }

    fn nav_item(
        &self,
        t: &Theme,
        page: Page,
        badge: Option<SharedString>,
        locked: bool,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let on = page == self.page;
        let enabled = on || !locked;
        let text = t.text;
        div()
            .id(page.as_str())
            .group(page.as_str())
            .debug_selector(move || format!("nav-{}", page.as_str()))
            .flex()
            .items_center()
            .gap(px(10.))
            .h(px(32.))
            .px(px(10.))
            .rounded(px(R_ITEM))
            .text_color(match (on, enabled) {
                (true, _) => t.text,
                (false, true) => t.dim,
                (false, false) => t.mute,
            })
            .when(on, |d| d.bg(t.selected))
            .when(enabled && !on, |d| {
                d.cursor_pointer()
                    .hover(|s| s.bg(t.hover).text_color(t.text))
                    .on_click(cx.listener(move |v, _, _, cx| v.go(page, cx)))
            })
            .child(
                icon(
                    page_icon(page),
                    16.,
                    match (on, enabled) {
                        (true, _) => t.text,
                        (false, true) => t.dim,
                        (false, false) => t.mute,
                    },
                )
                .group_hover(page.as_str(), move |s| s.text_color(text)),
            )
            .child(div().flex_1().child(page.label()))
            .when_some(badge, |d, b| {
                d.child(
                    div()
                        .min_w(px(18.))
                        .px(px(5.))
                        .rounded(px(R_PILL))
                        .bg(t.selected)
                        .text_color(t.text)
                        .text_size(px(11.))
                        .flex()
                        .justify_center()
                        .child(b),
                )
            })
    }

    fn sidebar(&self, t: &Theme, cx: &mut Context<Self>) -> impl IntoElement {
        let pending_hooks = self.settings.read(cx).pending_hooks();
        let update = self.overview.read(cx).update_available();
        let locked = self.flasher.read(cx).flashing();
        let app_update = self.overview.read(cx).app_update().filter(|_| self.offer_update);
        let badge = |page: Page| -> Option<SharedString> {
            match page {
                Page::Hooks if pending_hooks > 0 => Some(pending_hooks.to_string().into()),
                Page::Flash if update => Some("1".into()),
                _ => None,
            }
        };
        let group = || div().flex().flex_col().gap(px(1.));
        let mut main = group();
        for page in [Page::Overview, Page::Device, Page::Flash] {
            main = main.child(self.nav_item(t, page, badge(page), locked, cx));
        }
        let mut claude = group().child(
            div().px(px(10.)).pt(px(18.)).pb(px(6.)).text_size(px(12.5)).text_color(t.mute).child("Claude Code"),
        );
        for page in [Page::Accounts, Page::Hooks] {
            claude = claude.child(self.nav_item(t, page, badge(page), locked, cx));
        }
        let footer = div()
            .flex()
            .flex_col()
            .gap(px(4.))
            .px(px(10.))
            .pt(px(12.))
            .text_size(px(12.))
            .text_color(t.mute)
            .child(
                div().flex().gap(px(4.)).whitespace_nowrap().child("Created by").child(
                    div()
                        .id("credit")
                        .debug_selector(|| "credit".into())
                        .text_color(t.dim)
                        .cursor_pointer()
                        .hover(|s| s.text_color(t.text).underline())
                        .on_click(|_, _, cx| cx.open_url(CREDIT_URL))
                        .child("@HermannBjorgvin"),
                ),
            )
            .child(format!("Version {}", crate::version()))
            .when_some(app_update, |d, tag| d.child(self.update_control(t, tag, cx)));
        div()
            .flex()
            .flex_col()
            .w(px(216.))
            .flex_shrink_0()
            .px(px(8.))
            .pt(px(12.))
            .pb(px(14.))
            .bg(t.bg)
            .border_r_1()
            .border_color(t.line)
            .child(main)
            .child(claude)
            .child(div().flex_1())
            .child(self.nav_item(t, Page::Settings, None, locked, cx))
            .child(footer)
    }
}

/// Hand a downloaded package to the OS: mount the disk image or start the
/// installer; show a `.deb` or AppImage in the file manager.
fn open_package(package: AppPackage, path: &std::path::Path, cx: &mut gpui::App) {
    if package.opens_installer() {
        cx.open_with_system(path);
    } else {
        cx.reveal_path(path);
    }
}

/// The author's profile, linked from the sidebar footer.
const CREDIT_URL: &str = "https://x.com/HermannBjorgvin";

/// The sidebar icon for a page.
fn page_icon(page: Page) -> &'static str {
    match page {
        Page::Overview => "gauge",
        Page::Device => "tablet",
        Page::Flash => "zap",
        Page::Accounts => "users",
        Page::Hooks => "webhook",
        Page::Settings => "settings",
        Page::Setup => "rotate-ccw",
    }
}

impl Render for AppView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let t = Theme::for_appearance(window.appearance());
        let body = if self.setup {
            div().size_full().child(self.wizard.clone())
        } else {
            let content = match self.page {
                Page::Overview => self.overview.clone().into_any_element(),
                Page::Flash => self.flasher.clone().into_any_element(),
                _ => self.settings.clone().into_any_element(),
            };
            let header = match window.window_decorations() {
                gpui::Decorations::Client { .. } => px(super::HEADER_H),
                gpui::Decorations::Server => px(0.),
            };
            let page_h = window.viewport_size().height - header;
            div().size_full().flex().child(self.sidebar(&t, cx)).child(
                div()
                    .id("page")
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .overflow_y_scroll()
                    // At least the visible height, so a page can pin
                    // something to its foot; taller pages scroll.
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .min_h(page_h)
                            .max_w(px(720.))
                            .px(px(32.))
                            .pt(px(28.))
                            .pb(px(24.))
                            .child(content),
                    ),
            )
        };
        chrome(&t, window, body)
    }
}

#[cfg(test)]
mod tests {

    use gpui::{Modifiers, TestAppContext, VisualTestContext};

    use super::*;
    use crate::ui::flasher::tests::FakeBoard;

    fn click(cx: &mut VisualTestContext, selector: &'static str) {
        cx.run_until_parked();
        let b = cx.debug_bounds(selector).unwrap_or_else(|| panic!("{selector} isn't on screen"));
        cx.simulate_click(b.center(), Modifiers::none());
        cx.run_until_parked();
    }

    #[gpui::test]
    fn sidebar_and_window_commands_switch_pages(cx: &mut TestAppContext) {
        let root = std::env::temp_dir().join(format!("cm-app-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let paths = Paths::under(&root);
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |window, cx| {
            let overview = cx.new(|cx| OverviewView::new(p.clone(), cx));
            let settings = cx.new(|cx| SettingsView::with_usb(p.clone(), Page::Device, false, cx));
            let flasher = cx.new(|cx| FlasherView::with_hardware(p.clone(), None, Arc::new(FakeBoard::default()), cx));
            AppView::build(p, Page::Overview, overview, settings, flasher, window, cx)
        });
        click(cx, "nav-hooks");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Hooks));

        ipc::send_window_command(&paths, Page::Settings).unwrap();
        cx.executor().advance_clock(REFRESH * 2);
        cx.run_until_parked();
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Settings));

        click(cx, "nav-overview");
        click(cx, "run-setup");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Setup));
        click(cx, "skip-setup");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Overview));
        assert!(!crate::onboarding::needed(&paths), "skipping counts as done");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[gpui::test]
    fn footer_offers_a_newer_release(cx: &mut TestAppContext) {
        let root = std::env::temp_dir().join(format!("cm-app-update-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let paths = Paths::under(&root);
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        let status = |latest: &str| {
            let snap = ipc::StatusSnapshot {
                updated_at: clawdmeter_core::unix_now(),
                latest_firmware: Some(latest.into()),
                ..Default::default()
            };
            ipc::write_status(&paths, &snap).unwrap();
        };
        status(crate::version());
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |window, cx| {
            let overview = cx.new(|cx| OverviewView::new(p.clone(), cx));
            let settings = cx.new(|cx| SettingsView::with_usb(p.clone(), Page::Device, false, cx));
            let flasher = cx.new(|cx| FlasherView::with_hardware(p.clone(), None, Arc::new(FakeBoard::default()), cx));
            AppView::build(p, Page::Overview, overview, settings, flasher, window, cx)
        });
        // An install the release has no package for gets one link, to the page.
        view.update(cx, |v, _| {
            v.offer_app_update(true);
            v.set_update_package(None);
        });
        cx.run_until_parked();
        assert!(cx.debug_bounds("app-update").is_none(), "this version is the latest");

        status("v999.0.0");
        cx.executor().advance_clock(REFRESH * 2);
        cx.run_until_parked();
        assert!(cx.debug_bounds("app-update").is_some());
        assert!(cx.debug_bounds("app-update-notes").is_none());

        // One it can download also links to the release notes.
        view.update(cx, |v, cx| {
            v.set_update_package(Some(AppPackage::Dmg));
            cx.notify();
        });
        cx.run_until_parked();
        assert!(cx.debug_bounds("app-update").is_some());
        assert!(cx.debug_bounds("app-update-notes").is_some());
        let _ = std::fs::remove_dir_all(&root);
    }
}
