//! The window's root: a sidebar of pages, or the setup wizard filling the
//! window. Child views ask for another page with [`Navigate`]; the tray and
//! second launches do it through `window-commands` ([`crate::ipc`]).

use std::path::PathBuf;
use std::sync::Arc;

use clawdmeter_core::Paths;
use gpui::{div, prelude::*, px, Context, Entity, SharedString, Window};

use super::decorations::{self, ButtonLayout};
use super::flasher::FlasherView;
use super::overview::OverviewView;
use super::settings::SettingsView;
use super::setup::{SetupEvent, SetupView};
use super::theme::{Theme, R_ITEM, R_PILL};
use super::update::UpdateView;
use super::widgets::{icon, update_pill};
use super::{chrome, Navigate, REFRESH};
use crate::{ipc, Page};

/// The app window.
pub struct AppView {
    page: Page,
    /// Dark appearance as last drawn.
    dark: Option<bool>,
    /// Header bar buttons as last drawn.
    layout: Arc<ButtonLayout>,
    /// The setup wizard fills the window.
    setup: bool,
    overview: Entity<OverviewView>,
    settings: Entity<SettingsView>,
    flasher: Entity<FlasherView>,
    update: Entity<UpdateView>,
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
        let update = cx.new(|cx| UpdateView::new(paths.clone(), cx));
        cx.subscribe(&update, |v, _, n: &Navigate, cx| v.go(n.0, cx)).detach();
        settings.update(cx, |s, cx| s.follow_update(update.clone(), cx));
        cx.subscribe(&wizard, |v, _, e: &SetupEvent, cx| match e {
            SetupEvent::Finished => {
                v.setup = false;
                v.go(Page::Overview, cx);
            }
        })
        .detach();
        // `show <page>` from the tray or a second launch.
        let p = paths;
        cx.spawn_in(window, async move |this, cx| loop {
            cx.background_executor().timer(REFRESH).await;
            let pages = ipc::take_window_commands(&p);
            let r = this.update_in(cx, |v, window, cx| {
                if let Some(page) = pages.last() {
                    v.go(*page, cx);
                    if !window.is_window_active() {
                        log::debug!(
                            "asked to show {}: raising the window (a Wayland compositor may refuse, \
                             and then a minimized window stays minimized)",
                            page.as_str()
                        );
                    }
                    window.activate_window();
                }
                // The desktop's button layout can change while we're open
                // (GNOME Tweaks).
                let layout = decorations::button_layout();
                if v.layout != layout {
                    log::debug!("header bar buttons now {layout:?}");
                    v.layout = layout;
                    cx.notify();
                }
                // Light/dark can change after the first frame (on X11 it
                // arrives from the settings portal). Polled rather than
                // observed: GPUI 0.2.2's X11 client redraws inside the portal
                // callback when someone observes it, and panics.
                let dark = Theme::for_appearance(window.appearance()).dark;
                if v.dark != Some(dark) {
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
            dark: None,
            layout: decorations::button_layout(),
            page: Page::Overview,
            setup: false,
            overview,
            settings,
            flasher,
            update,
            wizard,
        };
        v.go(page, cx);
        v
    }

    /// The Update page.
    pub fn update_view(&self) -> &Entity<UpdateView> {
        &self.update
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
            self.update.update(cx, |u, cx| u.set_active(page == Page::Update, cx));
        }
        cx.notify();
    }

    /// The footer's notice of a newer release, until it is skipped: a row
    /// that opens the Update page.
    fn update_notice(&self, t: &Theme, tag: &str, locked: bool, cx: &mut Context<Self>) -> impl IntoElement {
        let on = self.page == Page::Update;
        let enabled = on || !locked;
        update_pill(t, "app-update", tag, on, enabled)
            .mt(px(4.))
            .when(enabled && !on, |d| d.on_click(cx.listener(|v, _, _, cx| v.go(Page::Update, cx))))
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
        let app_update = self.update.read(cx).announced().map(str::to_owned);
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
            .when_some(app_update, |d, tag| d.child(self.update_notice(t, &tag, locked, cx)));
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
        Page::Update => "arrow-down-to-line",
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
                Page::Update => self.update.clone().into_any_element(),
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
        chrome(&t, window, cx, body)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

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
    fn a_newer_release_is_announced_until_skipped(cx: &mut TestAppContext) {
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
        let update = view.read_with(cx, |v, _| v.update_view().clone());
        // Notes are given, so opening the page asks GitHub nothing.
        let notes = |tag: &str| {
            vec![clawdmeter_flash::release::ReleaseNotes {
                tag: tag.into(),
                published: "2026-11-10".into(),
                body: "## New\n- A thing".into(),
            }]
        };
        update.update(cx, |u, _| {
            u.offer(true);
            u.set_package(None);
            u.show_notes("v999.0.0", notes("v999.0.0"));
        });
        cx.run_until_parked();
        assert!(cx.debug_bounds("app-update").is_none(), "this version is the latest");

        // Off screen, the page looks every few seconds.
        status("v999.0.0");
        cx.executor().advance_clock(REFRESH * 12);
        cx.run_until_parked();
        click(cx, "app-update");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Update));
        assert!(cx.debug_bounds("update-download").is_some());

        click(cx, "update-skip");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Overview));
        // (gpui 0.2.2 never forgets a `debug_bounds` entry, so the footer row
        // going away can only be checked through what it is drawn from.)
        update.read_with(cx, |u, _| {
            assert_eq!(u.available(), Some("v999.0.0"), "still there for Settings to open");
            assert_eq!(u.announced(), None);
        });
        let saved = clawdmeter_core::Settings::load(&paths).unwrap().skipped_update;
        assert_eq!(saved.as_deref(), Some("v999.0.0"));

