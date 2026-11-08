//! [`Bluetooth`] holds one D-Bus system-bus connection however often an
//! adapter is asked for, including when every attempt fails (no BlueZ, no
//! adapter, adapter off): the reconnect loop asks every few seconds while no
//! device is reachable. Its own test binary, so no other test opens sockets
//! meanwhile. Skipped without a system bus.
#![cfg(target_os = "linux")]

use clawdmeter_ble::Bluetooth;

fn sockets() -> usize {
    std::fs::read_dir("/proc/self/fd")
        .unwrap()
        .filter_map(|e| std::fs::read_link(e.ok()?.path()).ok())
        .filter(|t| t.to_string_lossy().starts_with("socket:"))
        .count()
}

#[tokio::test]
async fn one_system_bus_connection_across_attempts() {
    let bt = Bluetooth::default();
    let before = sockets();
    let _ = bt.adapter().await;
    let first = sockets();
    if first == before {
        eprintln!("skipped: no D-Bus system bus");
        return;
    }
    for _ in 0..5 {
        let _ = bt.adapter().await;
        let _ = bt.clone().adapter().await;
    }
    assert_eq!(sockets(), first, "each attempt opened another system-bus connection");
}
