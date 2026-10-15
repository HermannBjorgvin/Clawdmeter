//! BLE link to a Clawdmeter (btleplug).
//!
//! Finding the device: only a device the OS already
//! knows is used (paired on Linux, OS-connected on macOS/Windows, where the
//! bonded HID keyboard link keeps it connected), matched by the custom data
//! service or, failing that, by the exact name `Clawdmeter`. There is no
//! name scan by default, so a stranger's unit in range is never picked; a
//! filtered scan is available for first-time setup (`allow_scan`).
//!
//! The chosen device's address is cached in `ble-address` in the state dir
//! (`~/.config/clawdmeter`); a failed connect drops the cache so the next
//! attempt re-derives it.
//!
//! Per-OS recovery:
//!
//! * Linux: the device is marked trusted in BlueZ on connect (like
//!   `bluetoothctl trust`), so BlueZ reconnects it by itself.
//! * macOS: a connect that fails to encrypt (CoreBluetooth error 15, a bond
//!   left over from before a reflash or the on-device bond clear) unpairs
//!   the stale bond with `blueutil` when it is installed, so the next attempt
//!   re-bonds silently.
//! * Windows: a bonded device the OS isn't currently connected to is reached
//!   by address: `CLAWDMETER_BLE_ADDRESS`, the cached address, or the
//!   address in the Bluetooth PnP table.
//! * `CLAWDMETER_BLE_ADDRESS` (every OS) names the device to try first.
//! * Everywhere: writes that keep failing on a link that still reports
//!   itself connected (a half-open "zombie" link) make
//!   [`DeviceLink::is_connected`] report false, so the caller reconnects.
//!
//! Reconnect timing is [`Reconnect`] (backoff capped at
//! [`reconnect::BACKOFF_CAP`]), cut short by [`spawn_watcher`]'s wakeups when
//! the OS sees the device come back (BlueZ auto-reconnecting the bonded
//! device, or an advertisement during a scan).

#![warn(missing_docs)]

pub mod reconnect;
mod watch;

pub use reconnect::Reconnect;
pub use watch::{should_wake, spawn_watcher, Sighting, Wake, Wakeups};

use std::fs;
use std::io;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

use btleplug::api::{
    BDAddr, Central, CentralState, Characteristic, Manager as _, Peripheral as _, RetrievePeripheralsOptions,
    ScanFilter, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral};
use futures::{Stream, StreamExt};
use tokio::sync::mpsc;
use uuid::Uuid;

pub use clawdmeter_core::DEVICE_NAME;

/// Custom data service.
pub const SERVICE_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000001);
/// Host writes JSON here.
pub const RX_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000002);
/// Device ack/nack notifications (unused by the daemon).
pub const TX_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000003);
/// Device-initiated refresh request (notify `0x01`).
pub const REQ_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000004);
/// Standard HID service, which the device also exposes.
pub const HID_SERVICE_UUID: Uuid = Uuid::from_u128(0x00001812_0000_1000_8000_00805f9b34fb);

/// Largest payload written in one go. The firmware's RX buffer is 512 bytes;
/// writes use write-with-response, which the stacks split into a long write
/// when it exceeds the MTU.
pub const MAX_WRITE: usize = 500;

/// Link failures.
#[derive(Debug)]
pub enum LinkError {
    /// No Bluetooth adapter, or it is off.
    NoAdapter(String),
    /// No paired / OS-connected Clawdmeter.
    NotFound,
    /// The connect, discovery or a write failed.
    Ble(btleplug::Error),
    /// The connect or discovery took too long.
    Timeout(&'static str),
    /// The device lacks the data service's RX characteristic.
    MissingCharacteristic,
}

impl std::fmt::Display for LinkError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LinkError::NoAdapter(s) => write!(f, "no Bluetooth adapter: {s}"),
            LinkError::NotFound => write!(f, "no paired {DEVICE_NAME} found"),
            LinkError::Ble(e) => write!(f, "BLE error: {e}"),
            LinkError::Timeout(what) => write!(f, "timed out during {what}"),
            LinkError::MissingCharacteristic => write!(f, "device has no Clawdmeter RX characteristic"),
        }
    }
}

impl std::error::Error for LinkError {}

impl From<btleplug::Error> for LinkError {
    fn from(e: btleplug::Error) -> Self {
        LinkError::Ble(e)
    }
}

/// The one-line address cache.
#[derive(Debug, Clone)]
pub struct AddressCache {
    path: PathBuf,
}

