#!/usr/bin/env node
/**
 * Turn hand-drawn 20x20 grid images into a claudepix-schema animation JSON.
 *
 * Input is one image per frame — a grid with cells coloured in, however you
 * drew it (spreadsheet, pixel editor, screenshot). Each cell is sampled from
 * the middle of its area, so drawn gridlines and cell borders are ignored.
 *
 * It ALWAYS prints the grid it read back as text. Check that against your
 * drawing before trusting the JSON — a half-cell crop offset produces a
 * plausible-looking grid that is quietly wrong everywhere.
 *
 * Usage:
 *   node grid_image_to_anim.js --name "my anim" [options] frame0.png frame1.png ...
 *
 * Options:
 *   --name NAME        animation name (required; must also be added to a group
 *                      in splash.cpp or nothing will ever pick it)
 *   --category NAME    Idle | Work | Dance | Expressions   (default Idle)
 *   --hold MS          per-frame hold, default 200
 *   --holds A,B,C      per-frame holds, one per frame, overrides --hold
 *   --cells N          grid size, default 20 (the firmware requires 20)
 *   --crop x0,y0,x1,y1 use only this pixel box of each image, if your export
 *                      has margins, a toolbar, or a caption around the grid
 *   --bg RRGGBB        treat this colour as empty (default: whatever colour
 *                      occupies the most cells, which is the paper/background
 *                      in every drawing we've seen)
 *   --out FILE         default tools/custom_anims/<name>.json
 *   --dry              print the grids and the palette, write nothing
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const GRID_DEFAULT = 20;
const PALETTE_MAX = 10;   // firmware's SPLASH_PALETTE_SIZE; slot 0 is empty

// ── args ─────────────────────────────────────────────────────────────────────
const argv = process.argv.slice(2);
const files = argv.filter(a => !a.startsWith('--') && /\.(png)$/i.test(a));
const opt = (k, def) => {
  const i = argv.indexOf(`--${k}`);
  return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : def;
};
const flag = k => argv.includes(`--${k}`);

const NAME = opt('name', null);
const CATEGORY = opt('category', 'Idle');
const CELLS = parseInt(opt('cells', GRID_DEFAULT), 10);
const HOLD = parseInt(opt('hold', '200'), 10);
const HOLDS = opt('holds', null);
const CROP = opt('crop', null);
const BG = opt('bg', null);
const DRY = flag('dry');

if (!NAME || files.length === 0) {
  console.error('usage: grid_image_to_anim.js --name "my anim" frame0.png [frame1.png ...]');
  process.exit(1);
}

// ── PNG decode (all five scanline filters; 8-bit RGB / RGBA / grey) ──────────
function decodePng(file) {
  const d = fs.readFileSync(file);
  let i = 8, idat = [], w = 0, h = 0, bpp = 3, ctype = 2, depth = 8, plte = null, trns = null;
  while (i < d.length) {
    const len = d.readUInt32BE(i);
    const type = d.toString('ascii', i + 4, i + 8);
    if (type === 'IHDR') {
      w = d.readUInt32BE(i + 8); h = d.readUInt32BE(i + 12);
      depth = d[i + 16]; ctype = d[i + 17];
      bpp = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ctype];
    } else if (type === 'PLTE') {
      plte = d.subarray(i + 8, i + 8 + len);
    } else if (type === 'tRNS') {
      trns = d.subarray(i + 8, i + 8 + len);
    } else if (type === 'IDAT') {
      idat.push(d.subarray(i + 8, i + 8 + len));
    }
    i += 12 + len;
  }
  if (depth !== 8) throw new Error(`${file}: only 8-bit PNGs are supported (got ${depth}-bit)`);

  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * bpp;
  const out = Buffer.alloc(h * stride);
  let pos = 0;
  for (let y = 0; y < h; y++) {
    const f = raw[pos++];
    const line = Buffer.from(raw.subarray(pos, pos + stride)); pos += stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? line[x - bpp] : 0;
      const b = y ? out[(y - 1) * stride + x] : 0;
      const c = (y && x >= bpp) ? out[(y - 1) * stride + x - bpp] : 0;
      if (f === 1) line[x] = (line[x] + a) & 0xff;
      else if (f === 2) line[x] = (line[x] + b) & 0xff;
      else if (f === 3) line[x] = (line[x] + ((a + b) >> 1)) & 0xff;
      else if (f === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        line[x] = (line[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c)) & 0xff;
      }
    }
    line.copy(out, y * stride);
  }

  // Normalize to RGBA
  const rgba = Buffer.alloc(w * h * 4, 255);
  for (let p = 0; p < w * h; p++) {
    const o = p * bpp;
    if (ctype === 2) { rgba[p*4] = out[o]; rgba[p*4+1] = out[o+1]; rgba[p*4+2] = out[o+2]; }
    else if (ctype === 6) { rgba[p*4] = out[o]; rgba[p*4+1] = out[o+1]; rgba[p*4+2] = out[o+2]; rgba[p*4+3] = out[o+3]; }
    else if (ctype === 0) { rgba[p*4] = rgba[p*4+1] = rgba[p*4+2] = out[o]; }
    else if (ctype === 4) { rgba[p*4] = rgba[p*4+1] = rgba[p*4+2] = out[o]; rgba[p*4+3] = out[o+1]; }
    else if (ctype === 3) {
      const idx = out[o];
      rgba[p*4] = plte[idx*3]; rgba[p*4+1] = plte[idx*3+1]; rgba[p*4+2] = plte[idx*3+2];
      if (trns && idx < trns.length) rgba[p*4+3] = trns[idx];
    }
  }
  return { w, h, rgba };
}

// ── sample one cell: the most common colour in its middle 60% ────────────────
// The margin matters. Sampling the full cell picks up drawn gridlines and
// antialiased edges; sampling a single centre pixel is at the mercy of one
// stray pixel of JPEG-ish noise or a cursor artifact.
function sampleCell(img, x0, y0, cw, ch) {
  // Inset only while something is left to sample. A pixel-editor export is one
  // pixel per cell, and a fixed inset there eats the whole cell and reports an
  // entirely empty grid.
  const inset = size => (size - 2 * Math.floor(size * 0.2) >= 1 ? Math.floor(size * 0.2) : 0);
  const mx = inset(cw), my = inset(ch);
  const tally = new Map();
  for (let y = Math.floor(y0 + my); y < Math.max(y0 + ch - my, y0 + my + 1); y++) {
    for (let x = Math.floor(x0 + mx); x < Math.max(x0 + cw - mx, x0 + mx + 1); x++) {
      if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
      const o = (y * img.w + x) * 4;
      const key = img.rgba[o + 3] < 128 ? 'T'
        : `${img.rgba[o]},${img.rgba[o + 1]},${img.rgba[o + 2]}`;
      tally.set(key, (tally.get(key) || 0) + 1);
    }
  }
  let best = 'T', bestN = -1;
  for (const [k, n] of tally) if (n > bestN) { best = k; bestN = n; }
  return best;
}

function hex(key) {
  if (key === 'T') return 'transparent';
  const [r, g, b] = key.split(',').map(Number);
  return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('').toUpperCase();
}

// ── find the grid inside the image ──────────────────────────────────────────
// Screenshots and photos arrive with the grid sitting inside something else —
// a page, a spreadsheet's grey surround, a phone's letterboxing. Getting this
// wrong is the failure mode that matters: half a cell of offset yields a grid
// that looks plausible and is wrong in every row, so it's worth doing rather
// than asking for a pixel-perfect crop.
//
// Trims uniform borders: whole rows/columns matching the corner colour come
// off until something interesting starts. --crop overrides it entirely.
function autoCrop(img) {
  // A pixel-editor export is already exactly the grid — its dimensions are a
  // clean multiple of the cell count. Cropping one of those is actively wrong:
  // the empty background IS content, and trimming it shifts every cell.
  if (img.w % CELLS === 0 && img.h % CELLS === 0) return [0, 0, img.w, img.h];

  const at = (x, y) => {
    const o = (y * img.w + x) * 4;
    return [img.rgba[o], img.rgba[o + 1], img.rgba[o + 2], img.rgba[o + 3]];
  };
  const near = (p, q) => p[3] < 128 === q[3] < 128 &&
    Math.abs(p[0] - q[0]) < 24 && Math.abs(p[1] - q[1]) < 24 && Math.abs(p[2] - q[2]) < 24;
  const border = at(0, 0);
  const rowUniform = y => { for (let x = 0; x < img.w; x++) if (!near(at(x, y), border)) return false; return true; };
  const colUniform = x => { for (let y = 0; y < img.h; y++) if (!near(at(x, y), border)) return false; return true; };

  let y0 = 0, y1 = img.h, x0 = 0, x1 = img.w;
  while (y0 < y1 - 1 && rowUniform(y0)) y0++;
  while (y1 > y0 + 1 && rowUniform(y1 - 1)) y1--;
  while (x0 < x1 - 1 && colUniform(x0)) x0++;
  while (x1 > x0 + 1 && colUniform(x1 - 1)) x1--;
  return [x0, y0, x1, y1];
}

// ── read every frame into a grid of colour keys ─────────────────────────────
const crop = CROP ? CROP.split(',').map(Number) : null;
const rawGrids = files.map(file => {
  const img = decodePng(file);
  const [x0, y0, x1, y1] = crop || autoCrop(img);
  const cw = (x1 - x0) / CELLS, ch = (y1 - y0) / CELLS;
  const g = [];
  for (let r = 0; r < CELLS; r++) {
    const row = [];
    for (let c = 0; c < CELLS; c++) row.push(sampleCell(img, x0 + c * cw, y0 + r * ch, cw, ch));
    g.push(row);
  }
  const how = crop ? 'crop' : 'auto';
  return { file, grid: g, size: `${img.w}x${img.h}`, box: `${how} ${x0},${y0}-${x1},${y1}` };
});

// ── build the palette ───────────────────────────────────────────────────────
// Slot 0 is empty. Everything else is ranked by how many cells use it, so if
// the drawing overflows PALETTE_MAX the colours that get dropped are the ones
// used least — a stray antialiased pixel loses to a real fill.
const counts = new Map();
for (const { grid } of rawGrids)
  for (const row of grid) for (const k of row) counts.set(k, (counts.get(k) || 0) + 1);

const ranked = [...counts.entries()].sort((a, b) => b[1] - a[1]);
const bgKey = BG ? [...counts.keys()].find(k => hex(k) === '#' + BG.toUpperCase()) ?? 'T'
                 : ranked[0][0];

const palette = ['transparent'];
const indexOf = new Map([[bgKey, 0]]);
for (const [k] of ranked) {
  if (k === bgKey || indexOf.has(k)) continue;
  if (palette.length >= PALETTE_MAX) {
    console.warn(`! palette full — dropping ${hex(k)} (${counts.get(k)} cells) to empty`);
    indexOf.set(k, 0);
    continue;
  }
  indexOf.set(k, palette.length);
  palette.push(hex(k));
}

// ── emit ────────────────────────────────────────────────────────────────────
const holds = HOLDS ? HOLDS.split(',').map(Number) : rawGrids.map(() => HOLD);
if (holds.length !== rawGrids.length) {
  console.error(`--holds has ${holds.length} values but there are ${rawGrids.length} frames`);
  process.exit(1);
}

const frames = rawGrids.map(({ grid }, i) => ({
  hold: holds[i],
  grid: grid.map(row => row.map(k => indexOf.get(k))),
}));

console.log(`\npalette (${palette.length}/${PALETTE_MAX}):`);
palette.forEach((c, i) => console.log(`  ${i}  ${c}${i === 0 ? '  (empty)' : ''}`));

// Always print what was read. This is the check that catches a bad crop.
rawGrids.forEach(({ file, size, box }, i) => {
  console.log(`\n${path.basename(file)}  (${size}, grid box ${box}, hold ${holds[i]}ms)`);
  for (const row of frames[i].grid) {
    console.log('  ' + row.map(v => (v === 0 ? '.' : v)).join(''));
  }
});

if (DRY) { console.log('\n--dry: nothing written'); process.exit(0); }

const outFile = opt('out', path.join(__dirname, 'custom_anims', `${NAME.replace(/ /g, '_')}.json`));
const anim = {
  filename: `${NAME.replace(/ /g, '_')}.html`,
  name: NAME, category: CATEGORY,
  description: `Hand-drawn: ${files.length} frames.`,
  palette, frame_count: frames.length, frames,
};
fs.mkdirSync(path.dirname(outFile), { recursive: true });
fs.writeFileSync(outFile, JSON.stringify(anim, null, 1));
console.log(`\nWrote ${outFile}`);
console.log(`Next: add "${NAME}" to a group in firmware/src/splash.cpp, then run convert_to_c.js`);
