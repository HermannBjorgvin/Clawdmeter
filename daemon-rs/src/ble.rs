// BLE-central transport to the "Clawdmeter" ESP32 peripheral. Built directly
// on the pattern verified in daemon-rs-spike/ against real hardware: find the
// peripheral via connected_peripherals() (NOT scanning — see the ble-transport
// spec's "Connect only to an already system-paired peripheral" requirement
// and openspec/changes/multi-provider-rust-daemon/design.md Decision 2 for
// why scanning can never see this peripheral on macOS).

use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter, WriteType};
use btleplug::platform::{Manager, Peripheral};
use futures::StreamExt;
use uuid::Uuid;

const SERVICE_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000001);
const RX_CHAR_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000002);
const REQ_CHAR_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000004);

pub struct BleTransport {
    manager: Manager,
}

impl BleTransport {
    pub async fn new() -> Result<Self, Box<dyn std::error::Error>> {
        Ok(Self { manager: Manager::new().await? })
    }

    /// Find the already-OS-connected Clawdmeter peripheral, if any is
    /// currently connected. Returns None (not an error) if the OS hasn't
    /// connected to one — that's a normal, retriable state per the
    /// "No system-level connection means no daemon action" scenario.
    pub async fn find_peripheral(&self) -> Option<Peripheral> {
        let adapters = match self.manager.adapters().await {
            Ok(a) => a,
            Err(e) => {
                eprintln!("ble: failed to list adapters: {e}");
                return None;
            }
        };
        let adapter = adapters.into_iter().next()?;

        if let Err(e) = adapter.connected_peripherals(ScanFilter { services: vec![SERVICE_UUID] }).await {
            eprintln!("ble: connected_peripherals query failed: {e}");
            return None;
        }
        // The connected_peripherals call is async-dispatched internally on
        // macOS (see daemon-rs-spike/); give the CoreBluetooth round-trip
        // time before reading the populated cache — 500ms wasn't reliably
        // enough in testing, 2s matches the value verified against real
        // hardware in the spike.
        tokio::time::sleep(std::time::Duration::from_secs(2)).await;

        let peripherals = adapter.peripherals().await.ok()?;
        for p in peripherals {
            if let Ok(Some(props)) = p.properties().await {
                if props.local_name.as_deref() == Some("Clawdmeter") {
                    return Some(p);
                }
            }
        }
        // Not found is a normal, retriable state (OS not currently connected
        // to any Clawdmeter peripheral) — see the "No system-level
        // connection means no daemon action" ble-transport spec scenario.
        None
    }

    /// Write a usage payload (already-serialized JSON line) to the RX
    /// characteristic. Connects fresh each call rather than holding a
    /// long-lived connection open — matches the current Python daemon's
    /// per-poll-cycle connect/write/disconnect pattern, which is simple and
    /// tolerates the peripheral coming and going between polls.
    pub async fn send_payload(&self, payload: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
        let peripheral = self.find_peripheral().await.ok_or("Clawdmeter not currently connected")?;

        peripheral.connect().await?;
        peripheral.discover_services().await?;

        let chars = peripheral.characteristics();
        let rx = chars.iter().find(|c| c.uuid == RX_CHAR_UUID).ok_or("RX characteristic not found")?;
        peripheral.write(rx, payload, WriteType::WithoutResponse).await?;

        peripheral.disconnect().await?;
        Ok(())
    }

    /// True if the device is signaling (via the REQ characteristic) that it
    /// hasn't received data since boot and wants an immediate poll, rather
    /// than waiting for the next scheduled cycle.
    ///
    /// REQ is NOTIFY-only (see firmware/src/ble.cpp — no READ property), and
    /// the firmware fires it synchronously from `onSubscribe` (not on a
    /// timer), specifically so the notification isn't dropped before the
    /// central's CCCD write completes. So this subscribes fresh and waits a
    /// short window for that immediate notification, rather than reading a
    /// value or waiting indefinitely.
    pub async fn refresh_requested(&self) -> bool {
        let Some(peripheral) = self.find_peripheral().await else { return false };
        if peripheral.connect().await.is_err() {
            return false;
        }
        if peripheral.discover_services().await.is_err() {
            let _ = peripheral.disconnect().await;
            return false;
        }

        let chars = peripheral.characteristics();
        let Some(req) = chars.iter().find(|c| c.uuid == REQ_CHAR_UUID).cloned() else {
            let _ = peripheral.disconnect().await;
            return false;
        };

        let mut notifications = match peripheral.notifications().await {
            Ok(stream) => stream,
            Err(_) => {
                let _ = peripheral.disconnect().await;
                return false;
            }
        };

        let requested = if peripheral.subscribe(&req).await.is_ok() {
            tokio::time::timeout(std::time::Duration::from_secs(2), async {
                while let Some(notif) = notifications.next().await {
                    if notif.uuid == REQ_CHAR_UUID {
                        return notif.value.first().copied() == Some(0x01);
                    }
                }
                false
            })
            .await
            .unwrap_or(false)
        } else {
            false
        };

        let _ = peripheral.disconnect().await;
        requested
    }
}