impl AddressCache {
    /// Cache at `path` (normally `Paths::ble_address_file()`).
    pub fn new(path: impl Into<PathBuf>) -> Self {
        AddressCache { path: path.into() }
    }

    /// The cached key (MAC on Linux/Windows, CoreBluetooth UUID on macOS).
    pub fn load(&self) -> Option<String> {
        let raw = fs::read_to_string(&self.path).ok()?;
        let key = raw.lines().next()?.trim().to_owned();
        if key.is_empty() {
            return None;
        }
        // Only MACs (or btleplug ids) are ever written; reject garbage.
        if key.contains(':') && !is_mac(&key) {
            let _ = fs::remove_file(&self.path);
            return None;
        }
        Some(key)
    }

    /// Remember a device key.
    pub fn save(&self, key: &str) -> io::Result<()> {
        if let Some(d) = self.path.parent() {
            fs::create_dir_all(d)?;
        }
        fs::write(&self.path, format!("{key}\n"))
    }

    /// Drop the cache, but only if it still holds `key` (another process may
    /// have re-cached a different device meanwhile).
    pub fn invalidate(&self, key: &str) {
        if self.load().is_some_and(|k| k.eq_ignore_ascii_case(key)) {
            let _ = fs::remove_file(&self.path);
        }
    }
}

fn is_mac(s: &str) -> bool {
    let parts: Vec<&str> = s.split(':').collect();
    parts.len() == 6 && parts.iter().all(|p| p.len() == 2 && p.chars().all(|c| c.is_ascii_hexdigit()))
}

/// The stable key for a peripheral: its MAC when the platform exposes one,
/// else its platform id (macOS UUID).
pub fn peripheral_key(p: &Peripheral) -> String {
    let addr = p.address();
    if addr != BDAddr::default() {
        addr.to_string()
    } else {
        p.id().to_string()
    }
}

/// Connect / discovery knobs.
#[derive(Debug, Clone)]
pub struct LinkOptions {
    /// Bound on `connect()`; CoreBluetooth can otherwise wedge forever on a
    /// half-open HID link.
    pub connect_timeout: Duration,
    /// Bound on GATT discovery.
    pub discover_timeout: Duration,
    /// Bound on the REQ subscription (optional; polling works without it).
    pub subscribe_timeout: Duration,
    /// Fall back to a service-filtered scan when no known device is found
    /// (first-time setup only).
    pub allow_scan: bool,
    /// How long that scan runs.
    pub scan_time: Duration,
}

impl Default for LinkOptions {
    fn default() -> Self {
        LinkOptions {
            connect_timeout: Duration::from_secs(20),
            discover_timeout: Duration::from_secs(15),
            subscribe_timeout: Duration::from_secs(10),
            allow_scan: false,
            scan_time: Duration::from_secs(8),
        }
    }
}

/// The first powered adapter.
pub async fn adapter() -> Result<Adapter, LinkError> {
    let manager = Manager::new().await.map_err(|e| LinkError::NoAdapter(e.to_string()))?;
    let adapters = manager.adapters().await.map_err(|e| LinkError::NoAdapter(e.to_string()))?;
    let adapter = adapters.into_iter().next().ok_or_else(|| LinkError::NoAdapter("none present".into()))?;
    if let Ok(CentralState::PoweredOff) = adapter.adapter_state().await {
        return Err(LinkError::NoAdapter("adapter is off".into()));
    }
    Ok(adapter)
}

/// A candidate device the OS knows about.
#[derive(Debug, Clone)]
pub struct Candidate {
    /// The btleplug handle.
    pub peripheral: Peripheral,
    /// Cache key.
    pub key: String,
    /// Advertised / cached name.
    pub name: Option<String>,
    /// Exposes the custom data service (unambiguous).
    pub has_service: bool,
}

pub(crate) async fn describe(p: Peripheral) -> Candidate {
    let props = p.properties().await.ok().flatten();
    let name = props.as_ref().and_then(|pr| pr.local_name.clone());
    let has_service = props.as_ref().is_some_and(|pr| pr.services.contains(&SERVICE_UUID))
        || p.services().iter().any(|s| s.uuid == SERVICE_UUID);
    Candidate { key: peripheral_key(&p), peripheral: p, name, has_service }
}

/// True when a candidate is ours: the custom service alone is enough; the
/// HID service (shared with every keyboard) needs the exact name.
pub fn accept(name: Option<&str>, has_service: bool) -> bool {
    has_service || name == Some(DEVICE_NAME)
}

