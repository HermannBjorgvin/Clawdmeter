// BLE-in-Bun spike (openspec task 1.1): scan for and connect to the real
// "Clawdmeter" peripheral, write one byte to the RX characteristic, and
// confirm the connection/write path works at all before deciding whether
// this can live inside a `bun build --compile` single-file executable.

// @ts-ignore - noble ships no first-party types for this fork
import noble from "@stoprocent/noble";

const SERVICE_UUID = "4c41555a44657669636500000000001";
const RX_CHAR_UUID = "4c41555a44657669636500000000002";

function log(msg: string) {
    console.log(`[${new Date().toISOString()}] ${msg}`);
}

noble.on("stateChange", async (state: string) => {
    log(`Adapter state: ${state}`);
    if (state === "poweredOn") {
        log("Scanning for Clawdmeter...");
        await noble.startScanningAsync([], false);
    }
});

let sawAny = false;
noble.on("discover", async (peripheral: any) => {
    sawAny = true;
    const name = peripheral.advertisement?.localName;
    log(`Discovered: name=${JSON.stringify(name)} address=${peripheral.address} rssi=${peripheral.rssi}`);
    if (name !== "Clawdmeter") return;

    log(`Found Clawdmeter (${peripheral.address}), stopping scan and connecting...`);
    await noble.stopScanningAsync();

    try {
        await peripheral.connectAsync();
        log("Connected. Discovering services/characteristics...");

        const { characteristics } = await peripheral.discoverSomeServicesAndCharacteristicsAsync(
            [SERVICE_UUID],
            [RX_CHAR_UUID]
        );

        const rx = characteristics.find((c: any) => c.uuid === RX_CHAR_UUID);
        if (!rx) {
            log("RX characteristic not found — service/characteristic discovery failed");
            process.exit(1);
        }

        const payload = Buffer.from(JSON.stringify({ s: 1, sr: 1, w: 1, wr: 1, st: "allowed", ok: true }) + "\n");
        await rx.writeAsync(payload, false);
        log(`Wrote ${payload.length} bytes to RX characteristic successfully.`);

        await peripheral.disconnectAsync();
        log("Disconnected cleanly. SPIKE PASSED.");
        process.exit(0);
    } catch (err) {
        log(`SPIKE FAILED: ${err}`);
        process.exit(1);
    }
});

setTimeout(() => {
    if (!sawAny) {
        log("SPIKE FAILED: scan saw ZERO peripherals of any kind in 20s — this points to a permission/binding problem, not a Clawdmeter-specific issue");
    } else {
        log("SPIKE FAILED: scan saw other peripherals but never Clawdmeter in 20s");
    }
    process.exit(1);
}, 20000);
