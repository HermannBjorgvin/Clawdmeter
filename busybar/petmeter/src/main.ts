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

// The host holds the request open until something changes, so a press
// reaches the screen in one round trip. A return with nothing changed is not
// a failure: redrawing every WAIT_MS also restores the frame after anything
// else clears the canvas.
const WAIT_MS = 8_000;
const RETRY_MS = 1_000;
const RETRY_MAX_MS = 10_000;

// Clawd is authored on a 12x8 grid, so he renders at exactly 2x -- 24x16,
// filling the height. Codey is 16 wide and gets centred in the same slot;
// both stand 16 tall, which is what reads as "the same size".
const PET_X: Record<string, number> = { claude: 0, codex: 4 };
const NUM_X = 26;       // the pane: everything right of the mascot
const CELL_Y = 4;       // credit cells sit on the number's row
const CELL_H = 5;
// The caption row. At y=9 the baseline lands on row 16 -- one below the last
// pixel row -- so "Weekly" lost the tail of its y. Up one: baseline 15, caps
// on 10..14, and a row left for descenders.
const CAPTION_Y = 8;
const MAX_CELLS = 8;

// Measured against the device's own font files, not guessed: large digits and
// "/" are 7px, "%" is 10px, small advances ~4px.
const LARGE_DIGIT = 7;
const LARGE_PCT = 10;
const SMALL_ADV = 4;
const SMALL_WIDE = 6;   // "m" and "w"
const NORMAL_ADV = 6;
const WIDTH = 72;

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
  /** Which window this is, which decides the countdown's precision. */
  kind?: string;
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
const IDS: Array<[string, "text" | "rectangle" | "image"]> = [
  // The mascot is tombstoned like everything else. Leaving it out is how the
  // "no host" screen ended up with the previous card's Clawd still under the
  // words: an id this frame does not name is an id the canvas keeps.
  ["pet", "image"],
  ["num", "text"],
  ["label", "text"],
  ["reset", "text"],
  ["msg", "text"],
  ["track", "rectangle"],
  ["fill", "rectangle"],
  ["paused", "rectangle"],
  ["tmask", "rectangle"],
  ["ttext", "text"],
];
for (let i = 0; i < MAX_CELLS; i++) IDS.push([`cell${i}`, "rectangle"]);

function tombstone(id: string, type: "text" | "rectangle" | "image"): Element {
  const base = { id, type, x: 0, y: 0, display: "front", display_until: "1" };
  if (type === "text") {
    return { ...base, text: " ", font: "small", color: COL_DIM,
             align: "top_left" };
  }
  if (type === "image") {
    // Must name a real asset: an unreadable path rejects the whole batch,
    // tombstone or not.
    return { ...base, path: PET_IMAGE.claude };
  }
  return { ...base, width: 1, height: 1, fill: "solid",
           fill_colors: [COL_TRACK], border_width: 0 };
}

/** Fills in whatever the frame left out, so nothing lingers from the last card. */
function complete(used: Element[]): Element[] {
  const seen: Record<string, boolean> = {};
  for (const el of used) seen[el.id as string] = true;
  const out = used.slice();
  for (const pair of IDS) {
    if (pair[0] === "tmask" || pair[0] === "ttext") continue;  // transient
    if (!seen[pair[0]]) out.push(tombstone(pair[0], pair[1]));
  }
  return out;
}

function largeWidth(text: string): number {
  let w = 0;
  for (const ch of text) w += ch === "%" ? LARGE_PCT : LARGE_DIGIT;
  return w;
}

// Not every small glyph is 4px: "m" and "w" are wider, and assuming otherwise
// pushed the right-aligned reset past the screen edge -- "4h5m" lost its m.
function smallWidth(text: string): number {
  let w = 0;
  for (const ch of text) w += ch === "m" || ch === "w" ? SMALL_WIDE : SMALL_ADV;
  return w;
}

/** Right edge for a right-aligned string, never left of the pane. */
const rightAlign = (text: string) => Math.max(NUM_X, WIDTH - smallWidth(text));

/**
 * The number carries the alarm now that there is no bar to colour. White
 * below the warn threshold rather than green: with nothing else tinted, a
 * permanent green becomes wallpaper, and colour should mean something. The
 * desk device's numbers are white for the same reason.
 */
function colorFor(pct: number): string {
  if (pct >= CRIT_PCT) return COL_CRIT;
  if (pct >= WARN_PCT) return COL_WARN;
  return COL_TEXT;
}

/**
 * THE TIME AND THE LABEL DO NOT SHARE A ROW.
 *
 * "Current" and "3h48m" want 52px of a 46px one, so something gave: first the
 * time (which produced "3h48", a duration nobody writes and the one number on
 * the card worth acting on), then the label (which produced "Curre"). Both
 * were wrong. The time goes up beside the number instead, where the space is
 * free on every quota card, and the label gets the caption row to itself.
 */