/// Pick among accepted candidates: the cached key first, else the first.
pub fn choose<'a>(cands: &'a [Candidate], cached: Option<&str>) -> Option<&'a Candidate> {
    connect_order(cands, cached).into_iter().next()
}

/// The order to try candidates in: the cached key first, then the rest as
/// listed. Trying them all means a second paired unit that is switched off
/// (e.g. the board that was swapped out) can't block the one that's on.
pub fn connect_order<'a>(cands: &'a [Candidate], cached: Option<&str>) -> Vec<&'a Candidate> {
    let is_cached = |c: &Candidate| cached.is_some_and(|k| c.key.eq_ignore_ascii_case(k));
    cands.iter().filter(|c| is_cached(c)).chain(cands.iter().filter(|c| !is_cached(c))).collect()
}

/// List Clawdmeters the OS already knows, without scanning.
pub async fn known_devices(adapter: &Adapter) -> Vec<Candidate> {
    let mut list: Vec<Peripheral> = adapter
        .retrieve_peripherals(RetrievePeripheralsOptions {
            identifiers: None,
            services: Some(vec![SERVICE_UUID, HID_SERVICE_UUID]),
        })
        .await
        .unwrap_or_default();
    if cfg!(target_os = "linux") {
        // BlueZ also lists paired-but-disconnected devices; connecting to
        // them reconnects without a scan.
        for p in adapter.peripherals().await.unwrap_or_default() {
            if !list.iter().any(|q| q.id() == p.id()) {
                list.push(p);
            }
        }
    }
    let mut out = Vec::new();
    for p in list {
        let c = describe(p).await;
        if !accept(c.name.as_deref(), c.has_service) {
            continue;
        }
        #[cfg(target_os = "linux")]
        {
            // Never pick an unpaired unit BlueZ merely saw advertising.
            let id = c.peripheral.id().to_string();
            let paired = tokio::task::spawn_blocking(move || bluez::is_paired(&id)).await.unwrap_or(Ok(false));
            if !paired.unwrap_or(false) {
                continue;
            }
        }
        out.push(c);
    }
    out
}

/// Scan for advertisements of the data service (first-time setup).
pub async fn scan(adapter: &Adapter, time: Duration) -> Vec<Candidate> {
    if adapter.start_scan(ScanFilter { services: vec![SERVICE_UUID] }).await.is_err() {
        return Vec::new();
    }
    tokio::time::sleep(time).await;
    let _ = adapter.stop_scan().await;
    let mut out = Vec::new();
    for p in adapter.peripherals().await.unwrap_or_default() {
        let c = describe(p).await;
        if accept(c.name.as_deref(), c.has_service) {
            out.push(c);
        }
    }
    out
}

/// Consecutive failed writes after which a link that still claims to be
/// connected is treated as dead. The Windows daemon broke after one; two
/// rides out a single transient error without stalling the 120 s budget.
pub const ZOMBIE_WRITE_FAILURES: u32 = 2;

/// Counts consecutive write failures (the zombie-link breaker).
#[derive(Debug, Default)]
pub struct WriteHealth {
    failures: AtomicU32,
}

impl WriteHealth {
    /// Record one write outcome.
    pub fn record(&self, ok: bool) {
        if ok {
            self.failures.store(0, Ordering::Relaxed);
        } else {
            self.failures.fetch_add(1, Ordering::Relaxed);
        }
    }

    /// Too many failures in a row: abandon the link.
    pub fn is_zombie(&self) -> bool {
        self.failures.load(Ordering::Relaxed) >= ZOMBIE_WRITE_FAILURES
    }
}

/// A canonical MAC from a Windows Bluetooth PnP instance id:
/// `BTHLE\DEV_98A316A5D706\7&B8081D1&0&98A316A5D706` → `98:A3:16:A5:D7:06`.
pub fn mac_from_pnp_instance_id(instance_id: &str) -> Option<String> {
    let up = instance_id.to_ascii_uppercase();
    let mut rest = up.as_str();
    while let Some(i) = rest.find("DEV_") {
        let tail = &rest[i + 4..];
        let hex: String = tail.chars().take_while(char::is_ascii_hexdigit).collect();
        if hex.len() == 12 {
            let pairs: Vec<&str> = (0..6).map(|k| &hex[k * 2..k * 2 + 2]).collect();
            return Some(pairs.join(":"));
        }
        rest = tail;
    }
    None
}

