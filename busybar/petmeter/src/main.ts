import manifest from "./appmeta/manifest.json";

/**
 * WHY THERE IS NO LAYOUT LIBRARY HERE.
 *
 * `@busy-app/busy-lib` has row/column/render, and they are nicer than
 * arithmetic. They also ship font metric tables -- thousands of numbers built
 * at module load -- and this runtime cannot afford them. Its heap is small
 * enough that 40,000 array pushes kills it, and **an out-of-memory abort is
 * completely silent**: the script's first line prints, the last never does,
 * and nothing is logged. That cost an hour of looking for a syntax error that
 * was not there. Positions here are absolute, which a 72x16 screen wants
 * anyway.
 */
type Element = Record<string, unknown>;

const APP = manifest.id;

/**
 * Where the usage comes from. The Petmeter daemon serves its latest poll at
 * /usage.json. Over USB both addresses are fixed -- the bar is 10.0.4.20 and
 * the host 10.0.4.21 -- so there is nothing to discover and no Wi-Fi involved.
 */
const HOST = import.meta.env.VITE_PETMETER_HOST ?? "http://10.0.4.21:8724";
const SELF = "http://10.0.4.20";

const POLL_MS = 30_000;
const CARD_MS = 4_000;
const TICK_MS = 250;

const PET = 16;
const NUM_X = 18;       // the big number, right of the mascot
const COL_X = 45;       // the label-over-reset column
const COL_W = 27;       // clipped, so a long label cannot overflow
const BAR_X = 18;
const BAR_Y = 12;
const BAR_W = 54;
const BAR_H = 3;
const MAX_CELLS = 8;

// Thresholds and colours from the firmware's pct_color(), so the bar and the
// meter on the desk never disagree about whether a number is alarming. The
// number itself stays white: the firmware colours bar indicators, never text.
const WARN_PCT = 75;
const CRIT_PCT = 90;
const COL_OK = "#8FA76BFF";
const COL_WARN = "#D97757FF";
const COL_CRIT = "#C0392BFF";
const COL_TEXT = "#FAF9F5FF";
const COL_DIM = "#B0AEA5FF";
const COL_TRACK = "#2A2A28FF";

type Card = {
  provider: string;
  label: string;
  pct?: number;
  held?: number;
  used?: number;
  /** Seconds left as of the poll; aged locally, never from the bar's clock. */
  in_s?: number;
};

const PET_IMAGE: Record<string, string> = {
  // Image paths resolve against the APP ROOT, not the assets folder -- and one
  // unreadable image rejects the whole draw with a 400, so a wrong path here
  // means no frame at all rather than a frame with a gap in it.
  claude: "appmeta/assets/clawd_16.png",
  codex: "appmeta/assets/codey_16.png",
};

/**
 * THE FIXED ID SET, AND WHY IT IS FIXED.
 *
 * The canvas merges draws by id: an element missing from the next draw stays
 * on screen. Left alone, the credit card's cells would still be sitting under
 * the next card's bar. So every frame names every id, and the ones it does
 * not use go as tombstones -- a `display_until` already in the past, which
 * the device destroys on sight.
 *
 * An id must also keep its **type** across draws or the entire batch 400s,
 * which is why `reset` is always text and never a countdown.
 */
const IDS: Array<[string, "text" | "rectangle"]> = [
  ["num", "text"],
  ["label", "text"],
  ["reset", "text"],
  ["msg", "text"],
  ["track", "rectangle"],
  ["fill", "rectangle"],
  ["paused", "rectangle"],
];
for (let i = 0; i < MAX_CELLS; i++) IDS.push([`cell${i}`, "rectangle"]);

function tombstone(id: string, type: "text" | "rectangle"): Element {
  const base = { id, type, x: 0, y: 0, display: "front", display_until: "1" };
  return type === "text"
    ? { ...base, text: " ", font: "small", color: COL_DIM, align: "top_left" }
    : { ...base, width: 1, height: 1, fill: "solid",
        fill_colors: [COL_TRACK], border_width: 0 };
}

/** Fills in whatever the frame left out, so nothing lingers from the last card. */
function complete(used: Element[]): Element[] {
  const seen: Record<string, boolean> = {};
  for (const el of used) seen[el.id as string] = true;
  const out = used.slice();
  for (const pair of IDS) if (!seen[pair[0]]) out.push(tombstone(pair[0], pair[1]));
  return out;
}

function colorFor(pct: number): string {
  if (pct >= CRIT_PCT) return COL_CRIT;
  if (pct >= WARN_PCT) return COL_WARN;
  return COL_OK;
}

function text(
  id: string,
  value: string,
  font: "small" | "normal" | "large",
  x: number,
  y: number,
  color: string,
  width?: number,
): Element {
  const el: Element = {
    id, type: "text", text: value, font, x, y,
    align: "top_left", color, display: "front", timeout: 0,
  };
  if (width !== undefined) el.width = width;
  return el;
}

