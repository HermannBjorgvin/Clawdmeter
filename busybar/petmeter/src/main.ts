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

/** The name the app draws under; the device clears elements by this id. */
const APP = manifest.id;

/**
 * Where the usage comes from. The Petmeter daemon serves its latest poll at
 * /usage.json. Over USB both addresses are fixed — the bar is 10.0.4.20 and
 * the host 10.0.4.21 — so there is nothing to discover and no Wi-Fi involved.
 */
const HOST = import.meta.env.VITE_PETMETER_HOST ?? "http://10.0.4.21:8724";

const POLL_MS = 30_000; // the daemon polls every 60s; half that is never
                        // more than one poll behind
const CARD_MS = 4_000;  // how long each quota holds the screen
const TICK_MS = 250;    // how often the dwell timer is checked, which is also
                        // how quickly a press feels like it did something

const WIDTH = 72;
const PET = 16;          // the mascot is a square the height of the screen
const GAP = 2;

// Thresholds and colours from the firmware's pct_color(), so the bar and the
// meter on the desk never disagree about whether a number is alarming.
const WARN_PCT = 75;
const CRIT_PCT = 90;
const COL_OK = "#8FA76BFF";
const COL_WARN = "#D97757FF";
const COL_CRIT = "#C0392BFF";
const COL_TEXT = "#FAF9F5FF";
// Panel greys vanish on an LED matrix — there is no lit background for them to
// sit against — so "dim" here is the firmware's dim, not its track colour.
const COL_DIM = "#B0AEA5FF";
const COL_TRACK = "#2A2A28FF";

/**
 * The runtime's input global. `listen` is installed on the realm by
 * js_input.c, so it is not an import — it is simply there, and TypeScript has
 * to be told. The handler gets one event per physical action; the returned
 * function unbinds.
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

/** One quota, as the daemon describes it. */
type Card = {
  provider: "claude" | "codex" | string;
  label: string;
  pct?: number;
  resets_at?: number;
  held?: number;
  used?: number;
  expires_at?: number;
};

// Image paths resolve against the APP ROOT, not the assets folder -- and one
// unreadable image rejects the whole draw with a 400, so a wrong path here
// means no frame at all rather than a frame with a gap in it.
const PET_IMAGE: Record<string, string> = {
  claude: "appmeta/assets/clawd_16.png",
  codex: "appmeta/assets/codey_16.png",
};

const PANE_X = PET + GAP;        // where the reading starts
const BAR_Y = 13;
const BAR_H = 3;

/** Advance width per character, measured on the device's bitmap fonts. */
const ADV = { small: 4, normal: 6 } as const;

function textWidth(text: string, font: keyof typeof ADV): number {
  return text.length * ADV[font];
}

function colorFor(pct: number): string {
  if (pct >= CRIT_PCT) return COL_CRIT;
  if (pct >= WARN_PCT) return COL_WARN;
  return COL_OK;
}

function text(
  id: string,
  value: string,
  font: keyof typeof ADV,
  x: number,
  y: number,
  color: string,
): Element {
  return {
    id,
    type: "text",
    text: value,
    font,
    x,
    y,
    align: "top_left",
    color,
    display: "front",
    timeout: 0,
  };
}

/** A rectangle has no `color`: it has a fill and a border, and the border
 *  defaults to 1px white. Pass only a colour and you get an outline. */
function rect(id: string, x: number, y: number, w: number, h: number, color: string): Element {
  return {
    id,
    type: "rectangle",
    x,
    y,
    width: w,
    height: h,
    radius: 0,
    fill: "solid",
    fill_colors: [color],
    border_width: 0,
    display: "front",
    timeout: 0,
  };
}

function petOf(card: Card): Element {
  return {
    id: "pet",
    type: "image",
    path: PET_IMAGE[card.provider] ?? PET_IMAGE.claude,
    x: 0,
    y: 0,
    width: PET,
    height: PET,
    display: "front",
    timeout: 0,
  };
}