// The caption band starts LEFT of the pane. Only Clawd's arms reach x=23,
// and they occupy rows 4..7; on rows 9..15 both mascots stop at column 19, so
// the band can begin at 22 with the same 2px clearance. Those four pixels are
// what make "Current" and "3h40m" fit on one row without shortening either.
const BAND_X = 22;
const BAND_W = 50;

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
 * PRECISION IS A PROPERTY OF THE WINDOW, NOT OF WHAT FITS.
 *
 * A 5-hour window is worth minutes, so it always shows them: "3h40m". A 7-day
 * one is not -- "Weekly" plus "23h59m" is 52px of a 50px row and always would
 * be -- so those carry days and hours, and hours alone inside the last day.
 * Nothing is ever shortened to make it fit; the unit is chosen once, by what
 * the number means.
 *
 * Never the device's `countdown` element: that renders HH:MM:SS in a wide
 * font, ticks every 100ms, and takes hours modulo 60, so a five-day reset
 * would show as 21 hours.
 */
function until(seconds: number, kind?: string): string {
  if (seconds <= 0) return "now";
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (kind === "session") {
    return h >= 1 ? `${h}h${String(m).padStart(2, "0")}m` : `${m}m`;
  }
  if (d >= 1) return `${d}d${h}h`;
  if (h >= 1) return `${h}h`;
  return `${m}m`;
}

function petOf(card: Card): Element {
  return {
    id: "pet", type: "image",
    path: PET_IMAGE[card.provider] ?? PET_IMAGE.claude,
    x: PET_X[card.provider] ?? 0, y: 0, display: "front", timeout: 0,
  };
}

/**
 * A quota. The number owns the top row at full height; the label and the
 * reset time share the row below, label left and reset right-aligned.
 *
 * THERE IS NO BAR. In a 46px pane a `large` number cannot share a row with
 * any label, and label + 9px number + a bar do not stack in 16 rows, so
 * something had to give. The bar was earning least: its track is invisible
 * on an LED matrix, so it never showed headroom, and its one real job --
 * carrying the alarm -- a 9px number does better than a 2px stripe.
 *
 * Text `y` is the top of the line box and goes negative so the caps land on
 * the intended rows: the number's ink on 0..8, the caption's on 11..15.
 */
function quota(card: Card, left: number | null): Element[] {
  const pct = Math.round(card.pct ?? 0);
  // Three digits and a "%" would reach the countdown above; the sign is the
  // part that can go, since the bar-less card has nothing else to be.
  const shown = pct >= 100 ? "100" : `${pct}%`;
  const reset = left === null ? null : until(left, card.kind);
  const out: Element[] = [
    petOf(card),
    text("num", shown, "large", NUM_X, -2, colorFor(pct)),
    text("label", card.label, "small", BAND_X, CAPTION_Y, COL_DIM, BAND_W),
  ];
  if (reset !== null) {
    out.push(text("reset", reset, "small", rightAlign(reset), CAPTION_Y, COL_DIM));
  }
  return out;
}

/**
 * Reset credits are a count, not a proportion, so the ledger sits beside the
 * number instead of a bar: one cell per credit the window handed out, solid
 * while held and hollow once spent.
 */
function credits(card: Card, left: number | null): Element[] {
  const held = card.held ?? 0;
  const total = held + (card.used ?? 0);
  const shown = `${held}/${total}`;
  const out: Element[] = [
    petOf(card),
    text("num", shown, "large", NUM_X, -2, COL_TEXT),
    text("label", card.label, "small", BAND_X, CAPTION_Y, COL_DIM, BAND_W),
  ];

  const detail = held === 0 ? "spent" : left !== null ? until(left, card.kind) : null;
  if (detail !== null) {
    out.push(text("reset", detail, "small", rightAlign(detail),
                  CAPTION_Y, COL_DIM));
  }

  const n = Math.min(total, MAX_CELLS);
  const x0 = NUM_X + largeWidth(shown) + 3;
  const cellW = n > 0 ? Math.floor((WIDTH - x0 - (n - 1)) / n) : 0;
  // Below 3px a hollow cell has no hole left and reads as a solid one, which
  // would say "held" about a credit that is spent.
  if (cellW >= 3) {
    for (let i = 0; i < n; i++) {
      const x = x0 + i * (cellW + 1);
      out.push(
        i < held
          ? rect(`cell${i}`, x, CELL_Y, cellW, CELL_H, COL_OK)
          : rect(`cell${i}`, x, CELL_Y, cellW, CELL_H, null, COL_DIM),
      );
    }
  }
  return out;
}

/**
 * A two-second word over the caption row, for a press that would otherwise
 * have nothing to show for itself. The device deletes both elements when the
 * timeout expires, so there is no second request and no state to unwind; the
 * mask is what stops the label and reset showing through underneath.
 */
