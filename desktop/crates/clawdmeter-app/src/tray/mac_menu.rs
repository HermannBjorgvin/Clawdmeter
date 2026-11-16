//! The macOS tray menu's panel: the device's home screen drawn in AppKit
//! (Clawd, the link, a meter per usage window, the reset countdowns, and a
//! weekly-only Codex plan's reset credits as the device's Resets card), with
//! the Overview's reload button in its corner (the menu stays open while it
//! checks, so the new numbers come in where you're looking) and a footer:
//! Open Clawdmeter on the left; on the right, an update icon (while the app
//! or the board is behind the latest release, `tray::update_offer`), the chime on this computer (the Overview's switch),
//! Settings, and Quit as icons, Quit at the far end and red only under the
//! pointer. The panel is the whole macOS menu; Linux and
//! Windows keep the native list.
//!
//! muda has no API for an item with a view, so the panel goes into muda's
//! NSMenu (`ContextMenu::ns_menu`) directly, at index 0. Clicks set flags
//! the tray's tick takes ([`take_actions`]).
//!
//! Text uses the system label colours, so it follows light and dark mode and
//! the menu's vibrancy by itself; the bar fills are the Overview's (the
//! device's) colours, picked by the view's appearance on each update.

use std::cell::Cell;
use std::sync::atomic::{AtomicU8, Ordering};

use objc2::rc::Retained;
use objc2::runtime::AnyObject;
use objc2::{define_class, msg_send, sel, AnyThread, DefinedClass, MainThreadMarker, MainThreadOnly};
use objc2_app_kit::{
    NSAppearance, NSAppearanceCustomization, NSAppearanceNameAqua, NSAppearanceNameDarkAqua, NSBox, NSBoxType,
    NSButton, NSCellImagePosition, NSColor, NSControlSize, NSEvent, NSFont, NSFontWeightMedium, NSFontWeightRegular,
    NSFontWeightSemibold, NSImage, NSImageView, NSLineBreakMode, NSMenu, NSMenuItem, NSProgressIndicator,
    NSProgressIndicatorStyle, NSTextAlignment, NSTextField, NSTitlePosition, NSTrackingArea, NSTrackingAreaOptions,
    NSView,
};
use objc2_foundation::{NSArray, NSData, NSPoint, NSRect, NSSize, NSString};

use super::header::{Header, Resets, RESET_CHIPS};
use crate::art::{cell_rgb, idle_cell, IDLE_H, IDLE_W};
use crate::ui::overview::RefreshState;
use crate::ui::theme::Theme;
use crate::ui::widgets::Tone;

/// The header's width; the menu grows to fit it.
const W: f64 = 300.0;
/// Left and right inset.
const PAD: f64 = 14.0;
/// Clawd's size in points per sprite cell (drawn from 2x pixels).
const CELL: f64 = 2.0;
const CLAWD_W: f64 = IDLE_W as f64 * CELL;
const CLAWD_H: f64 = IDLE_H as f64 * CELL;
/// Where the text beside Clawd starts.
const TEXT_X: f64 = PAD + CLAWD_W + 10.0;
/// Where the first meter starts, and one meter's height and gap.
const METERS_Y: f64 = 10.0 + CLAWD_H + 14.0;
const METER_H: f64 = 55.0;
const METER_GAP: f64 = 12.0;
/// The Resets block, in a meter's place: its header row's height, the
/// ticket's size, and the chips' height and gap.
const RESETS_HEAD_H: f64 = 20.0;
const TICKET: f64 = 12.0;
const CHIP_H: f64 = 22.0;
const CHIP_GAP: f64 = 6.0;

/// What a click in the panel asks the tray to do.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// The reload button: check usage now.
    Refresh,
    /// Open Clawdmeter: the window on the Overview.
    Open,
    /// The gear: the window on the Settings page.
    Settings,
    /// The update icon: the window on the Update page (the app) or the Flash
    /// page (the firmware), whichever the tray offers.
    Update,
    /// The speaker: switch the chime on this computer (`desktop_chime`), as
    /// the Overview's switch does.
    Chime,
    /// The power icon: quit.
    Quit,
}

impl Action {
    const ALL: [Action; 6] =
        [Action::Refresh, Action::Open, Action::Settings, Action::Update, Action::Chime, Action::Quit];

    fn bit(self) -> u8 {
        1 << self as u8
    }
}

/// Clicks not yet taken by the tray, one bit per [`Action`].
static ACTIONS: AtomicU8 = AtomicU8::new(0);

/// The panel's clicks since the last call, in [`Action::ALL`] order.
pub fn take_actions() -> Vec<Action> {
    let bits = ACTIONS.swap(0, Ordering::SeqCst);
    Action::ALL.into_iter().filter(|a| bits & a.bit() != 0).collect()
}