/** A rectangle has no `color`: it has a fill and a border, and the border
 *  defaults to 1px white. Pass only a colour and you get an outline. */
function rect(
  id: string, x: number, y: number, w: number, h: number,
  color: string | null, border?: string,
): Element {
  return {
    id, type: "rectangle", x, y, width: w, height: h, radius: 0,
    fill: color ? "solid" : "none",
    fill_colors: [color ?? COL_TRACK],
    border_width: border ? 1 : 0,
    border_color: border ?? COL_DIM,
    display: "front", timeout: 0,
  };
}

/**
 * "1h25m", "5d21h", "10d" -- no spaces, because "23h 59m" is 28px and would
 * clip the 27px column. Deliberately not the device's `countdown` element:
 * that renders HH:MM:SS in a wide font, ticks every 100ms, and takes hours
 * modulo 60, so a five-day reset would show as 21 hours.
 */
function until(seconds: number): string {
  if (seconds <= 0) return "now";
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d >= 1) return `${d}d${h}h`;
  if (h >= 1) return `${h}h${m}m`;
  return `${m}m`;
}

function petOf(card: Card): Element {
  return {
    id: "pet", type: "image",
    path: PET_IMAGE[card.provider] ?? PET_IMAGE.claude,
    x: 0, y: 0, display: "front", timeout: 0,
  };
}

/**
 * A quota. Big number left of the pane, label over reset time in a fixed
 * column, bar across the bottom. The column sits at a fixed x rather than
 * following the number's width, or the label would jump between cards.
 *
 * Text `y` is the top of the line box and goes negative so the caps land on
 * the intended rows: the number's ink on 1..9, the column's on 0..4 and 6..10.
 */
function quota(card: Card, left: number | null): Element[] {
  const pct = Math.round(card.pct ?? 0);
  // At 100 the "%" no longer fits beside three digits, and shrinking the font
  // at the moment the number matters most is the wrong trade.
  const shown = pct >= 100 ? "100" : `${pct}%`;
  const filled =
    pct > 0 ? Math.max(1, Math.round((BAR_W * Math.min(pct, 100)) / 100)) : 0;

  const out: Element[] = [
    petOf(card),
    text("num", shown, "large", NUM_X, -1, COL_TEXT),
    text("label", card.label, "small", COL_X, -2, COL_DIM, COL_W),
    rect("track", BAR_X, BAR_Y, BAR_W, BAR_H, COL_TRACK),
  ];
  if (left !== null) {
    out.push(text("reset", until(left), "small", COL_X, 4, COL_DIM, COL_W));
  }
  if (filled > 0) out.push(rect("fill", BAR_X, BAR_Y, filled, BAR_H, colorFor(pct)));
  return out;
}

/**
 * Reset credits are a count, not a proportion, so the bar row becomes the desk
 * device's ledger: one cell per credit the window handed out, solid while held
 * and hollow once spent. Same grammar, different shape.
 */
function credits(card: Card, left: number | null): Element[] {
  const held = card.held ?? 0;
  const total = held + (card.used ?? 0);
  const out: Element[] = [
    petOf(card),
    text("num", `${held}/${total}`, "large", NUM_X, -1, COL_TEXT),
    text("label", card.label, "small", COL_X, -2, COL_DIM, COL_W),
    text(
      "reset",
      held === 0 ? "all used" : left !== null ? until(left) : " ",
      "small", COL_X, 4, COL_DIM, COL_W,
    ),
  ];

  const n = Math.min(total, MAX_CELLS);
  if (n > 0) {
    const cellW = Math.floor((BAR_W - 2 * (n - 1)) / n);
    for (let i = 0; i < n; i++) {
      const x = BAR_X + i * (cellW + 2);
      out.push(
        i < held
          ? rect(`cell${i}`, x, BAR_Y, cellW, BAR_H, COL_OK)
          : rect(`cell${i}`, x, BAR_Y, cellW, BAR_H, null, COL_DIM),
      );
    }
  }
  return out;
}

function frame(card: Card, left: number | null, paused: boolean): Element[] {
  const body = card.pct === undefined ? credits(card, left) : quota(card, left);
  // The badge sits in the pet box's top-right corner, clear of ink on both
  // mascots, so it never collides with the label the way a screen-corner dot
  // does.
  if (paused) body.push(rect("paused", PET - 2, 0, 2, 2, COL_DIM));
  return complete(body);
}

/** The pet says "app alive, host present, reading missing". Without a host
 *  there is nothing of ours to show, so that state is text alone. */
function message(value: string, withPet: Card | null): Element[] {
  const out: Element[] = withPet ? [petOf(withPet)] : [];
  out.push(text("msg", value, "normal", withPet ? 27 : 18, 3, COL_DIM));
  return complete(out);
}