/// A MAC string (`AA:BB:CC:DD:EE:FF`, any case) as a [`BDAddr`].
pub fn parse_mac(s: &str) -> Option<BDAddr> {
    let s = s.trim();
    if !is_mac(s) {
        return None;
    }
    BDAddr::from_str_delim(s).ok()
}

/// True when a connect error is macOS's stale-bond failure (CBErrorDomain
/// code 15, "Failed to encrypt the connection").
pub fn is_encryption_error(msg: &str) -> bool {
    let m = msg.to_ascii_lowercase();
    m.contains("code=15") || m.contains("encrypt")
}

/// The address of the paired device named `name` in `blueutil --paired`
/// output (`address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...`).
pub fn blueutil_paired_address(out: &str, name: &str) -> Option<String> {
    let needle = format!("name: \"{name}\"");
    out.lines().filter(|l| l.contains(&needle)).find_map(|l| {
        let rest = &l[l.find("address:")? + "address:".len()..];
        let addr: String =
            rest.trim_start().chars().take_while(|c| c.is_ascii_hexdigit() || *c == '-' || *c == ':').collect();
        (!addr.is_empty()).then_some(addr)
    })
}

/// `CLAWDMETER_BLE_ADDRESS`: a MAC (or, on macOS, a CoreBluetooth UUID)
/// to try before any other known device.
pub fn pinned_address() -> Option<String> {
    std::env::var("CLAWDMETER_BLE_ADDRESS").ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
}

/// What the device asks or tells the app through its notifications.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceEvent {
    /// REQ: it has no data yet and wants a payload.
    Refresh,
    /// TX `{"dismiss":true}`: the alert ended on the device (a tap, or its
    /// own timeout).
    Dismissed,
}

/// The device's events, in order.
pub type DeviceEvents = mpsc::UnboundedReceiver<DeviceEvent>;

/// Read one notification. TX also carries the write acks
/// (`{"ack":true}` / `{"err":true}`), which mean nothing here.
pub fn device_event(uuid: Uuid, value: &[u8]) -> Option<DeviceEvent> {
    if uuid == REQ_UUID {
        return Some(DeviceEvent::Refresh);
    }
    if uuid != TX_UUID {
        return None;
    }
    let text: String = std::str::from_utf8(value).ok()?.chars().filter(|c| !c.is_whitespace()).collect();
    text.contains("\"dismiss\":true").then_some(DeviceEvent::Dismissed)
}

/// An open GATT link.
pub struct DeviceLink {
    peripheral: Peripheral,
    rx: Characteristic,
    key: String,
    name: Option<String>,
    health: WriteHealth,
}

impl std::fmt::Debug for DeviceLink {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DeviceLink").field("key", &self.key).field("name", &self.name).finish()
    }
}

impl DeviceLink {
    /// Find, connect, discover and subscribe. On a failed connect the address
    /// cache is dropped (if it pointed at that device).
    pub async fn open(
        adapter: &Adapter,
        cache: &AddressCache,
        opts: &LinkOptions,
    ) -> Result<(DeviceLink, DeviceEvents), LinkError> {
        // CLAWDMETER_BLE_ADDRESS pins the device to try first (the bash
        // daemon's DEVICE_MAC, the Windows daemon's override); else the cache.
        let cached = pinned_address().or_else(|| cache.load());
        let mut cands = known_devices(adapter).await;
        if cands.is_empty() {
            cands = bonded_by_address(adapter, cached.as_deref()).await;
        }
        if cands.is_empty() && opts.allow_scan {
            cands = scan(adapter, opts.scan_time).await;
        }
        let mut last_err = LinkError::NotFound;
        for cand in connect_order(&cands, cached.as_deref()) {
            match Self::connect(cand.clone(), opts).await {
                Ok(ok) => {
                    if cached.as_deref() != Some(cand.key.as_str()) {
                        let _ = cache.save(&cand.key);
                    }
                    return Ok(ok);
                }
                Err(e) => {
                    log::warn!("connect to {} failed: {e}", cand.key);
                    if cfg!(target_os = "macos") && is_encryption_error(&e.to_string()) {
                        log::warn!("encryption failed: likely a stale macOS bond; trying to unpair it");
                        let _ = tokio::task::spawn_blocking(macos_unpair_stale_bond).await;
                    }
                    cache.invalidate(&cand.key);
                    last_err = e;
                }
            }
        }
        Err(last_err)
    }

