//! Wakeups for the reconnect loop: the OS saw the device come back.
//!
//! Built on btleplug's central event stream, so it is portable. What each
//! stack reports:
//!
//! * BlueZ: a trusted, bonded device that comes back in range is reconnected
//!   by the kernel's background scan on its own. `org.bluez.Device1`'s
//!   `Connected` flips to true over D-Bus (`PropertiesChanged`), which
//!   btleplug turns into [`CentralEvent::DeviceConnected`]. RSSI and
//!   advertisement-data updates arrive whenever discovery is running (ours
//!   or another app's).
//! * CoreBluetooth / WinRT: connect events for peripherals this process
//!   handles and advertisements during scans; the HID bond there keeps the
//!   device OS-connected, so wakeups matter less.
//!
//! The watcher only reports devices that pass [`accept`]; whether a sighting
//! should wake the loop is [`should_wake`].

use std::collections::HashMap;
use std::time::{Duration, Instant};

use btleplug::api::{Central, CentralEvent};
use btleplug::platform::PeripheralId;
use futures::StreamExt;
use tokio::sync::mpsc;

use crate::{accept, describe, Bluetooth};

/// How the device showed up.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sighting {
    /// The OS holds a connection to it (BlueZ auto-reconnected it).
    Connected,
    /// It is advertising (seen by a running scan).
    Advertising,
}

/// One sighting of an accepted device.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Wake {
    /// Cache key (address / platform id) of the device.
    pub key: String,
    /// What was seen.
    pub sighting: Sighting,
}

/// Wakeup stream from [`spawn_watcher`].
pub type Wakeups = mpsc::UnboundedReceiver<Wake>;

/// Should this sighting cut the reconnect wait short? An OS-level connect of
/// an accepted device always does: we then only need GATT on top. An
/// advertisement only does for the device we're looking for (the cached
/// one, or any while nothing is cached), so a stranger's unit advertising
/// nearby can't keep the loop busy.
pub fn should_wake(w: &Wake, cached: Option<&str>) -> bool {
    match w.sighting {
        Sighting::Connected => true,
        Sighting::Advertising => cached.is_none_or(|k| k.eq_ignore_ascii_case(&w.key)),
    }
}

/// The sighting a central event stands for, if any. `DeviceDiscovered` and
/// `ServicesAdvertisement` are left out: BlueZ synthesizes both for every
/// known device when the stream opens, and a bonded device that comes back
/// gets RSSI / data updates rather than a fresh discovery.
fn sighting(ev: &CentralEvent) -> Option<(&PeripheralId, Sighting)> {
    match ev {
        CentralEvent::DeviceConnected(id) => Some((id, Sighting::Connected)),
        CentralEvent::DeviceUpdated(id)
        | CentralEvent::RssiUpdate { id, .. }
        | CentralEvent::ManufacturerDataAdvertisement { id, .. }
        | CentralEvent::ServiceDataAdvertisement { id, .. } => Some((id, Sighting::Advertising)),
        _ => None,
    }
}

/// Advertising sightings per device are reported at most this often (a scan
/// delivers RSSI updates many times a second).
const ADVERT_DEBOUNCE: Duration = Duration::from_secs(2);
/// Wait before reopening the event stream after the adapter went away.
const REOPEN_AFTER: Duration = Duration::from_secs(5);

/// Watch `bt`'s first adapter for sightings of a Clawdmeter. Runs until the
/// receiver is dropped; reopens the stream when it ends, or when `bt`
/// replaces its manager after the D-Bus connection died (a dead connection
/// can leave its stream hanging). Must be called inside a tokio runtime.
pub fn spawn_watcher(bt: Bluetooth) -> Wakeups {
    let (tx, rx) = mpsc::unbounded_channel();
    tokio::spawn(async move {
        loop {
            tokio::select! {
                () = tx.closed() => return,
                () = watch_once(&bt, &tx) => {}
            }
            tokio::select! {
                () = tx.closed() => return,
                () = tokio::time::sleep(REOPEN_AFTER) => {}
            }
        }
    });
    rx
}