/// The reload glyph's size, points.
const ICON: f64 = 16.0;
/// The reload button's square, larger than the glyph so it's easy to hit.
const HIT: f64 = 24.0;
/// How far in from the right edge the glyph sits: further than the content
/// column (`PAD`), clear of the menu's rounded corner.
const ICON_INSET: f64 = 20.0;
/// The glyph's square, centred on the title line.
const GLYPH: NSRect = NSRect::new(NSPoint::new(W - ICON_INSET - ICON, 10.5), NSSize::new(ICON, ICON));
/// The speaker's tooltips: what a click does, in the Overview switch's words.
const CHIME_OFF_TIP: &str = "Stop the chime on this computer";
const CHIME_ON_TIP: &str = "Play a chime on this computer when Claude needs you";
/// The footer: the gap above its line, and its row's height and padding.
const FOOT_GAP: f64 = 10.0;
const FOOT_ROW: f64 = 26.0;
const FOOT_PAD: f64 = 7.0;

define_class!(
    /// A flipped view (y grows down, like the layout below), and its buttons'
    /// target. A click on the panel itself does nothing (nothing on it
    /// highlights like an item, so nothing should act like one). Buttons that
    /// open the window or quit close the menu first; reload and the speaker
    /// keep it open, to show what changed.
    #[unsafe(super(NSView))]
    #[name = "ClawdmeterMenuHeader"]
    struct HeaderView;

    impl HeaderView {
        #[unsafe(method(isFlipped))]
        fn is_flipped(&self) -> bool {
            true
        }

        #[unsafe(method(refresh:))]
        fn refresh(&self, _sender: Option<&AnyObject>) {
            self.ask(Action::Refresh, false);
        }

        #[unsafe(method(open:))]
        fn open(&self, _sender: Option<&AnyObject>) {
            self.ask(Action::Open, true);
        }

        #[unsafe(method(settings:))]
        fn settings(&self, _sender: Option<&AnyObject>) {
            self.ask(Action::Settings, true);
        }

        #[unsafe(method(update:))]
        fn update(&self, _sender: Option<&AnyObject>) {
            self.ask(Action::Update, true);
        }

        #[unsafe(method(chime:))]
        fn chime(&self, _sender: Option<&AnyObject>) {
            self.ask(Action::Chime, false);
        }

        #[unsafe(method(quit:))]
        fn quit(&self, _sender: Option<&AnyObject>) {
            self.ask(Action::Quit, true);
        }
    }
);

#[derive(Default)]
struct HoverIvars {
    /// Red under the pointer (Quit).
    danger: Cell<bool>,
    /// The pointer entered it and hasn't left. AppKit sends no
    /// `mouseExited:` when the menu closes under the pointer (a click that
    /// opens the window), so [`HoverButton::tint`] also checks where the
    /// pointer really is, and moving between windows or hiding clears it.
    inside: Cell<bool>,
}

define_class!(
    /// An icon button that brightens under the pointer (red, for Quit), so
    /// the footer's icons answer the mouse the way menu items do.
    #[unsafe(super(NSButton))]
    #[name = "ClawdmeterMenuButton"]
    #[ivars = HoverIvars]
    struct HoverButton;

    impl HoverButton {
        #[unsafe(method(mouseEntered:))]
        fn mouse_entered(&self, _event: &NSEvent) {
            self.ivars().inside.set(true);
            self.tint();
        }

        #[unsafe(method(mouseExited:))]
        fn mouse_exited(&self, _event: &NSEvent) {
            self.ivars().inside.set(false);
            self.tint();
        }

        #[unsafe(method(viewDidMoveToWindow))]
        fn view_did_move_to_window(&self) {
            // SAFETY: NSView's own implementation, called as an override should.
            let _: () = unsafe { msg_send![super(self), viewDidMoveToWindow] };
            self.ivars().inside.set(false);
            self.tint();
        }

        #[unsafe(method(viewDidHide))]
        fn view_did_hide(&self) {
            // SAFETY: NSView's own implementation, called as an override should.
            let _: () = unsafe { msg_send![super(self), viewDidHide] };
            self.ivars().inside.set(false);
            self.tint();
        }
    }
);

impl HoverButton {
    /// An image-only, borderless button sending `action` to `target`.
    fn new(
        mtm: MainThreadMarker,
        icon: &str,
        label: &str,
        target: &AnyObject,
        action: objc2::runtime::Sel,
    ) -> Retained<Self> {
        let this = Self::alloc(mtm).set_ivars(HoverIvars::default());
        // SAFETY: NSButton's designated initializer on a fresh allocation.
        let b: Retained<Self> = unsafe { msg_send![super(this), initWithFrame: rect(0.0, 0.0, HIT, HIT)] };
        if let Some(image) = icon_image(icon) {
            image.setAccessibilityDescription(Some(&NSString::from_str(label)));
            b.setImage(Some(&image));
        }
        b.setBordered(false);
        b.setImagePosition(NSCellImagePosition::ImageOnly);
        b.setToolTip(Some(&NSString::from_str(label)));
        // SAFETY: the target is the panel view, which holds the button (a
        // control doesn't retain its target), and `action` is its method.
        unsafe {
            b.setTarget(Some(target));
            b.setAction(Some(action));
        }
        let options = NSTrackingAreaOptions::MouseEnteredAndExited
            | NSTrackingAreaOptions::ActiveAlways
            | NSTrackingAreaOptions::InVisibleRect;
        let owner: &AnyObject = &b;
        // SAFETY: the area is owned by the button it's added to, and
        // InVisibleRect makes it follow the button's frame.
        let area = unsafe {
            NSTrackingArea::initWithRect_options_owner_userInfo(
                NSTrackingArea::alloc(),
                NSRect::ZERO,
                options,
                Some(owner),
                None,
            )
        };
        b.addTrackingArea(&area);
        b.tint();
        b
    }

