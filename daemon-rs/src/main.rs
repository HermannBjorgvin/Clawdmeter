mod ble;
mod config;
mod providers;

use config::Config;
use providers::NormalizedUsage;
use std::collections::HashMap;
use std::time::Duration;

const POLL_INTERVAL: Duration = Duration::from_secs(60);
const TICK: Duration = Duration::from_secs(5);

/// Which provider+credential's usage is currently "active" (shown on the
/// display) across polls. Ported from `PlanSelector` in
/// daemon/claude_usage_daemon.py: the candidate whose session % most
/// recently rose wins (recent API activity); ties and the startup case
/// break by highest current session %.
#[derive(Default)]
struct PlanSelector {
    prev_s: HashMap<String, i32>,
    last_active: HashMap<String, i32>,
    seq: i32,
}

impl PlanSelector {
    fn choose(&mut self, candidates: &HashMap<String, NormalizedUsage>) -> Option<String> {
        self.seq += 1;
        for (key, usage) in candidates {
            let s = usage.s.round() as i32;
            if let Some(&prev) = self.prev_s.get(key) {
                if s > prev {
                    self.last_active.insert(key.clone(), self.seq);
                }
            }
            self.prev_s.insert(key.clone(), s);
        }
        candidates
            .keys()
            .max_by_key(|key| {
                let recency = *self.last_active.get(*key).unwrap_or(&0);
                let pct = candidates[*key].s.round() as i32;
                (recency, pct)
            })
            .cloned()
    }
}

#[tokio::main]
async fn main() {
    println!("Clawdmeter daemon (Rust) starting...");

    let cfg = Config::load();
    let registry = match providers::build_registry(&cfg) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("config error: {e}");
            std::process::exit(1);
        }
    };
    if registry.is_empty() {
        eprintln!("no providers enabled — nothing to poll");
        std::process::exit(1);
    }

    let ble = match ble::BleTransport::new().await {
        Ok(b) => b,
        Err(e) => {
            eprintln!("failed to initialize BLE: {e}");
            std::process::exit(1);
        }
    };

    let mut selector = PlanSelector::default();
    let mut last_poll = std::time::Instant::now() - POLL_INTERVAL; // poll immediately on first loop

    loop {
        let due = last_poll.elapsed() >= POLL_INTERVAL;
        // Only probe for a device-initiated refresh when we're not already
        // about to poll this tick — doing both back-to-back was found (by
        // testing against real hardware) to race two BLE connect cycles
        // against each other and fail. The refresh signal only matters right
        // after the device boots with no data yet (see ble.rs), so checking
        // it exclusively on off-cycle ticks loses no real responsiveness.
        let refresh = !due && ble.refresh_requested().await;

        if due || refresh {
            last_poll = std::time::Instant::now();
            if refresh {
                println!("device requested refresh — polling immediately");
            }

            let mut candidates: HashMap<String, NormalizedUsage> = HashMap::new();
            for provider in &registry {
                for cred in provider.find_credentials().await {
                    if let Some(usage) = provider.poll_usage(&cred).await {
                        let key = format!("{}:{}", provider.id(), cred.label);
                        candidates.insert(key, usage);
                    }
                }
            }

            if candidates.is_empty() {
                println!("no usable data this cycle");
            } else if let Some(active_key) = selector.choose(&candidates) {
                let mut usage = candidates.remove(&active_key).unwrap();
                usage.c = Some(cfg.chime_enabled());
                match serde_json::to_vec(&usage) {
                    Ok(mut payload) => {
                        payload.push(b'\n');
                        match ble.send_payload(&payload).await {
                            Ok(()) => println!("sent usage from {active_key}: {}", String::from_utf8_lossy(&payload)),
                            Err(e) => eprintln!("BLE write failed: {e}"),
                        }
                    }
                    Err(e) => eprintln!("failed to serialize payload: {e}"),
                }
            }
        }

        tokio::time::sleep(TICK).await;
    }
}
