// Rust BLE spike: use btleplug's unmerged "connected_peripherals" API
// (github.com/deviceplug/btleplug/pull/437, forked at challiwill/btleplug)
// to retrieve the Clawdmeter peripheral WITHOUT scanning — i.e. exercise
// macOS's retrieveConnectedPeripheralsWithServices path directly, which is
// exactly what noble's macOS binding was missing.

use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter, WriteType};
use btleplug::platform::Manager;
use uuid::Uuid;

const SERVICE_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000001);
const RX_CHAR_UUID: Uuid = Uuid::from_u128(0x4c41555a_4465_7669_6365_000000000002);

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Initializing BLE manager...");
    let manager = Manager::new().await?;
    let adapters = manager.adapters().await?;
    let adapter = adapters.into_iter().next().ok_or("no BLE adapter found")?;
    println!("Using adapter: {:?}", adapter.adapter_info().await);

    println!("Calling connected_peripherals (retrieveConnectedPeripheralsWithServices) for our service UUID...");
    adapter
        .connected_peripherals(ScanFilter {
            services: vec![SERVICE_UUID],
        })
        .await?;

    // Give the async CoreBluetooth round-trip a moment to populate the cache.
    tokio::time::sleep(std::time::Duration::from_secs(2)).await;

    let peripherals = adapter.peripherals().await?;
    println!("connected_peripherals returned {} peripheral(s)", peripherals.len());

    let mut target = None;
    for p in peripherals {
        let props = p.properties().await?;
        let name = props.as_ref().and_then(|pr| pr.local_name.clone());
        println!("  - {:?} name={:?}", p.id(), name);
        if name.as_deref() == Some("Clawdmeter") {
            target = Some(p);
        }
    }

    let peripheral = match target {
        Some(p) => p,
        None => {
            println!("SPIKE FAILED: Clawdmeter not found via connected_peripherals");
            std::process::exit(1);
        }
    };

    println!("Found Clawdmeter. Connecting...");
    peripheral.connect().await?;
    peripheral.discover_services().await?;

    let chars = peripheral.characteristics();
    let rx = chars
        .iter()
        .find(|c| c.uuid == RX_CHAR_UUID)
        .ok_or("RX characteristic not found")?;

    let payload = br#"{"s":1,"sr":1,"w":1,"wr":1,"st":"allowed","ok":true}
"#;
    peripheral.write(rx, payload, WriteType::WithoutResponse).await?;
    println!("Wrote {} bytes to RX characteristic successfully.", payload.len());

    peripheral.disconnect().await?;
    println!("Disconnected cleanly. SPIKE PASSED.");
    Ok(())
}