    /// The pointer is over the button right now (it's in a window).
    fn pointer_inside(&self) -> bool {
        let Some(window) = self.window() else { return false };
        let at = self.convertPoint_fromView(window.convertPointFromScreen(NSEvent::mouseLocation()), None);
        let b = self.bounds();
        at.x >= b.origin.x
            && at.y >= b.origin.y
            && at.x <= b.origin.x + b.size.width
            && at.y <= b.origin.y + b.size.height
    }

    /// Dim when disabled, bright under the pointer (red for Quit), quiet
    /// otherwise.
    fn tint(&self) {
        let c = if !self.isEnabled() {
            NSColor::tertiaryLabelColor()
        } else if self.ivars().inside.get() && self.pointer_inside() {
            if self.ivars().danger.get() {
                NSColor::systemRedColor()
            } else {
                NSColor::labelColor()
            }
        } else {
            NSColor::secondaryLabelColor()
        };
        self.setContentTintColor(Some(&c));
    }

    /// Show `image` (made once: rasterising an SVG is too much to redo on
    /// every update) and say what a click does.
    fn show(&self, image: Option<&NSImage>, label: &str) {
        self.setImage(image);
        self.setToolTip(Some(&NSString::from_str(label)));
    }
}

impl HeaderView {
    fn new(mtm: MainThreadMarker, frame: NSRect) -> Retained<Self> {
        // SAFETY: NSView's designated initializer, on a fresh allocation of a
        // subclass with no ivars.
        unsafe { msg_send![Self::alloc(mtm), initWithFrame: frame] }
    }

    /// Queue `action` for the tray, closing the menu first when it opens
    /// the window or quits.
    fn ask(&self, action: Action, close: bool) {
        ACTIONS.fetch_or(action.bit(), Ordering::SeqCst);
        if close {
            // SAFETY: a plain getter; the item outlives this event.
            if let Some(menu) = self.enclosingMenuItem().and_then(|i| unsafe { i.menu() }) {
                menu.cancelTracking();
            }
        }
    }
}

/// One meter's views, laid out like a device panel.
struct MeterViews {
    value: Retained<NSTextField>,
    label: Retained<NSTextField>,
    track: Retained<NSBox>,
    fill: Retained<NSBox>,
    tick: Retained<NSBox>,
    reset: Retained<NSTextField>,
}

/// A weekly-only Codex plan's reset credits, laid out like the device's
/// Resets card: "Resets expire", the count in a ticket pill, then a chip per
/// held credit's expiry date (the soonest filled) or the line in their place.
struct ResetViews {
    title: Retained<NSTextField>,
    pill: Retained<NSBox>,
    ticket: Retained<NSImageView>,
    count: Retained<NSTextField>,
    chips: [(Retained<NSBox>, Retained<NSTextField>); RESET_CHIPS],
    note: Retained<NSTextField>,
}

/// The header item and the views it updates.
pub struct MenuHeader {
    item: Retained<NSMenuItem>,
    view: Retained<HeaderView>,
    title: Retained<NSTextField>,
    detail: Retained<NSTextField>,
    age: Retained<NSTextField>,
    reload: Retained<HoverButton>,
    spinner: Retained<NSProgressIndicator>,
    meters: [MeterViews; 2],
    resets: ResetViews,
    line: Retained<NSBox>,
    open: Retained<NSButton>,
    update: Retained<HoverButton>,
    /// The orange dot on the update icon.
    update_dot: Retained<NSBox>,
    chime: Retained<HoverButton>,
    /// The speaker's two faces: chime on, chime off.
    chime_icons: [Option<Retained<NSImage>>; 2],
    settings: Retained<HoverButton>,
    quit: Retained<HoverButton>,
    /// What the views show now (updates that change nothing are skipped).
    shown: Option<(Header, bool)>,
}

fn rect(x: f64, y: f64, w: f64, h: f64) -> NSRect {
    NSRect::new(NSPoint::new(x, y), NSSize::new(w, h))
}

fn label(mtm: MainThreadMarker, font: &NSFont, color: &NSColor) -> Retained<NSTextField> {
    let l = NSTextField::labelWithString(&NSString::from_str(""), mtm);
    l.setFont(Some(font));
    l.setTextColor(Some(color));
    l.setLineBreakMode(NSLineBreakMode::ByTruncatingTail);
    l
}