    /// Connect to a specific candidate.
    pub async fn connect(cand: Candidate, opts: &LinkOptions) -> Result<(DeviceLink, DeviceEvents), LinkError> {
        let p = cand.peripheral;
        if !p.is_connected().await.unwrap_or(false) {
            let r = match tokio::time::timeout(opts.connect_timeout, p.connect()).await {
                Ok(r) => r.map_err(LinkError::Ble),
                Err(_) => Err(LinkError::Timeout("connect")),
            };
            if let Err(e) = r {
                // Cancel BlueZ's pending Connect, or the next attempt fails
                // with "Operation already in progress".
                let _ = p.disconnect().await;
                return Err(e);
            }
        }
        tokio::time::timeout(opts.discover_timeout, p.discover_services())
            .await
            .map_err(|_| LinkError::Timeout("service discovery"))??;
        let chars = p.characteristics();
        let rx = chars
            .iter()
            .find(|c| c.uuid == RX_UUID && c.service_uuid == SERVICE_UUID)
            .cloned()
            .ok_or(LinkError::MissingCharacteristic)?;
        let (tx, events) = mpsc::unbounded_channel();
        let notifying: Vec<_> = chars
            .iter()
            .filter(|c| (c.uuid == REQ_UUID || c.uuid == TX_UUID) && c.service_uuid == SERVICE_UUID)
            .cloned()
            .collect();
        if !notifying.is_empty() {
            // Listen before subscribing: the device fires its REQ notify
            // inside the subscribe (it asks only when it has no data yet).
            match p.notifications().await {
                Ok(stream) => spawn_event_forwarder(stream, tx),
                Err(e) => log::warn!("no notification stream: {e}"),
            }
        }
        for c in &notifying {
            let what = if c.uuid == REQ_UUID { "refresh" } else { "dismiss" };
            match tokio::time::timeout(opts.subscribe_timeout, p.subscribe(c)).await {
                Ok(Ok(())) => {}
                Ok(Err(e)) => log::warn!("{what} subscription unavailable: {e}"),
                Err(_) => log::warn!("{what} subscription timed out; carrying on without it"),
            }
        }
        #[cfg(target_os = "linux")]
        {
            // `bluetoothctl trust`: BlueZ then
            // reconnects the device on its own after it drops.
            let id = p.id().to_string();
            if let Ok(Err(e)) = tokio::task::spawn_blocking(move || bluez::trust(&id)).await {
                log::debug!("could not mark the device trusted: {e}");
            }
        }
        Ok((DeviceLink { key: cand.key, name: cand.name, peripheral: p, rx, health: WriteHealth::default() }, events))
    }

    /// Write one payload (already encoded to at most [`MAX_WRITE`] bytes).
    pub async fn write(&self, data: &[u8]) -> Result<(), LinkError> {
        let r = match tokio::time::timeout(
            Duration::from_secs(10),
            self.peripheral.write(&self.rx, data, WriteType::WithResponse),
        )
        .await
        {
            Ok(r) => r.map_err(LinkError::Ble),
            Err(_) => Err(LinkError::Timeout("write")),
        };
        self.health.record(r.is_ok());
        if self.health.is_zombie() {
            log::warn!("{ZOMBIE_WRITE_FAILURES} writes in a row failed on a live-looking link; dropping it");
        }
        r
    }

    /// Still connected? False once writes keep failing (a zombie link), so
    /// the caller drops it and reconnects.
    pub async fn is_connected(&self) -> bool {
        !self.health.is_zombie() && self.peripheral.is_connected().await.unwrap_or(false)
    }

    /// Drop the GATT connection (the OS may keep the HID link).
    pub async fn close(&self) {
        let _ = self.peripheral.disconnect().await;
    }

    /// Cache key (address).
    pub fn key(&self) -> &str {
        &self.key
    }

    /// Device name, if known.
    pub fn name(&self) -> Option<&str> {
        self.name.as_deref()
    }

    /// Platform id string (BlueZ: `hci0/dev_AA_BB_...`).
    pub fn id(&self) -> String {
        self.peripheral.id().to_string()
    }
}

/// Windows: a bonded device that Windows isn't connected to right now is
/// absent from `known_devices` (which lists connected peripherals only), but
/// WinRT can still connect to it by address. Addresses come from
/// `CLAWDMETER_BLE_ADDRESS`, then the cache, then the PnP table. Elsewhere
/// this finds nothing: BlueZ already lists paired devices, and macOS has no
/// way to reach a peripheral the OS doesn't hold.
#[cfg(not(windows))]
async fn bonded_by_address(_adapter: &Adapter, _cached: Option<&str>) -> Vec<Candidate> {
    Vec::new()
}