/**
 * Wipe whatever a previous version of this app left on screen.
 *
 * Draws merge by id, and ids this build never names can never be overwritten
 * -- an older layout's elements simply stay there forever, which is exactly
 * what a stale "27%" sitting under the new label column turned out to be.
 * Once, at startup: clearing between cards would close the canvas and let the
 * bar's own UI flash through.
 */
async function clearCanvas(): Promise<void> {
  await fetch(
    new Request(`${SELF}/api/display/draw?application_name=${APP}`, {
      method: "DELETE",
    }),
  );
}

let lastError = "";

async function draw(elements: Element[]): Promise<void> {
  const resp = await fetch(
    new Request(`${SELF}/api/display/draw`, {
      method: "POST",
      body: JSON.stringify({ application_name: APP, priority: 50, elements }),
    }),
  );
  // A rejected draw used to be silent, which is what made a bad frame look
  // like a dead app. 409 is a focus session owning the screen, not a fault.
  if (resp.status !== 200 && resp.status !== 409) {
    const body = await resp.text();
    if (body !== lastError) {
      lastError = body;
      console.error(`${APP}: draw ${resp.status} ${body}`);
    }
  }
}

/** Entry point. The build rewrites the default export into a call. */
export default function run(): void {
  let cards: Card[] = [];
  let index = 0;
  let paused = false;
  let dwell = 0;
  let polledAt = 0;

  const report = (err: unknown) =>
    console.error(`${APP}: ${err instanceof Error ? err.message : String(err)}`);

  /** Seconds left on this card, aged by our own elapsed time since the poll,
   *  so the countdown never depends on the bar's clock being right. */
  function remaining(card: Card): number | null {
    if (typeof card.in_s !== "number") return null;
    return Math.max(0, card.in_s - Math.round((Date.now() - polledAt) / 1000));
  }

  function show(): void {
    if (cards.length === 0) return;
    index = ((index % cards.length) + cards.length) % cards.length;
    const card = cards[index];
    void draw(frame(card, remaining(card), paused)).catch(report);
  }

  function step(by: number): void {
    index += by;
    dwell = 0;
    show();
  }

  async function poll(): Promise<void> {
    try {
      const resp = await fetch(`${HOST}/usage.json`);
      const data = await resp.json();
      polledAt = Date.now();
      cards = Array.isArray(data.cards) ? data.cards : [];
      if (cards.length === 0) await draw(message("no data", null));
    } catch (err) {
      // Host asleep, unplugged, or the daemon stopped. Say which rather than
      // leaving the last good frame up, which would quietly go stale.
      report(err);
      cards = [];
      await draw(message("no host", null)).catch(report);
    }
  }

  // Controls follow what the case itself is engraved with: the red bar is
  // Start/Pause, the wheel is Scroll, and the wheel's press says "OK/Skip".
  // A manual step restarts the dwell, or the card you just asked for would
  // vanish a moment later.
  // THE CONTROLS ONLY EXIST FOR A PROPERLY LAUNCHED APP.
  //
  // `listen` is installed by js_setup_input_methods(), and a script started
  // from the CLI never gets it -- that context's globals are exactly console,
  // setInterval, setTimeout, clearInterval, clearTimeout, Request, fetch and
  // localStorage. So the handler below is dead code until the app can be
  // launched as an app, which is the same thing the hardcoded apps menu
  // blocks. It is written and guarded rather than deleted: the binding is
  // correct, only the launch path is missing.
  //
  // Guarded because an uncaught ReferenceError here takes the whole app down
  // silently -- for a long time that looked like a dead draw loop and was a
  // dead control binding. Losing the buttons is survivable; losing the
  // display is not.
  if (typeof listen !== "function") {
    console.info(`${APP}: no input API in this context; controls disabled`);
  } else try {
    listen("input", (event: InputEvent) => {
      if (event.action === "release") return;
      if (event.key === "start") {
        paused = !paused;
        dwell = 0;
        show();
      } else if (event.key === "encoder") {
        step(event.delta ?? (event.action === "clockwise" ? 1 : -1));
      } else if (event.key === "ok") {
        step(1);
      }
    });
  } catch (err) {
    console.error(`${APP}: controls unavailable: ${String(err)}`);
  }

  void clearCanvas()
    .catch(report)
    .then(poll)
    .then(show)
    .catch(report);
  setInterval(() => void poll().catch(report), POLL_MS);
  setInterval(() => {
    if (paused || cards.length === 0) return;
    dwell += TICK_MS;
    if (dwell >= CARD_MS) step(1);
  }, TICK_MS);
}

/**
 * The runtime's input global, installed on the realm by js_input.c -- not an
 * import, so TypeScript has to be told it exists.
 */
declare function listen(
  type: "input",
  handler: (event: InputEvent) => void,
): () => void;

type InputEvent = {
  /** Which control: the top bar is `start`, the wheel is `encoder`. */
  key: "encoder" | "start" | "ok" | "back";
  action: "press" | "release" | "clockwise" | "counterclockwise";
  /** Encoder only: +1 clockwise, -1 counterclockwise. */
  delta?: number;
};