/// A borderless rounded rectangle filled with `color`.
fn bar(mtm: MainThreadMarker, radius: f64, color: &NSColor) -> Retained<NSBox> {
    let b = NSBox::initWithFrame(NSBox::alloc(mtm), rect(0.0, 0.0, 1.0, 1.0));
    b.setBoxType(NSBoxType::Custom);
    b.setTitlePosition(NSTitlePosition::NoTitle);
    b.setBorderWidth(0.0);
    b.setCornerRadius(radius);
    b.setFillColor(color);
    b
}

fn srgb(rgb: u32) -> Retained<NSColor> {
    let c = |shift: u32| f64::from((rgb >> shift) & 0xff) / 255.0;
    NSColor::colorWithSRGBRed_green_blue_alpha(c(16), c(8), c(0), 1.0)
}

/// A bar's fill in the Overview's colours (`overview::bar_fill`) for the
/// view's appearance; `None`, a Codex account's neutral bar
/// (`overview::codex_fill`).
fn fill_color(tone: Option<Tone>, dark: bool) -> Retained<NSColor> {
    let theme = if dark { Theme::dark() } else { Theme::light() };
    let c = match tone {
        Some(tone) => crate::ui::overview::bar_fill(&theme, tone),
        None => crate::ui::overview::codex_fill(&theme),
    };
    rgba_color(c)
}

/// The theme's dim colour, opaque: the pace marker on a neutral (Codex)
/// bar, as the device draws it in `TH_DIM`.
fn dim_color(dark: bool) -> Retained<NSColor> {
    let theme = if dark { Theme::dark() } else { Theme::light() };
    rgba_color(theme.dim)
}

fn rgba_color(c: gpui::Rgba) -> Retained<NSColor> {
    NSColor::colorWithSRGBRed_green_blue_alpha(c.r.into(), c.g.into(), c.b.into(), c.a.into())
}

/// An NSImage of `size` points from straight RGBA pixels.
fn image_from_rgba(rgba: &[u8], w: usize, h: usize, size: NSSize) -> Option<Retained<NSImage>> {
    let mut png = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut png, w as u32, h as u32);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut writer = enc.write_header().ok()?;
        writer.write_image_data(rgba).ok()?;
    }
    let image = NSImage::initWithData(NSImage::alloc(), &NSData::with_bytes(&png))?;
    image.setSize(size);
    Some(image)
}

/// One of the app's bundled Lucide icons (`assets/icons/<name>.svg`, the
/// ones the window draws) at 2x, as a template image: AppKit tints it for
/// the appearance and the button's state.
fn icon_image(name: &str) -> Option<Retained<NSImage>> {
    icon_image_at(name, ICON)
}

/// [`icon_image`], `size` points square.
fn icon_image_at(name: &str, size: f64) -> Option<Retained<NSImage>> {
    use resvg::{tiny_skia, usvg};
    let svg = crate::ui::Assets::get(&format!("icons/{name}.svg"))?;
    let tree = usvg::Tree::from_data(svg, &usvg::Options::default()).ok()?;
    let px = (size * 2.0) as u32;
    let mut pixmap = tiny_skia::Pixmap::new(px, px)?;
    let scale = px as f32 / tree.size().width();
    resvg::render(&tree, tiny_skia::Transform::from_scale(scale, scale), &mut pixmap.as_mut());
    // A template only uses the alpha; tiny-skia's colour is premultiplied,
    // so keep the alpha alone.
    let rgba: Vec<u8> = pixmap.data().as_chunks::<4>().0.iter().flat_map(|p| [0, 0, 0, p[3]]).collect();
    let image = image_from_rgba(&rgba, px as usize, px as usize, NSSize::new(size, size))?;
    image.setTemplate(true);
    Some(image)
}