#[cfg(windows)]
async fn bonded_by_address(adapter: &Adapter, cached: Option<&str>) -> Vec<Candidate> {
    use btleplug::platform::PeripheralId;
    // `cached` already prefers CLAWDMETER_BLE_ADDRESS over the cache file.
    let mut addrs: Vec<String> = cached.map(str::to_owned).into_iter().collect();
    if addrs.iter().all(|a| parse_mac(a).is_none()) {
        if let Ok(Some(mac)) = tokio::task::spawn_blocking(windows_pnp_address).await {
            addrs.push(mac);
        }
    }
    let mut out: Vec<Candidate> = Vec::new();
    for addr in addrs.iter().filter_map(|a| parse_mac(a)) {
        if out.iter().any(|c| c.key.eq_ignore_ascii_case(&addr.to_string())) {
            continue;
        }
        if let Ok(p) = adapter.add_peripheral(&PeripheralId::from(addr)).await {
            log::info!("trying bonded address {addr}");
            out.push(Candidate {
                key: addr.to_string(),
                peripheral: p,
                name: Some(DEVICE_NAME.into()),
                has_service: false,
            });
        }
    }
    out
}

/// The bonded Clawdmeter's MAC from the Windows PnP table (the Windows
/// daemon's `Get-PnpDevice` lookup). `None` on any failure.
#[cfg(windows)]
fn windows_pnp_address() -> Option<String> {
    use std::os::windows::process::CommandExt;
    let script = format!(
        "Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | \
         Where-Object {{ $_.FriendlyName -eq '{DEVICE_NAME}' }} | Select-Object -ExpandProperty InstanceId"
    );
    let out = std::process::Command::new("powershell")
        .args(["-NoProfile", "-NonInteractive", "-Command", &script])
        .stdin(std::process::Stdio::null())
        .creation_flags(0x0800_0000) // CREATE_NO_WINDOW
        .output()
        .ok()?;
    String::from_utf8_lossy(&out.stdout).lines().find_map(mac_from_pnp_instance_id)
}

/// macOS: forget a stale bond for the device with `blueutil` (CoreBluetooth
/// has no unpair API). The firmware pairs "just works", so the next connect
/// re-bonds with no prompt. Logs a manual fix when blueutil is missing or
/// lacks Bluetooth permission (it hangs then, hence the timeout).
fn macos_unpair_stale_bond() {
    if !cfg!(target_os = "macos") {
        return;
    }
    let run = |args: &[&str]| -> Option<String> {
        let mut child = std::process::Command::new("blueutil")
            .args(args)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::null())
            .spawn()
            .ok()?;
        let start = std::time::Instant::now();
        loop {
            match child.try_wait() {
                Ok(Some(st)) if st.success() => break,
                Ok(Some(_)) | Err(_) => return None,
                Ok(None) if start.elapsed() > Duration::from_secs(8) => {
                    let _ = child.kill();
                    log::warn!(
                        "blueutil timed out; it likely lacks Bluetooth permission (System Settings > \
                         Privacy & Security > Bluetooth)"
                    );
                    return None;
                }
                Ok(None) => std::thread::sleep(Duration::from_millis(100)),
            }
        }
        let mut s = String::new();
        io::Read::read_to_string(&mut child.stdout.take()?, &mut s).ok()?;
        Some(s)
    };
    let Some(paired) = run(&["--paired"]) else {
        log::warn!(
            "can't clear the stale bond automatically: install blueutil (`brew install blueutil`), or forget \
             '{DEVICE_NAME}' in System Settings > Bluetooth and connect again"
        );
        return;
    };
    match blueutil_paired_address(&paired, DEVICE_NAME) {
        Some(addr) if run(&["--unpair", &addr]).is_some() => {
            log::info!("unpaired the stale bond for '{DEVICE_NAME}' [{addr}]; it re-pairs on the next connect")
        }
        Some(addr) => log::warn!("blueutil could not unpair {addr}"),
        None => log::info!("no paired '{DEVICE_NAME}' to unpair"),
    }
}

fn spawn_event_forwarder(
    mut stream: Pin<Box<dyn Stream<Item = btleplug::api::ValueNotification> + Send>>,
    tx: mpsc::UnboundedSender<DeviceEvent>,
) {
    tokio::spawn(async move {
        while let Some(n) = stream.next().await {
            if let Some(ev) = device_event(n.uuid, &n.value) {
                if tx.send(ev).is_err() {
                    break;
                }
            }
        }
    });
}

