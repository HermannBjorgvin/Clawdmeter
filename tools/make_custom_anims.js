#!/usr/bin/env node
/**
 * Builds hand-composed animations into tools/custom_anims/, in the same shape
 * the scraper emits for tools/claudepix_data/.
 *
 * These are NOT redrawn characters. Each one takes an existing claudepix
 * animation as its base and overlays props that track the creature frame by
 * frame — the head is located per frame, so headphones stay on his head
 * through a bounce instead of being pinned to fixed coordinates. That keeps
 * the character exactly as claudepix drew him.
 *
 * Output: tools/custom_anims/<name>.json + _index.json, picked up by
 * convert_to_c.js alongside the scraped set.
 *
 * Usage: node make_custom_anims.js [--preview out.png]
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const SRC_DIR = path.join(__dirname, 'claudepix_data');
const OUT_DIR = path.join(__dirname, 'custom_anims');
const GRID = 20;

const args = process.argv.slice(2);
const opt = (k, def) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : def; };

// ── palette ──────────────────────────────────────────────────────────────────
// Slots 0-2 must match the base animation's palette so its cells keep meaning
// (0 transparent, 1 body, 2 eyes). 3+ are ours. Everything we add has to be
// LIGHT: the panel background is black, so a dark prop is an invisible prop.
const BODY = 1, EYE = 2, CUSHION = 3, BAND = 4, SHADOW = 5, NOTE = 6;
const PALETTE = [
  'transparent',
  '#CD7F6A',   // body — converter remaps this to brand terracotta
  '#0f0f0f',   // eyes
  '#c0d8e4',   // ear cushions
  '#ffffff',   // headband
  '#7aaabb',   // cushion shadow
  '#e8e0d0',   // music notes
];

// Sleep variants keep slots 0-2 (the base animation's own palette) and add
// their drifting mark on top.
const MARK = 3, MARK_CENTER = 4, MARK_BRIGHT = 4;
const PALETTE_HEARTS = ['transparent', '#CD7F6A', '#0f0f0f', '#e8788f', '#ffc2d1'];
const PALETTE_BLOSSOM = ['transparent', '#CD7F6A', '#0f0f0f', '#f3b6c8', '#ffd98a'];

// ── helpers ──────────────────────────────────────────────────────────────────
const blank = () => Array.from({ length: GRID }, () => new Array(GRID).fill(0));
const inBounds = (r, c) => r >= 0 && r < GRID && c >= 0 && c < GRID;
function put(g, r, c, v) { if (inBounds(r, c) && g[r][c] === 0) g[r][c] = v; }
function putOver(g, r, c, v) { if (inBounds(r, c)) g[r][c] = v; }

// Locate the head: first row holding body pixels, and that row's extent.
function findHead(grid) {
  for (let r = 0; r < GRID; r++) {
    const cols = [];
    for (let c = 0; c < GRID; c++) if (grid[r][c] === BODY || grid[r][c] === EYE) cols.push(c);
    if (cols.length) return { top: r, left: cols[0], right: cols[cols.length - 1] };
  }
  return null;
}

// Headphones anchored to the head found above, so they ride the bounce.
function addHeadphones(grid, head) {
  const { top, left, right } = head;

  // Ear cushions: 2 wide x 3 tall just outside the head, stopping short of the
  // row where the arms stick out.
  for (let dr = 0; dr < 3; dr++) {
    for (let dc = 1; dc <= 2; dc++) {
      putOver(grid, top + dr, left - dc, dr === 2 ? SHADOW : CUSHION);
      putOver(grid, top + dr, right + dc, dr === 2 ? SHADOW : CUSHION);
    }
  }
  // Band: shoulders one row above the head, arcing across two rows above that.
  putOver(grid, top - 1, left - 1, BAND);
  putOver(grid, top - 1, left, BAND);
  putOver(grid, top - 1, right, BAND);
  putOver(grid, top - 1, right + 1, BAND);
  for (let c = left + 1; c <= right - 1; c++) putOver(grid, top - 2, c, BAND);
}

// A note is four cells — a two-tall stem over a two-wide head:
//   .6
//   .6
//   66
// Three cells read as crumbs at this size; the stem is what makes it a note.
// Drawn with put(), so where he overlaps it the note passes behind him.
function addNote(grid, r, c) {
  put(grid, r - 2, c + 1, NOTE);
  put(grid, r - 1, c + 1, NOTE);
  put(grid, r, c, NOTE);
  put(grid, r, c + 1, NOTE);
}

// ── fm listening ─────────────────────────────────────────────────────────────
// Base: dance_bounce (he already moves to a beat). Add headphones + two notes
// drifting up the sides on offset phases, so the loop never looks metronomic.
function buildFmListening() {
  const base = JSON.parse(fs.readFileSync(path.join(SRC_DIR, 'dance_bounce.json'), 'utf8'));
  const n = base.frames.length;

  const frames = base.frames.map((f, i) => {
    const g = blank();
    for (let r = 0; r < GRID; r++) for (let c = 0; c < GRID; c++) g[r][c] = f.grid[r][c];

    const head = findHead(g);
    if (head) addHeadphones(g, head);

    // Each note climbs one row every other frame and restarts at the bottom.
    // The two columns run half a cycle apart. Columns 0-1 and 18-19 are the
    // only ones the ear cushions (left-2, right+2) never claim.
    const travel = 10, bottom = 13;
    const phaseL = Math.floor(i / 2) % travel;
    const phaseR = Math.floor((i + n / 2) / 2) % travel;
    addNote(g, bottom - phaseL, 0);
    addNote(g, bottom - phaseR, 18);

    return { hold: f.hold, grid: g };
  });

  return {
    filename: 'fm_listening.html',
    name: 'fm listening',
    category: 'Dance',
    description: 'Headphones on, bouncing to the stream, notes drifting up both sides.',
    palette: PALETTE,
    frame_count: frames.length,
    frames,
  };
}

// ── sleep variants ───────────────────────────────────────────────────────────
// Base: expression_sleep. That animation draws its "Zzz" in the BODY color a
// few pixels above his head, and his body never reaches above the head row —
// so everything above the head is the Zzz, and clearing those rows strips it
// cleanly without touching him. Then we drift our own mark up instead.

// A blossom survives 3x3 — a cross of petals around a lit center reads
// immediately. A heart does not: its two top humps land on separate pixels
// with a gap between them and the thing reads as a pair of antennae. So the
// heart gets the full 5x4 form, which needs the clear band above his head.
//
//   3x3 blossom     5x3 heart
//     .P.            .H.H.
//     PCP            HHHHH
//     .P.            .HHH.
//
// The heart is three rows, not four: his head starts at row 4, and a four-row
// heart fills the clear band exactly, leaving no gap — it then reads as a bow
// sitting on his head rather than something floating above it. Dropping the
// bottom point buys the row of black that sells the float.
//
// It also never changes size. A 3x3 heart was tried as the pulsed-out state
// and it isn't legible: the two top humps sit on separate pixels with a gap
// between them, so it reads as a pair of antennae. The beat is carried by
// brightness instead, which keeps the silhouette readable in every frame.
function drawBlossom(g, r, c) {
  for (const [dr, dc] of [[0, 1], [1, 0], [1, 2], [2, 1]]) put(g, r + dr, c + dc, MARK);
  put(g, r + 1, c + 1, MARK_CENTER);
}
const HEART_BIG = [[0, 1], [0, 3], [1, 0], [1, 1], [1, 2], [1, 3], [1, 4],
                   [2, 1], [2, 2], [2, 3]];
function drawHeart(g, r, c, color) {
  for (const [dr, dc] of HEART_BIG) put(g, r + dr, c + dc, color);
}

// Blossoms drift up both sides. Confined to rows 1-6 in columns 2-4 and 16-18:
// his arms reach out to columns 3 and 17 from row 7 down, so anything drifting
// lower is swallowed by his silhouette. Above row 7 those columns stay clear.
// travel divides the 12 steps of a 24-frame loop evenly, so the marks land back
// where they started exactly as the animation wraps.
function driftBlossoms(g, i, n) {
  const steps = Math.floor(i / 2), travel = 4, bottom = 4;
  drawBlossom(g, bottom - (steps % travel), 16);
  drawBlossom(g, bottom - ((steps + 2) % travel), 2);
}

// One heart beating over his head, in the band above row 4 that his body never
// reaches. The beat is carried by brightness: lit for the first third of each
// beat, resting shade otherwise. Column 7 centers the 5-wide form on his head,
// which spans columns 5-15.
//
// Two beats per loop whatever the base's frame count — a fixed period would
// leave a stutter at the wrap on any base whose length it doesn't divide
// (expression_sleep has 24 frames, idle_breathe has 16).
function beatHeart(g, i, n) {
  const period = Math.max(4, Math.round(n / 2));
  drawHeart(g, 0, 7, (i % period) < Math.ceil(period / 3) ? MARK_BRIGHT : MARK);
}

// `base` picks whose body we decorate. expression_sleep has his eyes CLOSED —
// claudepix simply draws no eye pixels in it — so a heart over that base reads
// as a good dream. idle_breathe keeps his eyes open (and blinking mid-loop),
// which reads as him being pleased about something. Both are worth having.
function buildDecoratedVariant({ name, base: baseName, description, palette, decorate }) {
  const base = JSON.parse(fs.readFileSync(path.join(SRC_DIR, `${baseName}.json`), 'utf8'));

  const frames = base.frames.map((f, i) => {
    const g = blank();
    for (let r = 0; r < GRID; r++) for (let c = 0; c < GRID; c++) g[r][c] = f.grid[r][c];

    // Clear the band above his head. On expression_sleep that band holds the
    // "Zzz", which is drawn in the BODY color — so it can't be found by color,
    // only by position. His head is the first row more than a few cells wide;
    // everything above it is prop, and ours replaces it. On bases without a
    // prop up there (idle_breathe) this is a no-op.
    let headTop = 0;
    for (let r = 0; r < GRID; r++) {
      const n = g[r].filter(v => v !== 0).length;
      if (n >= 6) { headTop = r; break; }
    }
    for (let r = 0; r < headTop; r++) g[r].fill(0);

    decorate(g, i, base.frames.length);

    return { hold: f.hold, grid: g };
  });

  return {
    filename: `${name.replace(/ /g, '_')}.html`,
    name, category: 'Idle', description, palette,
    frame_count: frames.length, frames,
  };
}

// ── preview PNG (contact sheet of every frame) ───────────────────────────────
function crc32(buf) {
  const table = [];
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  let crc = 0xffffffff;
  for (const b of buf) crc = table[(crc ^ b) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function writePreview(file, anim, cell = 6, cols = 8) {
  const pal = anim.palette.map(s => s === 'transparent' ? [0, 0, 0]
    : [parseInt(s.slice(1, 3), 16), parseInt(s.slice(3, 5), 16), parseInt(s.slice(5, 7), 16)]);
  const tile = GRID * cell + 4;
  const rows = Math.ceil(anim.frames.length / cols);
  const W = tile * cols, H = tile * rows;
  const img = Buffer.alloc(W * H * 3);

  anim.frames.forEach((f, i) => {
    const ox = (i % cols) * tile + 2, oy = Math.floor(i / cols) * tile + 2;
    for (let gy = 0; gy < GRID; gy++) for (let gx = 0; gx < GRID; gx++) {
      const [r, g, b] = pal[f.grid[gy][gx]] || [0, 0, 0];
      for (let dy = 0; dy < cell; dy++) for (let dx = 0; dx < cell; dx++) {
        const x = ox + gx * cell + dx, y = oy + gy * cell + dy;
        const o = (y * W + x) * 3;
        img[o] = r; img[o + 1] = g; img[o + 2] = b;
      }
    }
  });

  const raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) {
    raw[y * (W * 3 + 1)] = 0;
    img.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(file, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0)),
  ]));
}

// ── main ─────────────────────────────────────────────────────────────────────
const anims = [
  buildFmListening(),
  // Only awake bases. Closed-eye versions of these were built and dropped:
  // expression_sleep already covers "he's asleep", and a second sleeping
  // animation spends catalog space without adding a state you can read at a
  // glance. The Zzz-stripping in buildDecoratedVariant stays because it's what
  // makes an expression_sleep base possible at all, should one be wanted again.
  buildDecoratedVariant({
    name: 'idle hearts', base: 'idle_breathe',
    description: 'Awake and pleased about it, heart beating overhead.',
    palette: PALETTE_HEARTS, decorate: beatHeart,
  }),
  buildDecoratedVariant({
    name: 'idle blossom', base: 'idle_breathe',
    description: 'Awake, breathing easy, blossoms drifting up both sides.',
    palette: PALETTE_BLOSSOM, decorate: driftBlossoms,
  }),
];

fs.mkdirSync(OUT_DIR, { recursive: true });
// Wipe stale outputs first: without this a removed animation lingers as an
// orphan .json and the next reader has to guess whether it's still live.
for (const f of fs.readdirSync(OUT_DIR)) {
  if (f.endsWith('.json')) fs.unlinkSync(path.join(OUT_DIR, f));
}
for (const a of anims) {
  fs.writeFileSync(path.join(OUT_DIR, a.filename.replace('.html', '.json')), JSON.stringify(a, null, 1));
}
fs.writeFileSync(path.join(OUT_DIR, '_index.json'), JSON.stringify(
  anims.map(a => ({
    filename: a.filename, name: a.name, category: a.category,
    frame_count: a.frame_count, palette_size: a.palette.length,
  })), null, 2));

console.log(`Wrote ${anims.length} custom animation(s) to ${OUT_DIR}`);
// --preview takes a path prefix; one contact sheet per animation.
const preview = opt('--preview', null);
if (preview) {
  for (const a of anims) {
    const file = `${preview}-${a.name.replace(/ /g, '_')}.png`;
    writePreview(file, a);
    console.log(`Preview: ${file}`);
  }
}
