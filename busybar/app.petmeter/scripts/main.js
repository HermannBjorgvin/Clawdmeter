// Petmeter — an on-device BUSY Bar app, picked from the apps menu.
//
// The sink in daemon/sinks/busybar.py pushes: it draws whenever the daemon
// polls, whether or not you are looking. This pulls: you select it from the
// menu and it fetches the current numbers itself, so the screen is right the
// moment you open it rather than whenever the host last got round to it.
//
// WHERE THE DATA COMES FROM. The daemon serves its latest payload at
// /usage.json. Over USB the two addresses are fixed -- the bar is 10.0.4.20
// and the host is 10.0.4.21 -- so there is nothing to discover and no Wi-Fi
// involved. Set HOST below if you serve it from somewhere else.
//
// This runs on the bar, in its JS runtime, and talks to the bar's own HTTP API
// through the same fetch() it uses to reach the host.

const HOST = "http://10.0.4.21:8724";
const SELF = "http://10.0.4.20";
const APP_ID = "app.petmeter";

const REFRESH_MS = 30000;      // the host polls every 60s; half that never
                               // shows a number more than one poll stale
const WIDTH = 72;
const BAR_Y = 13;
const BAR_H = 3;

// Thresholds and colours from the firmware's pct_color(), so the bar and the
// meter on the desk never disagree about whether a number is alarming.
const WARN_PCT = 75;
const CRIT_PCT = 90;
const COL_OK = "#8FA76BFF";
const COL_WARN = "#D97757FF";
const COL_CRIT = "#C0392BFF";
const COL_TEXT = "#FAF9F5FF";
const COL_DIM = "#B0AEA5FF";
const COL_TRACK = "#2A2A28FF";

function colorFor(pct) {
    if (pct >= CRIT_PCT) return COL_CRIT;
    if (pct >= WARN_PCT) return COL_WARN;
    return COL_OK;
}

function draw(elements, led) {
    const body = { application_name: APP_ID, priority: 50, elements: elements };
    if (led) body.led_notification_color = led;
    return fetch(new Request(SELF + "/api/display/draw", {
        method: "POST",
        body: JSON.stringify(body),
    }));
}

function message(text) {
    return draw([{
        id: "msg", type: "text", text: text, font: "small",
        x: WIDTH / 2, y: 8, align: "center", color: COL_DIM,
        display: "front", timeout: 0,
    }]);
}

function render(usage) {
    const pct = Math.round(usage.pct);
    const elements = [{
        id: "pct", type: "text", text: pct + "%", font: "normal",
        x: 0, y: 2, align: "top_left", color: COL_TEXT,
        display: "front", timeout: 0,
    }];

    // The device counts this down itself, so it stays true between refreshes.
    if (typeof usage.resets_at === "number" && usage.resets_at > 0) {
        elements.push({
            id: "reset", type: "countdown", timestamp: String(usage.resets_at),
            direction: "time_left", show_hours: "when_non_zero",
            x: 34, y: 2, align: "top_left", color: COL_DIM,
            display: "front", timeout: 0,
        });
    }

    // A rectangle has no `color`: it has a fill (default none) and a border
    // (default 1px white). Pass only a colour and you get a white outline.
    elements.push({
        id: "track", type: "rectangle", x: 0, y: BAR_Y,
        width: WIDTH, height: BAR_H, radius: 0,
        fill: "solid", fill_colors: [COL_TRACK], border_width: 0,
        display: "front", timeout: 0,
    });
    const filled = pct > 0 ? Math.max(1, Math.round(WIDTH * Math.min(pct, 100) / 100)) : 0;
    if (filled > 0) {
        elements.push({
            id: "fill", type: "rectangle", x: 0, y: BAR_Y,
            width: filled, height: BAR_H, radius: 0,
            fill: "solid", fill_colors: [colorFor(pct)], border_width: 0,
            display: "front", timeout: 0,
        });
    }
    return draw(elements, pct >= CRIT_PCT ? COL_CRIT : null);
}

async function tick() {
    try {
        const resp = await fetch(HOST + "/usage.json");
        if (!resp.ok) {
            // The daemon is up but has nothing usable -- a dead token, say.
            await message("no data");
            return;
        }
        const usage = await resp.json();
        if (!usage.ok) {
            await message("no data");
            return;
        }
        await render(usage);
    } catch (e) {
        // Host asleep, unplugged, or daemon stopped. Say which rather than
        // leaving the last good frame up, which would quietly go stale.
        console.error("petmeter:", e);
        await message("no host");
    }
}

tick();
setInterval(tick, REFRESH_MS);