async fn watch_once(bt: &Bluetooth, tx: &mpsc::UnboundedSender<Wake>) {
    // Subscribe before the adapter lookup, so a connection lost between
    // the two is counted here.
    let mut losses = bt.connection_losses();
    let opened_at = *losses.borrow_and_update();
    let adapter = match bt.adapter().await {
        Ok(a) => a,
        Err(e) => {
            log::debug!("wakeup watcher: {e}");
            return;
        }
    };
    let mut events = match adapter.events().await {
        Ok(s) => s,
        Err(e) => {
            log::debug!("wakeup watcher: no event stream: {e}");
            return;
        }
    };
    let mut last_advert: HashMap<PeripheralId, Instant> = HashMap::new();
    loop {
        let ev = tokio::select! {
            ev = events.next() => match ev {
                Some(ev) => ev,
                None => {
                    log::debug!("wakeup watcher: event stream ended");
                    break;
                }
            },
            _ = losses.changed() => {
                // A dead connection can leave its event stream hanging
                // rather than ending it: the match channels stay open
                // while the session is alive. Reopen on the replacement.
                log::debug!("wakeup watcher: connection replaced; reopening");
                break;
            }
        };
        let Some((id, kind)) = sighting(&ev) else { continue };
        if kind == Sighting::Advertising {
            let now = Instant::now();
            if last_advert.get(id).is_some_and(|t| now.duration_since(*t) < ADVERT_DEBOUNCE) {
                continue;
            }
            last_advert.insert(id.clone(), now);
        }
        let Ok(p) = adapter.peripheral(id).await else { continue };
        let c = describe(p).await;
        if !accept(c.name.as_deref(), c.has_service) {
            continue;
        }
        log::debug!("wakeup: {} {:?}", c.key, kind);
        if tx.send(Wake { key: c.key, sighting: kind }).is_err() {
            break;
        }
    }
    // bluez-async's event-stream drop unwraps a `RemoveMatch` call on the
    // stream's connection; on a dead one that panics, and release builds
    // abort (panic = "abort"). A counted loss says dead at once; else one
    // lookup proves the connection alive — or replaces a dead manager on
    // the way, counting the loss. Anything but a clear "alive" means
    // leak: a forgotten stream is heap only, no socket. The count is read
    // after the lookup: one that succeeds on a manager someone else just
    // put in place of this stream's dead one has bumped it by then.
    if bt.adapter().await.is_err() || must_leak(opened_at, *losses.borrow()) {
        std::mem::forget(events);
    }
}

/// Must a stream opened when `opened_at` connection losses were counted be
/// leaked rather than dropped? Once `Bluetooth` counts its connection lost
/// the stream is dead weight, and dropping it runs `RemoveMatch` on that
/// dead connection (see `watch_once`).
fn must_leak(opened_at: u64, now: u64) -> bool {
    now != opened_at
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wake(key: &str, sighting: Sighting) -> Wake {
        Wake { key: key.into(), sighting }
    }

    #[test]
    fn connected_always_wakes() {
        assert!(should_wake(&wake("AA:BB:CC:DD:EE:FF", Sighting::Connected), None));
        assert!(should_wake(&wake("AA:BB:CC:DD:EE:FF", Sighting::Connected), Some("11:22:33:44:55:66")));
    }

    #[test]
    fn advertising_wakes_only_for_the_cached_device() {
        let w = wake("AA:BB:CC:DD:EE:FF", Sighting::Advertising);
        assert!(should_wake(&w, None));
        assert!(should_wake(&w, Some("aa:bb:cc:dd:ee:ff")));
        assert!(!should_wake(&w, Some("11:22:33:44:55:66")));
    }

    #[test]
    fn only_streams_of_a_lost_connection_are_leaked() {
        assert!(!must_leak(2, 2));
        assert!(must_leak(2, 3));
        // A connection lost twice while the stream was open.
        assert!(must_leak(2, 4));
    }
}