function toast(word: string): Element[] {
  return [
    {
      id: "tmask", type: "rectangle", x: BAND_X, y: CAPTION_Y + 1,
      width: BAND_W, height: 7, radius: 0,
      fill: "solid", fill_colors: ["#000000FF"], border_width: 0,
      display: "front", timeout: 2, z_index: 100,
    },
    {
      id: "ttext", type: "text", text: word, font: "small",
      x: rightAlign(word), y: CAPTION_Y, align: "top_left",
      color: COL_TEXT, display: "front", timeout: 2, z_index: 110,
    },
  ];
}

function frame(card: Card, left: number | null, paused: boolean,
               say?: string): Element[] {
  const body = card.pct === undefined ? credits(card, left) : quota(card, left);
  // The badge sits at the pane's top right, which is empty on every card --
  // the number ends by x=54 at worst and the cells sit two rows below.
  if (paused) body.push(rect("paused", 70, 0, 2, 2, COL_DIM));
  const out = complete(body);
  return say ? out.concat(toast(say)) : out;
}

/**
 * The pet says "app alive, host present, reading missing", so `no data` keeps
 * it and centres the words in the pane beside it. `no host` drops it -- the
 * pet lives on the host -- and centres on the whole screen, which only works
 * because `pet` is tombstoned above.
 */
function message(value: string, withPet: Card | null): Element[] {
  const width = value.length * NORMAL_ADV;
  if (withPet) {
    const x = NUM_X + Math.round((WIDTH - NUM_X - width) / 2);
    return complete([petOf(withPet), text("msg", value, "normal", x, 3, COL_DIM)]);
  }
  return complete([
    text("msg", value, "normal", Math.round((WIDTH - width) / 2), 3, COL_DIM),
  ]);
}

/**
 * Wipe whatever a previous version of this app left on screen.
 *
 * Draws merge by id, and ids this build never names can never be overwritten
 * -- an older layout's bar sat under the new caption row indefinitely. Once,
 * at startup: clearing between cards closes the canvas and the bar's own UI
 * flashes through.
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
  let gen = -1;
  let polledAt = 0;
  let backoff = RETRY_MS;
  let wasPaused: boolean | null = null;

  const report = (err: unknown) =>
    console.error(`${APP}: ${err instanceof Error ? err.message : String(err)}`);

  /** Seconds left on this card, aged by our own elapsed time since the poll,
   *  so the countdown never depends on the bar's clock being right. */
  function remaining(card: Card): number | null {
    if (typeof card.in_s !== "number") return null;
    return Math.max(0, card.in_s - Math.round((Date.now() - polledAt) / 1000));
  }

  // The controls the case is engraved with are read on the HOST, off the
  // device's CLI: this firmware gives a JS app no input API at all. If a
  // future one does, `listen` appears and the app can forward events rather
  // than growing a second state machine.
  if (typeof listen === "function") {
    try {
      listen("input", () => {
        /* reserved: forward to the host once the firmware exposes this */
      });
    } catch (err) {
      report(err);
    }
  }

  async function loop(): Promise<void> {
    for (;;) {
      try {
        const url = `${HOST}/usage.json?since=${gen}&wait=${WAIT_MS}`;
        const data = await fetch(url).then((r) => r.json());
        polledAt = Date.now();
        backoff = RETRY_MS;

        cards = Array.isArray(data.cards) ? data.cards : [];
        const control = data.control ?? { index: 0, paused: false, gen: 0 };
        gen = control.gen;

        if (cards.length === 0) {
          await draw(message("no data", null));
        } else {
          const card = cards[control.index % cards.length];
          // Toast only on the change, not on every redraw of a paused card.
          const say =
            wasPaused === null || wasPaused === control.paused
              ? undefined
              : control.paused
                ? "paused"
                : "running";
          wasPaused = control.paused;
          await draw(frame(card, remaining(card), control.paused, say));
        }
      } catch (err) {
        // Host asleep, unplugged, or the daemon stopped. Say which rather
        // than leaving the last good frame up to go quietly stale.
        report(err);
        await draw(message("no host", null)).catch(report);
        await new Promise((resolve) => setTimeout(resolve, backoff));
        backoff = Math.min(backoff * 2, RETRY_MAX_MS);
      }
    }
  }

  void clearCanvas().catch(report).then(loop).catch(report);
}

/**
 * The runtime's input global, installed by js_input.c -- which postdates the
 * firmware this device runs, so it is absent here. Declared for the day a
 * newer one has it; the buttons are read host-side in the meantime.
 */
declare function listen(
  type: "input",
  handler: (event: InputEvent) => void,
): () => void;

type InputEvent = {
  key: "encoder" | "start" | "ok" | "back";
  action: "press" | "release" | "clockwise" | "counterclockwise";
  delta?: number;
};
