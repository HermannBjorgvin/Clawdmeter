#!/usr/bin/env node
/**
 * Convert makebead pixel-art editor exports into a claudepix-schema animation.
 *
 * One export per frame, in order. The export is already structured — a colour
 * dictionary plus a flat array of indices with null for empty — so nothing is
 * inferred here. No decoding, no grid detection, no colour quantisation, and
 * so nothing to eyeball afterwards.
 *
 *   node makebead_to_anim.js --name "stretch" --holds 300,120,300 f0.json f1.json f2.json
 *
 * Options:
 *   --name NAME       animation name (required; must also be listed in
 *                     GROUP_NAMES in firmware/src/splash.cpp or nothing shows it)
 *   --category NAME   Idle | Work | Dance | Expressions   (default Idle)
 *   --hold MS         per-frame hold, default 200
 *   --holds A,B,C     per-frame holds, one per frame, overrides --hold
 *   --brand           remap makebead's pure orange #FF8000 to the brand
 *                     terracotta #D97757 the rest of the set uses
 *   --out FILE        default tools/drawn_anims/<name>.json
 *   --dry             print the grids and palette, write nothing
 */

const fs = require('fs');
const path = require('path');

const GRID = 20;
const PALETTE_MAX = 10;             // firmware SPLASH_PALETTE_SIZE; slot 0 is empty
const BRAND_TERRACOTTA = '#D97757';
const MAKEBEAD_ORANGE = '#FF8000';

const argv = process.argv.slice(2);

// Options that consume the next argument. Needed explicitly: the inputs are
// .json and so is --out's value, so "everything ending in .json is an input"
// silently treats the output path as an extra frame.
const VALUED = new Set(['name', 'category', 'hold', 'holds', 'out']);

const files = [];
const opts = new Map();
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (!a.startsWith('--')) { files.push(a); continue; }
  const key = a.slice(2);
  if (VALUED.has(key)) opts.set(key, argv[++i]);
  else opts.set(key, true);
}
const opt = (k, def) => (opts.has(k) ? opts.get(k) : def);
const flag = k => opts.get(k) === true;

const NAME = opt('name', null);
const CATEGORY = opt('category', 'Idle');
const HOLD = parseInt(opt('hold', '200'), 10);
const HOLDS = opt('holds', null);
const BRAND = flag('brand');
const DRY = flag('dry');

if (!NAME || files.length === 0) {
  console.error('usage: makebead_to_anim.js --name "my anim" frame0.json [frame1.json ...]');
  process.exit(1);
}

// ── read every frame ────────────────────────────────────────────────────────
// Colours are keyed by hex across frames, not by dictionary slot: makebead
// writes each frame's dict independently, so the same colour can land on a
// different index in each file. Keying by slot would scramble any frame whose
// dict happens to be ordered differently.
const paletteHex = [];              // index 0 is empty, filled in below
const indexOfHex = new Map();

function loadFrame(file) {
  const d = JSON.parse(fs.readFileSync(file, 'utf8'));
  const [w, h] = d.size || [];
  if (w !== GRID || h !== GRID) {
    console.error(`${file}: canvas is ${w}x${h}, the firmware needs ${GRID}x${GRID}`);
    process.exit(1);
  }
  if (!Array.isArray(d.cells) || d.cells.length !== GRID * GRID) {
    console.error(`${file}: expected ${GRID * GRID} cells, got ${d.cells?.length}`);
    process.exit(1);
  }

  const grid = [];
  for (let r = 0; r < GRID; r++) {
    const row = [];
    for (let c = 0; c < GRID; c++) {
      const v = d.cells[r * GRID + c];
      if (v === null || v === undefined) { row.push(0); continue; }

      const entry = d.dict[v];
      if (!entry) {
        console.error(`${file}: cell ${r},${c} refers to dict slot ${v}, which doesn't exist`);
        process.exit(1);
      }
      let hex = entry.hex.toUpperCase();
      if (BRAND && hex === MAKEBEAD_ORANGE) hex = BRAND_TERRACOTTA;

      if (!indexOfHex.has(hex)) {
        if (paletteHex.length + 1 >= PALETTE_MAX) {
          console.error(`palette overflow at ${hex}: max ${PALETTE_MAX} including the empty slot`);
          process.exit(1);
        }
        indexOfHex.set(hex, paletteHex.length + 1);
        paletteHex.push(hex);
      }
      row.push(indexOfHex.get(hex));
    }
    grid.push(row);
  }
  return grid;
}

const grids = files.map(loadFrame);
const holds = HOLDS ? HOLDS.split(',').map(Number) : grids.map(() => HOLD);
if (holds.length !== grids.length) {
  console.error(`--holds has ${holds.length} values but there are ${grids.length} frames`);
  process.exit(1);
}

const palette = ['transparent', ...paletteHex];

// ── report ──────────────────────────────────────────────────────────────────
console.log(`\npalette (${palette.length}/${PALETTE_MAX}):`);
palette.forEach((c, i) => {
  let note = '';
  if (i === 0) note = '  (empty — renders as the panel background)';
  else if (/^#0{6}$|^#0F0F0F$/i.test(c)) note = '  (black — reads as a hole in the body, which is how the eyes work)';
  else if (isDark(c)) note = '  (DARK — the panel background is black, so this will be near-invisible)';
  console.log(`  ${i}  ${c}${note}`);
});

function isDark(hex) {
  const n = parseInt(hex.slice(1), 16);
  const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
  return (0.299 * r + 0.587 * g + 0.114 * b) < 60;
}

grids.forEach((g, i) => {
  console.log(`\n${path.basename(files[i])}  (hold ${holds[i]}ms)`);
  for (const row of g) console.log('  ' + row.map(v => (v === 0 ? '.' : v)).join(''));
});

if (DRY) { console.log('\n--dry: nothing written'); process.exit(0); }

const outFile = opt('out', path.join(__dirname, 'drawn_anims', `${NAME.replace(/ /g, '_')}.json`));
fs.mkdirSync(path.dirname(outFile), { recursive: true });
fs.writeFileSync(outFile, JSON.stringify({
  name: NAME,
  category: CATEGORY,
  description: `Drawn in makebead: ${files.length} frames.`,
  palette,
  frames: grids.map((grid, i) => ({ hold: holds[i], grid })),
}, null, 1));

console.log(`\nWrote ${outFile}`);
console.log(`Next: add "${NAME}" to a group in firmware/src/splash.cpp, then run convert_to_c.js`);