/// The idle Clawd at 2x as an image `CLAWD_W` x `CLAWD_H` points: whole
/// pixels per cell, as published (the art module's sprite).
fn clawd_image() -> Option<Retained<NSImage>> {
    let k = (CELL * 2.0) as usize;
    let (w, h) = (IDLE_W * k, IDLE_H * k);
    let mut rgba = vec![0u8; w * h * 4];
    for r in 0..IDLE_H {
        for c in 0..IDLE_W {
            let v = idle_cell(c, r);
            if v == 0 {
                continue;
            }
            let rgb = cell_rgb(v);
            for y in r * k..(r + 1) * k {
                for x in c * k..(c + 1) * k {
                    let i = (y * w + x) * 4;
                    rgba[i..i + 4].copy_from_slice(&[(rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, 0xff]);
                }
            }
        }
    }
    image_from_rgba(&rgba, w, h, NSSize::new(CLAWD_W, CLAWD_H))
}

/// The view's appearance is a dark one.
fn is_dark(view: &NSView) -> bool {
    // SAFETY: AppKit's appearance name constants, read on the main thread.
    let (aqua, dark) = unsafe { (NSAppearanceNameAqua, NSAppearanceNameDarkAqua) };
    let names = NSArray::from_slice(&[aqua, dark]);
    view.effectiveAppearance().bestMatchFromAppearancesWithNames(&names).is_some_and(|n| &*n == dark)
}

/// muda's NSMenu for `menu` (the one the status item opens).
pub fn ns_menu(menu: &tray_icon::menu::Menu) -> Retained<NSMenu> {
    let ptr = tray_icon::menu::ContextMenu::ns_menu(menu).cast::<NSMenu>();
    // SAFETY: muda creates the NSMenu with the Menu and keeps it for the
    // Menu's life; retaining it here only adds a reference.
    unsafe { Retained::retain(ptr) }.expect("muda menus have an NSMenu")
}

impl MenuHeader {
    /// Build the panel and put it at the top of muda's `menu`. Call before
    /// appending any items, on the main thread.
    pub fn install(menu: &tray_icon::menu::Menu) -> Self {
        let mtm = MainThreadMarker::new().expect("the tray menu is built on the main thread");
        Self::install_in(mtm, &ns_menu(menu))
    }

    fn install_in(mtm: MainThreadMarker, menu: &NSMenu) -> Self {
        let view = HeaderView::new(mtm, rect(0.0, 0.0, W, 100.0));
        // SAFETY: AppKit's font weight constants, read on the main thread.
        let (regular, medium, semibold) = unsafe { (NSFontWeightRegular, NSFontWeightMedium, NSFontWeightSemibold) };
        let primary = NSColor::labelColor();
        let secondary = NSColor::secondaryLabelColor();
        let small = NSFont::monospacedDigitSystemFontOfSize_weight(11.0, regular);

        if let Some(image) = clawd_image() {
            let clawd = NSImageView::imageViewWithImage(&image, mtm);
            clawd.setFrame(rect(PAD, 10.0, CLAWD_W, CLAWD_H));
            view.addSubview(&clawd);
        }
        let title = label(mtm, &NSFont::systemFontOfSize_weight(13.0, semibold), &primary);
        // The title leaves the corner to the reload button.
        title.setFrame(rect(TEXT_X, 10.0, W - ICON_INSET - ICON - (HIT - ICON) / 2.0 - 6.0 - TEXT_X, 17.0));
        let detail = label(mtm, &NSFont::systemFontOfSize_weight(11.0, regular), &secondary);
        // A long folder loses its middle, keeping the drive and the name.
        detail.setLineBreakMode(NSLineBreakMode::ByTruncatingMiddle);
        let age = label(mtm, &small, &secondary);
        view.addSubview(&title);
        view.addSubview(&detail);
        view.addSubview(&age);

        // The reload button and, while a check runs, a spinner in its place.
        let pad = (HIT - ICON) / 2.0;
        let hit = rect(GLYPH.origin.x - pad, GLYPH.origin.y - pad, HIT, HIT);
        let target: &AnyObject = &view;
        let reload = HoverButton::new(mtm, "refresh-cw", "Refresh usage", target, sel!(refresh:));
        reload.setFrame(hit);
        let spinner = NSProgressIndicator::initWithFrame(NSProgressIndicator::alloc(mtm), GLYPH);
        spinner.setStyle(NSProgressIndicatorStyle::Spinning);
        spinner.setControlSize(NSControlSize::Small);
        spinner.setIndeterminate(true);
        spinner.setDisplayedWhenStopped(false);
        view.addSubview(&reload);
        view.addSubview(&spinner);

        let meter = || {
            let m = MeterViews {
                value: label(mtm, &NSFont::monospacedDigitSystemFontOfSize_weight(22.0, medium), &primary),
                label: label(mtm, &NSFont::systemFontOfSize_weight(11.0, regular), &secondary),
                track: bar(mtm, 3.0, &NSColor::quaternaryLabelColor()),
                fill: bar(mtm, 3.0, &fill_color(Some(Tone::Ok), true)),
                tick: bar(mtm, 1.0, &primary),
                reset: label(mtm, &small, &secondary),
            };
            m.label.setAlignment(NSTextAlignment::Right);
            for v in [&*m.value, &*m.label, &*m.reset] {
                view.addSubview(v);
            }
            for b in [&m.track, &m.fill, &m.tick] {
                view.addSubview(b);
            }
            m
        };
        let meters = [meter(), meter()];

        // The Resets block, in the second meter's place.
        let chip_text = NSFont::monospacedDigitSystemFontOfSize_weight(11.0, medium);
        let ticket = NSImageView::new(mtm);
        if let Some(image) = icon_image_at("ticket", TICKET) {
            ticket.setImage(Some(&image));
        }
        let resets = ResetViews {
            title: label(mtm, &NSFont::systemFontOfSize_weight(13.0, medium), &primary),
            pill: bar(mtm, RESETS_HEAD_H / 2.0, &NSColor::quaternaryLabelColor()),
            ticket,
            count: label(mtm, &chip_text, &primary),
            chips: std::array::from_fn(|_| {
                let text = label(mtm, &chip_text, &primary);
                text.setAlignment(NSTextAlignment::Center);
                (bar(mtm, CHIP_H / 2.0, &NSColor::quaternaryLabelColor()), text)
            }),
            note: label(mtm, &small, &secondary),
        };
        resets.title.setStringValue(&NSString::from_str("Resets expire"));
        view.addSubview(&resets.title);
        view.addSubview(&resets.pill);
        view.addSubview(&resets.ticket);
        view.addSubview(&resets.count);
        for (chip, text) in &resets.chips {
            view.addSubview(chip);
            view.addSubview(text);
        }
        view.addSubview(&resets.note);

        // The footer: a line, Open Clawdmeter, and the icons on the right.
        let line = bar(mtm, 0.0, &NSColor::separatorColor());
        // SAFETY: the target is the panel view, which holds the button, and
        // `open:` is its method.
        let open = unsafe {
            NSButton::buttonWithTitle_target_action(
                &NSString::from_str("Open Clawdmeter"),
                Some(target),
                Some(sel!(open:)),
                mtm,
            )
        };
        open.setControlSize(NSControlSize::Regular);
        open.sizeToFit();
        let update = HoverButton::new(mtm, "arrow-down-to-line", "Update", target, sel!(update:));
        let update_dot = bar(mtm, 3.0, &fill_color(Some(Tone::Accent), true));
        let chime = HoverButton::new(mtm, "volume-2", CHIME_OFF_TIP, target, sel!(chime:));
        let chime_icons = [("volume-2", CHIME_OFF_TIP), ("volume-x", CHIME_ON_TIP)].map(|(icon, label)| {
            let image = icon_image(icon);
            if let Some(i) = &image {
                i.setAccessibilityDescription(Some(&NSString::from_str(label)));
            }
            image
        });
        let settings = HoverButton::new(mtm, "settings", "Settings", target, sel!(settings:));
        let quit = HoverButton::new(mtm, "power", "Quit Clawdmeter", target, sel!(quit:));
        quit.ivars().danger.set(true);
        view.addSubview(&line);
        view.addSubview(&open);
        for b in [&update, &chime, &settings, &quit] {
            view.addSubview(b);
        }
        view.addSubview(&update_dot);

        let item = NSMenuItem::new(mtm);
        item.setView(Some(&view));
        menu.insertItem_atIndex(&item, 0);
        MenuHeader {
            item,
            view,
            title,
            detail,
            age,
            reload,
            spinner,
            meters,
            resets,
            line,
            open,
            update,
            update_dot,
            chime,
            chime_icons,
            settings,
            quit,
            shown: None,
        }
    }

    /// Show `h`. Cheap when nothing changed; safe while the menu is open.
    pub fn update(&mut self, h: &Header) {
        let dark = is_dark(&self.view);
        if self.shown.as_ref().is_some_and(|(s, d)| s == h && *d == dark) {
            return;
        }
        self.title.setStringValue(&NSString::from_str(h.title));
        let checking = h.refresh == RefreshState::Checking;
        let was_checking = self.shown.as_ref().is_some_and(|(s, _)| s.refresh == RefreshState::Checking);
        let ready = h.refresh == RefreshState::Ready;
        self.reload.setHidden(checking);
        self.reload.setEnabled(ready);
        self.reload.tint();
        self.reload.setToolTip(Some(&NSString::from_str(&h.refresh_tip())));
        // SAFETY: the sender is only reported to observers; none is valid.
        if checking && !was_checking {
            unsafe { self.spinner.startAnimation(None) };
        } else if !checking && was_checking {
            unsafe { self.spinner.stopAnimation(None) };
        }
        // "~/.claude · updated 6 min ago", as the Overview writes it: the age
        // follows the folder and keeps its width; a long folder gives up its
        // middle to make room.
        let text_w = W - TEXT_X - PAD;
        let age = h.age.as_deref().map(|a| format!("· {a}"));
        self.age.setStringValue(&NSString::from_str(age.as_deref().unwrap_or("")));
        self.age.sizeToFit();
        let age_w = if age.is_some() { self.age.frame().size.width.ceil() } else { 0.0 };
        self.detail.setStringValue(&NSString::from_str(&h.detail));
        self.detail.sizeToFit();
        // The labels' own padding makes the gap one space wide.
        let room = if age.is_some() { text_w - age_w } else { text_w };
        let detail_w = self.detail.frame().size.width.ceil().min(room);
        self.detail.setFrame(rect(TEXT_X, 29.0, detail_w, 14.0));
        self.age.setFrame(rect(TEXT_X + detail_w, 29.0, age_w, 14.0));
        self.age.setHidden(age.is_none());
        let bar_w = W - 2.0 * PAD;
        for (i, m) in self.meters.iter().enumerate() {
            let Some(data) = h.meters.get(i) else {
                for v in [&**m.value, &**m.label, &**m.reset, &**m.track, &**m.fill, &**m.tick] {
                    v.setHidden(true);
                }
                continue;
            };
            let y = METERS_Y + i as f64 * (METER_H + METER_GAP);
            m.value.setStringValue(&NSString::from_str(&data.value));
            m.value.setFrame(rect(PAD, y, 140.0, 28.0));
            m.label.setStringValue(&NSString::from_str(data.label));
            m.label.setFrame(rect(W - PAD - 120.0, y + 9.0, 120.0, 14.0));
            m.track.setFrame(rect(PAD, y + 31.0, bar_w, 6.0));
            // A sliver of fill under 1 % still reads as "some", as the device's does.
            let fill_w = if data.frac > 0.0 { (bar_w * data.frac).max(6.0) } else { 0.0 };
            m.fill.setFrame(rect(PAD, y + 31.0, fill_w, 6.0));
            m.fill.setFillColor(&fill_color(data.fill, dark));
            m.fill.setHidden(fill_w == 0.0);
            // On a neutral (Codex) bar the marker is the opaque dim colour so
            // it shows on the fill, as the device's does.
            let tick = if data.fill.is_some() { NSColor::labelColor() } else { dim_color(dark) };
            m.tick.setFillColor(&tick);
            match data.pace {
                Some(p) => {
                    m.tick.setFrame(rect(PAD + (bar_w * p - 1.0).clamp(0.0, bar_w - 2.0), y + 29.0, 2.0, 10.0));
                    m.tick.setHidden(false);
                }
                None => m.tick.setHidden(true),
            }
            m.reset.setStringValue(&NSString::from_str(&data.reset));
            m.reset.setFrame(rect(PAD, y + 41.0, bar_w, 14.0));
            for v in [&**m.value, &**m.label, &**m.reset, &**m.track] {
                v.setHidden(false);
            }
        }
        // A weekly-only Codex plan's resets take the second meter's place.
        let slot = h.meters.len().min(1) as f64;
        self.layout_resets(h.resets.as_ref(), METERS_Y + slot * (METER_H + METER_GAP), dark);
        let n = (h.meters.len() + usize::from(h.resets.is_some())).min(2) as f64;
        let meters_end = METERS_Y + n * METER_H + (n - 1.0).max(0.0) * METER_GAP;
        let height = self.layout_footer(h, meters_end + FOOT_GAP, dark);
        if (self.view.frame().size.height - height).abs() > 0.5 {
            self.view.setFrameSize(NSSize::new(W, height));
            // Have an open menu re-measure the item (setting the same view
            // again may be ignored).
            self.item.setView(None);
            self.item.setView(Some(&self.view));
        }
        self.shown = Some((h.clone(), dark));
    }

    /// Lay out the Resets block from `y` (a meter's top), or hide it.
    fn layout_resets(&self, resets: Option<&Resets>, y: f64, dark: bool) {
        let r = &self.resets;
        let Some(data) = resets else {
            for v in [&**r.title, &**r.pill, &**r.ticket, &**r.count, &**r.note] {
                v.setHidden(true);
            }
            for (chip, text) in &r.chips {
                chip.setHidden(true);
                text.setHidden(true);
            }
            return;
        };
        let bar_w = W - 2.0 * PAD;
        // The header: the title, and the count in a pill at the right edge,
        // its ticket dim with none held, as the device draws it.
        r.title.setFrame(rect(PAD, y + 1.0, 160.0, 17.0));
        r.count.setStringValue(&NSString::from_str(&data.held.to_string()));
        r.count.sizeToFit();
        let count_w = r.count.frame().size.width.ceil();
        let pill_w = 8.0 + TICKET + 4.0 + count_w + 6.0;
        let pill_x = W - PAD - pill_w;
        r.pill.setFrame(rect(pill_x, y, pill_w, RESETS_HEAD_H));
        r.ticket.setFrame(rect(pill_x + 8.0, y + (RESETS_HEAD_H - TICKET) / 2.0, TICKET, TICKET));
        r.count.setFrame(rect(pill_x + 8.0 + TICKET + 4.0, y + 3.0, count_w, 14.0));
        let fg = if data.held == 0 { NSColor::secondaryLabelColor() } else { NSColor::labelColor() };
        r.ticket.setContentTintColor(Some(&fg));
        r.count.setTextColor(Some(&fg));
        for v in [&**r.title, &**r.pill, &**r.ticket, &**r.count] {
            v.setHidden(false);
        }
        // The row: equal chips across the panel, the soonest filled like a
        // Codex bar; or the line in their place.
        let row_y = y + RESETS_HEAD_H + 7.0;
        let chip_w = (bar_w - (RESET_CHIPS - 1) as f64 * CHIP_GAP) / RESET_CHIPS as f64;
        let more = (data.chips.more > 0).then(|| format!("+{} more", data.chips.more));
        let cells: Vec<&str> = data.chips.dates.iter().map(String::as_str).chain(more.as_deref()).collect();
        let theme = if dark { Theme::dark() } else { Theme::light() };
        for (i, (chip, text)) in r.chips.iter().enumerate() {
            let Some(cell) = cells.get(i) else {
                chip.setHidden(true);
                text.setHidden(true);
                continue;
            };
            let x = PAD + i as f64 * (chip_w + CHIP_GAP);
            chip.setFrame(rect(x, row_y, chip_w, CHIP_H));
            let soonest = i == 0 && !data.chips.dates.is_empty();
            let is_more = more.is_some() && i == cells.len() - 1;
            text.setStringValue(&NSString::from_str(cell));
            // "+N more" shortens to "+N" where it doesn't fit, as on the device.
            if is_more {
                text.sizeToFit();
                if text.frame().size.width > chip_w {
                    text.setStringValue(&NSString::from_str(&format!("+{}", data.chips.more)));
                }
            }
            text.setFrame(rect(x, row_y + (CHIP_H - 14.0) / 2.0, chip_w, 14.0));
            if soonest {
                chip.setFillColor(&fill_color(None, dark));
                text.setTextColor(Some(&rgba_color(theme.panel)));
            } else {
                chip.setFillColor(&NSColor::quaternaryLabelColor());
                let color = if is_more { NSColor::secondaryLabelColor() } else { NSColor::labelColor() };
                text.setTextColor(Some(&color));
            }
            chip.setHidden(false);
            text.setHidden(false);
        }
        match data.chips.note {
            Some(note) => {
                r.note.setStringValue(&NSString::from_str(note));
                r.note.setFrame(rect(PAD, row_y + (CHIP_H - 14.0) / 2.0, bar_w, 14.0));
                r.note.setHidden(false);
            }
            None => r.note.setHidden(true),
        }
    }

    /// Lay out the footer from `top` (its line); the panel's height.
    fn layout_footer(&self, h: &Header, top: f64, dark: bool) -> f64 {
        self.line.setFrame(rect(PAD, top, W - 2.0 * PAD, 1.0));
        let row = top + 1.0 + FOOT_PAD;
        let mid = row + FOOT_ROW / 2.0;
        let open = self.open.frame().size;
        // Its bezel fills its frame: its edge lines up with the meters.
        self.open.setFrameOrigin(NSPoint::new(PAD, (mid - open.height / 2.0).round()));
        // Right to left from Quit, whose glyph sits under the reload one.
        let mut glyph_x = W - ICON_INSET - ICON;
        let pad = (HIT - ICON) / 2.0;
        let mut place = |b: &HoverButton, shown: bool| {
            b.setHidden(!shown);
            if shown {
                b.setFrame(rect(glyph_x - pad, mid - HIT / 2.0, HIT, HIT));
                glyph_x -= HIT + 4.0;
            }
        };
        place(&self.quit, true);
        place(&self.settings, true);
        place(&self.chime, true);
        place(&self.update, h.update.is_some());
        // The speaker shows the state; the tooltip says what a click does.
        let [on, off] = &self.chime_icons;
        if h.chime {
            self.chime.show(on.as_deref(), CHIME_OFF_TIP);
        } else {
            self.chime.show(off.as_deref(), CHIME_ON_TIP);
        }
        // A stale highlight (the menu closed under the pointer) clears here.
        for b in [&self.reload, &self.update, &self.chime, &self.settings, &self.quit] {
            b.tint();
        }
        if let Some(v) = &h.update {
            self.update.setToolTip(Some(&NSString::from_str(&v.label())));
            let f = self.update.frame();
            let glyph_right = f.origin.x + pad + ICON;
            self.update_dot.setFrame(rect(glyph_right - 3.0, f.origin.y + pad - 3.0, 6.0, 6.0));
            self.update_dot.setFillColor(&fill_color(Some(Tone::Accent), dark));
        }
        self.update_dot.setHidden(h.update.is_none());
        row + FOOT_ROW + FOOT_PAD
    }

    /// Render the header as a PNG on a menu-like background, in light or dark
    /// mode (the `tray_header` example; reviews without opening the menu).
    /// It takes the view out of its menu item, so it consumes the header:
    /// never for the one in the tray.
    pub fn preview_png(mut self, h: &Header, dark: bool) -> Option<Vec<u8>> {
        // SAFETY: AppKit's appearance name constants, read on the main thread.
        let name = unsafe {
            if dark {
                NSAppearanceNameDarkAqua
            } else {
                NSAppearanceNameAqua
            }
        };
        let appearance = NSAppearance::appearanceNamed(name)?;
        self.view.setAppearance(Some(&appearance));
        self.shown = None;
        self.update(h);
        let size = self.view.frame().size;
        let mtm = MainThreadMarker::from(&*self.view);
        let back = bar(mtm, 0.0, &srgb(if dark { 0x2b2b2d } else { 0xececec }));
        back.setContentViewMargins(NSSize::new(0.0, 0.0));
        back.setFrame(rect(0.0, 0.0, size.width, size.height));
        back.setAppearance(Some(&appearance));
        self.view.setFrameOrigin(NSPoint::new(0.0, 0.0));
        back.setContentView(Some(&self.view));
        let bounds = back.bounds();
        let rep = back.bitmapImageRepForCachingDisplayInRect(bounds)?;
        back.cacheDisplayInRect_toBitmapImageRep(bounds, &rep);
        let props = objc2_foundation::NSDictionary::new();
        // SAFETY: an empty properties dictionary is valid for PNG.
        let data =
            unsafe { rep.representationUsingType_properties(objc2_app_kit::NSBitmapImageFileType::PNG, &props) }?;
        Some(data.to_vec())
    }
}