        // The release after the skipped one is announced again.
        status("v999.1.0");
        update.update(cx, |u, _| u.show_notes("v999.1.0", notes("v999.1.0")));
        cx.executor().advance_clock(REFRESH * 12);
        cx.run_until_parked();
        update.read_with(cx, |u, _| assert_eq!(u.announced(), Some("v999.1.0")));

        // With "Check for updates" off, a release found before isn't offered.
        crate::ui::settings::write_settings(&paths, |s, _| s.update_check = false).unwrap();
        cx.executor().advance_clock(REFRESH * 12);
        cx.run_until_parked();
        update.read_with(cx, |u, _| assert_eq!((u.available(), u.announced()), (None, None)));

        // A skipped release is still there beside the Settings heading.
        crate::ui::settings::write_settings(&paths, |s, _| {
            s.update_check = true;
            s.skipped_update = Some("v999.1.0".into());
        })
        .unwrap();
        cx.executor().advance_clock(REFRESH * 12);
        cx.run_until_parked();
        update.read_with(cx, |u, _| assert_eq!((u.available(), u.announced()), (Some("v999.1.0"), None)));
        view.update(cx, |v, cx| v.go(Page::Settings, cx));
        click(cx, "settings-app-update");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Update));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[gpui::test]
    fn firmware_nudge_can_be_skipped(cx: &mut TestAppContext) {
        let root = std::env::temp_dir().join(format!("cm-app-fw-skip-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let paths = Paths::under(&root);
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        let status = |latest: &str| {
            let snap = ipc::StatusSnapshot {
                updated_at: clawdmeter_core::unix_now(),
                latest_firmware: Some(latest.into()),
                device_firmware: Some("v1.0.0".into()),
                ..Default::default()
            };
            ipc::write_status(&paths, &snap).unwrap();
        };
        status("v999.0.0");
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |window, cx| {
            let overview = cx.new(|cx| OverviewView::new(p.clone(), cx));
            let settings = cx.new(|cx| SettingsView::with_usb(p.clone(), Page::Device, false, cx));
            let flasher = cx.new(|cx| FlasherView::with_hardware(p.clone(), None, Arc::new(FakeBoard::default()), cx));
            AppView::build(p, Page::Overview, overview, settings, flasher, window, cx)
        });
        let overview = view.read_with(cx, |v, _| v.overview.clone());
        overview.read_with(cx, |o, _| assert!(o.update_available()));

        click(cx, "skip-firmware");
        overview.read_with(cx, |o, _| assert!(!o.update_available(), "no nudge and no Flash badge"));
        let saved = clawdmeter_core::Settings::load(&paths).unwrap().skipped_firmware;
        assert_eq!(saved.as_deref(), Some("v999.0.0"));

        // The same release written without its `v` is still skipped.
        status("999.0.0");
        cx.executor().advance_clock(REFRESH * 2);
        cx.run_until_parked();
        overview.read_with(cx, |o, _| assert!(!o.update_available()));

        // A later release nudges again.
        status("v999.1.0");
        cx.executor().advance_clock(REFRESH * 2);
        cx.run_until_parked();
        overview.read_with(cx, |o, _| assert!(o.update_available()));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[gpui::test]
    fn linux_picks_a_package_from_a_menu(cx: &mut TestAppContext) {
        use clawdmeter_flash::release::{AppPackage, ReleaseNotes};
        let root = std::env::temp_dir().join(format!("cm-app-linux-menu-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let paths = Paths::under(&root);
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        let snap = ipc::StatusSnapshot {
            updated_at: clawdmeter_core::unix_now(),
            latest_firmware: Some("v999.0.0".into()),
            ..Default::default()
        };
        ipc::write_status(&paths, &snap).unwrap();
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |window, cx| {
            let overview = cx.new(|cx| OverviewView::new(p.clone(), cx));
            let settings = cx.new(|cx| SettingsView::with_usb(p.clone(), Page::Device, false, cx));
            let flasher = cx.new(|cx| FlasherView::with_hardware(p.clone(), None, Arc::new(FakeBoard::default()), cx));
            AppView::build(p, Page::Overview, overview, settings, flasher, window, cx)
        });
        let update = view.read_with(cx, |v, _| v.update_view().clone());
        update.update(cx, |u, _| {
            u.offer(true);
            u.set_choices(AppPackage::for_linux("x86_64"), Some(AppPackage::Deb), false);
            let notes = ReleaseNotes { tag: "v999.0.0".into(), published: String::new(), body: String::new() };
            u.show_notes("v999.0.0", vec![notes]);
        });
        cx.executor().advance_clock(REFRESH * 2);
        click(cx, "app-update");
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Update));

        click(cx, "update-download");
        update.read_with(cx, |u, _| assert!(u.menu_open()));
        assert!(cx.debug_bounds("update-menu-deb").is_some());
        assert!(cx.debug_bounds("update-menu-appimage").is_some());
        // A click anywhere else closes it, and goes no further: not to the
        // sidebar item under it.
        click(cx, "nav-overview");
        update.read_with(cx, |u, _| assert!(!u.menu_open()));
        view.read_with(cx, |v, _| assert_eq!(v.page(), Page::Update));

        click(cx, "update-download");
        update.read_with(cx, |u, _| assert!(u.menu_open()));
        cx.simulate_keystrokes("escape");
        update.read_with(cx, |u, _| assert!(!u.menu_open(), "Escape closes it"));

        // After a failed download the menu is back, to try another package.
        update.update(cx, |u, _| u.show_download_state("v999.0.0", "failed"));
        click(cx, "update-download");
        update.read_with(cx, |u, _| assert!(u.menu_open()));
        let _ = std::fs::remove_dir_all(&root);
    }
}