/// Forget a device in BlueZ (unpair). Not called by the daemon loop: doing it
/// on connect failures would strand a device that is only ever found among
/// paired devices. Exposed for an explicit "Forget device" action.
#[cfg(target_os = "linux")]
pub async fn forget_device(link_id: String) -> Result<(), String> {
    tokio::task::spawn_blocking(move || bluez::remove_device(&link_id)).await.map_err(|e| e.to_string())?
}

#[cfg(target_os = "linux")]
mod bluez {
    //! Two BlueZ D-Bus calls btleplug doesn't expose.
    use dbus::blocking::stdintf::org_freedesktop_dbus::Properties;
    use dbus::blocking::Connection;
    use std::time::Duration;

    fn object_path(id: &str) -> String {
        if id.starts_with('/') {
            id.to_owned()
        } else {
            format!("/org/bluez/{id}")
        }
    }

    /// `org.bluez.Device1.Paired` for a btleplug id (`hci0/dev_...`).
    pub fn is_paired(id: &str) -> Result<bool, String> {
        let conn = Connection::new_system().map_err(|e| e.to_string())?;
        let path = object_path(id);
        let proxy = conn.with_proxy("org.bluez", path, Duration::from_secs(5));
        proxy.get::<bool>("org.bluez.Device1", "Paired").map_err(|e| e.to_string())
    }

    /// `org.bluez.Device1.Trusted = true` (`bluetoothctl trust`).
    pub fn trust(id: &str) -> Result<(), String> {
        use dbus::blocking::stdintf::org_freedesktop_dbus::Properties as _;
        let conn = Connection::new_system().map_err(|e| e.to_string())?;
        let proxy = conn.with_proxy("org.bluez", object_path(id), Duration::from_secs(5));
        if proxy.get::<bool>("org.bluez.Device1", "Trusted").unwrap_or(false) {
            return Ok(());
        }
        proxy.set("org.bluez.Device1", "Trusted", true).map_err(|e| e.to_string())
    }

    /// `org.bluez.Adapter1.RemoveDevice` on the device's adapter.
    pub fn remove_device(id: &str) -> Result<(), String> {
        let conn = Connection::new_system().map_err(|e| e.to_string())?;
        let path = object_path(id);
        let adapter = path.rsplit_once('/').map(|(a, _)| a.to_owned()).ok_or("bad device path")?;
        let proxy = conn.with_proxy("org.bluez", adapter, Duration::from_secs(5));
        let dev = dbus::Path::new(path).map_err(|e| e.to_string())?;
        proxy.method_call::<(), _, _, _>("org.bluez.Adapter1", "RemoveDevice", (dev,)).map_err(|e| e.to_string())
    }