/** A quota: its name, its number, and a bar showing the proportion spent. */
function quota(card: Card): Element[] {
  const pct = Math.round(card.pct ?? 0);
  const shown = `${pct}%`;
  const pane = WIDTH - PANE_X;
  const filled = pct > 0 ? Math.max(1, Math.round((pane * Math.min(pct, 100)) / 100)) : 0;

  const out: Element[] = [
    petOf(card),
    text("label", card.label, "small", PANE_X, 2, COL_DIM),
    // Right-aligned by arithmetic: the number is the thing you read first, so
    // it sits against the edge rather than drifting with the label's length.
    text("pct", shown, "normal", WIDTH - textWidth(shown, "normal"), 1, COL_TEXT),
    rect("track", PANE_X, BAR_Y, pane, BAR_H, COL_TRACK),
  ];
  if (filled > 0) out.push(rect("fill", PANE_X, BAR_Y, filled, BAR_H, colorFor(pct)));
  return out;
}

/**
 * Reset credits are a count, not a proportion: how many grants the window
 * handed out and how many are left. Drawn as a count and a countdown rather
 * than a bar, since there is no whole for it to be a fraction of.
 */
function credits(card: Card): Element[] {
  const total = (card.held ?? 0) + (card.used ?? 0);
  const shown = `${card.held ?? 0}/${total}`;
  const out: Element[] = [
    petOf(card),
    text("label", card.label, "small", PANE_X, 2, COL_DIM),
    text("count", shown, "normal", WIDTH - textWidth(shown, "normal"), 1, COL_TEXT),
  ];
  if (card.expires_at) {
    out.push({
      id: "exp",
      type: "countdown",
      timestamp: String(card.expires_at),
      direction: "time_left",
      show_hours: "when_non_zero",
      x: PANE_X,
      y: BAR_Y - 3,
      align: "top_left",
      color: COL_DIM,
      display: "front",
      timeout: 0,
    });
  }
  return out;
}

function frame(card: Card, paused: boolean): Element[] {
  const body = card.pct === undefined ? credits(card) : quota(card);
  // Pressing pause with nothing on screen to show for it feels broken, and
  // there is no room for a word. A dot in the corner is the whole budget.
  return paused ? [...body, rect("paused", WIDTH - 2, 0, 2, 2, COL_DIM)] : body;
}

function message(value: string): Element[] {
  return [text("msg", value, "small", PANE_X, 6, COL_DIM)];
}

/** The bar's own address, from inside the bar. */
const SELF = "http://10.0.4.20";

/**
 * One endpoint, called directly.
 *
 * The scaffold routes this through `@shared/device`, the generated BusyBar
 * client — which drags in an OpenAPI fetch layer and every other endpoint with
 * it. That bundles to ~36 KB minified, and a bundle that size dies silently on
 * this runtime while a 37 KB file of comments runs fine, so it is the weight
 * of the code and not the file that the device objects to. We call exactly one
 * endpoint; the client is not worth its bundle.
 */
async function draw(elements: Element[]): Promise<void> {
  await fetch(
    new Request(`${SELF}/api/display/draw`, {
      method: "POST",
      body: JSON.stringify({ application_name: APP, priority: 50, elements }),
    }),
  );
}

/** Entry point. The build rewrites the default export into a call. */
export default function run(): void {
  let cards: Card[] = [];
  let index = 0;
  let paused = false;
  let dwell = 0;

  const report = (err: unknown) =>
    console.error(`${APP}: ${err instanceof Error ? err.message : String(err)}`);

  function show(): void {
    if (cards.length === 0) return;
    index = ((index % cards.length) + cards.length) % cards.length;
    void draw(frame(cards[index], paused)).catch(report);
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
      cards = Array.isArray(data.cards) ? data.cards : [];
      if (cards.length === 0) await draw(message("no data"));
    } catch (err) {
      // Host asleep, unplugged, or the daemon stopped. Say which rather than
      // leaving the last good frame up, which would quietly go stale.
      report(err);
      cards = [];
      await draw(message("no host")).catch(report);
    }
  }

  // Controls follow what the case itself says: the red bar is Start/Pause, so
  // it holds and releases the rotation; the wheel is Scroll, so it steps by
  // hand in either direction; and the wheel's press is engraved "OK/Skip", so
  // it skips forward. A manual step restarts the dwell, or the card you just
  // asked for would vanish a moment later.
  listen("input", (event: InputEvent) => {
    if (event.action === "release") return;      // one action per press
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

  void poll().then(show).catch(report);
  setInterval(() => void poll().catch(report), POLL_MS);
  setInterval(() => {
    if (paused || cards.length === 0) return;
    dwell += TICK_MS;
    if (dwell >= CARD_MS) step(1);
  }, TICK_MS);
}