    #[cfg(test)]
    mod tests {
        #[test]
        fn paths() {
            assert_eq!(super::object_path("hci0/dev_AA_BB"), "/org/bluez/hci0/dev_AA_BB");
            assert_eq!(super::object_path("/org/bluez/hci1/dev_X"), "/org/bluez/hci1/dev_X");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_events() {
        assert_eq!(device_event(REQ_UUID, &[1]), Some(DeviceEvent::Refresh));
        assert_eq!(device_event(TX_UUID, br#"{"dismiss":true}"#), Some(DeviceEvent::Dismissed));
        assert_eq!(device_event(TX_UUID, br#"{ "dismiss": true }"#), Some(DeviceEvent::Dismissed));
        assert_eq!(device_event(TX_UUID, br#"{"ack":true}"#), None);
        assert_eq!(device_event(TX_UUID, br#"{"err":true}"#), None);
        assert_eq!(device_event(TX_UUID, &[0xff, 0xfe]), None);
        assert_eq!(device_event(RX_UUID, br#"{"dismiss":true}"#), None);
        // The firmware's reply, as written.
        let src = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src/ble.cpp"),
        )
        .unwrap();
        assert!(src.contains(r#"tx_char->setValue("{\"dismiss\":true}");"#));
    }

    #[test]
    fn uuids_match_firmware() {
        assert_eq!(SERVICE_UUID.to_string(), "4c41555a-4465-7669-6365-000000000001");
        assert_eq!(RX_UUID.to_string(), "4c41555a-4465-7669-6365-000000000002");
        assert_eq!(TX_UUID.to_string(), "4c41555a-4465-7669-6365-000000000003");
        assert_eq!(REQ_UUID.to_string(), "4c41555a-4465-7669-6365-000000000004");
        let src = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src/ble.cpp"),
        );
        if let Ok(src) = src {
            for u in [SERVICE_UUID, RX_UUID, TX_UUID, REQ_UUID] {
                assert!(src.contains(&u.to_string()), "{u} not in firmware/src/ble.cpp");
            }
            assert!(src.contains("BLE_BUF_SIZE 512"), "RX buffer size changed; revisit MAX_WRITE");
        }
    }

    #[test]
    fn address_cache() {
        let dir = std::env::temp_dir().join(format!("cm-ble-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let c = AddressCache::new(dir.join("ble-address"));
        assert_eq!(c.load(), None);
        c.save("AA:BB:CC:DD:EE:FF").unwrap();
        assert_eq!(c.load().as_deref(), Some("AA:BB:CC:DD:EE:FF"));
        c.invalidate("11:22:33:44:55:66");
        assert!(c.load().is_some(), "only drops the matching key");
        c.invalidate("aa:bb:cc:dd:ee:ff");
        assert_eq!(c.load(), None);
        fs::write(dir.join("ble-address"), "AA:BB:zz\n").unwrap();
        assert_eq!(c.load(), None, "malformed MAC discarded");
        assert!(!dir.join("ble-address").exists());
        c.save("6F1C2E3A-0000-4000-8000-123456789ABC").unwrap();
        assert_eq!(c.load().as_deref(), Some("6F1C2E3A-0000-4000-8000-123456789ABC"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn pnp_instance_ids() {
        assert_eq!(
            mac_from_pnp_instance_id(r"BTHLE\DEV_98A316A5D706\7&B8081D1&0&98A316A5D706").as_deref(),
            Some("98:A3:16:A5:D7:06")
        );
        assert_eq!(mac_from_pnp_instance_id(r"BTHLE\dev_98a316a5d706\x").as_deref(), Some("98:A3:16:A5:D7:06"));
        assert_eq!(mac_from_pnp_instance_id(r"BTHLE\SOMETHING\7&B8081D1"), None);
        assert_eq!(mac_from_pnp_instance_id(""), None);
        // A short hex run after DEV_ is not a MAC; neither is a 13-digit one.
        assert_eq!(mac_from_pnp_instance_id(r"BTHENUM\DEV_1234\x"), None);
        assert_eq!(mac_from_pnp_instance_id(r"BTHLE\DEV_98A316A5D7061\x"), None);
        assert_eq!(parse_mac("98:a3:16:a5:d7:06").map(|a| a.to_string()).as_deref(), Some("98:A3:16:A5:D7:06"));
        assert_eq!(parse_mac("6F1C2E3A-0000-4000-8000-123456789ABC"), None);
    }

    // Ported from test_windows_reconnect.py's zombie-link tests.
    #[test]
    fn zombie_breaker() {
        let h = WriteHealth::default();
        assert!(!h.is_zombie());
        h.record(false);
        assert!(!h.is_zombie(), "one transient failure is ridden out");
        h.record(true);
        h.record(false);
        assert!(!h.is_zombie(), "a success resets the count");
        h.record(false);
        assert!(h.is_zombie());
    }

    // The macOS daemon's stale-bond self-heal (unpair_macos).
    #[test]
    fn stale_bond_helpers() {
        assert!(is_encryption_error("Error Domain=CBErrorDomain Code=15 \"Failed to encrypt the connection\""));
        assert!(is_encryption_error("Failed to encrypt"));
        assert!(!is_encryption_error("timed out during connect"));
        let out = "address: 11-22-33-44-55-66, connected (master, -50 dBm), not favourite, paired, name: \"Keyboard\", recent access date: 2026\n\
                   address: 28-84-85-55-5c-3d, not connected, not favourite, paired, name: \"Clawdmeter\", recent access date: 2026\n";
        assert_eq!(blueutil_paired_address(out, "Clawdmeter").as_deref(), Some("28-84-85-55-5c-3d"));
        assert_eq!(blueutil_paired_address(out, "Other"), None);
    }

    #[test]
    fn acceptance_rules() {
        assert!(accept(None, true));
        assert!(accept(Some("Clawdmeter"), false));
        assert!(!accept(Some("Keychron K2"), false));
        assert!(!accept(Some("clawdmeter"), false));
        assert!(!accept(None, false));
    }
}
