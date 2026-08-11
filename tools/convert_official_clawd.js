#!/usr/bin/env node
/**
 * Converts the official Anthropic Clawd animations (research/clawd-official/)
 * to firmware/src/splash_animations.h.
 *
 * Sources are the shipped GIFs plus the two Lottie exports (Laptop, Soccer —
 * the Soccer GIF is an antialiased rescale, the Lottie is crisp). All art
 * lives on one logical 55×37 stage: core GIFs at 2750×1850 (50 px/cell),
 * persona GIFs at 1189×800 or 1188×798 (~21.6 px/cell, non-integer). Cells
 * are recovered by sampling cell centers after fitting the grid phase per
 * file — never by block averaging. See research/clawd-official/CLAUDE.md.
 *
 * Output format (v2): per-animation bounding-box-cropped frames, one byte per
 * cell indexing a ≤16-entry RGB565 palette (index 0 = background), per-frame
 * hold in ms with consecutive duplicate frames collapsed.
 *
 * Requires ImageMagick (`convert`, `identify`) on PATH for GIF decoding.
 *
 * Usage: node convert_official_clawd.js [--in DIR] [--out FILE] [--verify DIR]
 *   --verify DIR  additionally writes a mid-frame PNG per animation for
 *                 eyeballing against the originals.
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const args = process.argv.slice(2);
const opt = (k, def) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : def; };

const IN_DIR = path.resolve(opt('--in', path.join(__dirname, '..', 'research', 'clawd-official')));
const OUT_FILE = path.resolve(opt('--out',
  path.join(__dirname, '..', 'firmware', 'src', 'splash_animations.h')));
const VERIFY_DIR = opt('--verify', null);

const PALETTE_SIZE = 16;
const STAGE_W = 55, STAGE_H = 37;
const LOTTIE_HOLD_MS = 83;          // 12 fps
// AA-remnant clustering threshold. Must stay below the closest real palette
// pair — body #D97757 vs shading #BE684D are only Δ(26,14,9) apart (dist² 953).
const COLOR_MERGE_DIST2 = 600;

// The catalog. `kind: 'lottie'` reads the JSON export; everything else is a
// GIF decoded via ImageMagick. Names are what the engine's rate groups and
// splash_mini_create() match against.
const ANIMS = [
  { file: 'Clawd-CrabWalking.gif',   name: 'crab walking',  category: 'core' },
  { file: 'Clawd-Walking.gif',       name: 'walking',       category: 'core' },
  { file: 'Clawd-Waving.gif',        name: 'waving',        category: 'core' },
  { file: 'Clawd-Jumping.gif',       name: 'jumping',       category: 'core' },
  { file: 'Clawd-JumpingHappy.gif',  name: 'jumping happy', category: 'core' },
  { file: 'Clawd-Pointing.gif',      name: 'pointing',      category: 'core' },
  { file: 'Clawd-Dancing.gif',       name: 'dancing',       category: 'core' },
  { file: 'Clawd-Laptop.lottie.json', name: 'laptop',       category: 'core', kind: 'lottie' },
  { file: 'Clawd-Cloud-once.gif',    name: 'cloud',         category: 'persona' },
  { file: 'Clawd-Magnifier.gif',     name: 'magnifier',     category: 'persona' },
  // Racing car's driving lap repeats with period 16 but two frames differ by
  // 1-2 cells (exhaust variation), so exact cycle detection misses it. Manual
  // loop window: f30≈f14 makes 30→15 cycle seamlessly, and release exits into
  // the natural outro at f31.
  { file: 'Clawd-RacingCar.gif',     name: 'racing car',    category: 'persona', loop: [15, 30] },
  { file: 'Clawd-Trumpet.gif',       name: 'trumpet',       category: 'persona' },
  { file: 'Clawd-Basketball.gif',    name: 'basketball',    category: 'persona' },
  { file: 'Clawd-Skateboard.gif',    name: 'skateboard',    category: 'persona' },
  { file: 'Clawd-Soccer.lottie.json', name: 'soccer',       category: 'persona', kind: 'lottie' },
  // Decoded only to locate the sailing scene's loop window — not emitted:
  // its 11 frames are pixel-duplicates of scene frames inside that window,
  // and as a standalone it would start mid-scene with no intro.
  { file: '7bbe5052.gif',            name: 'sailing loop',  category: 'persona', emit: false },
  { file: 'ac0fa108.gif',            name: 'sailing scene', category: 'persona' },
  { file: 'Clawd-Lurking.gif',       name: 'lurking',       category: 'persona' },
];

function safeIdent(s) {
  return s.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
}

function rgb565(r, g, b) {
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// ── PPM (P6) parsing ─────────────────────────────────────────────────────────

function readPpm(file) {
  const buf = fs.readFileSync(file);
  // Header: P6 <w> <h> <maxval> then binary RGB. Tokens separated by whitespace.
  let pos = 0;
  const token = () => {
    while (buf[pos] === 0x23) { while (buf[pos] !== 0x0a) pos++; pos++; } // comment
    while (buf[pos] === 0x20 || buf[pos] === 0x0a || buf[pos] === 0x0d || buf[pos] === 0x09) pos++;
    let s = pos;
    while (pos < buf.length && buf[pos] > 0x20) pos++;
    return buf.toString('ascii', s, pos);
  };
  if (token() !== 'P6') throw new Error(`${file}: not P6`);
  const w = parseInt(token()), h = parseInt(token());
  token(); // maxval
  pos++;   // single whitespace after maxval
  return { w, h, data: buf.subarray(pos, pos + w * h * 3) };
}

// ── GIF decoding via ImageMagick ─────────────────────────────────────────────

function decodeGif(file) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'clawd-'));
  try {
    // Flatten transparency onto a magenta sentinel — the GIFs use transparent
    // holes both around the art AND inside it (the eyes are literally holes),
    // so the sentinel must be a color the art never uses. Interior holes are
    // re-inked in buildAnim.
    execFileSync('convert', [file, '-coalesce', '-background', 'magenta',
      '-alpha', 'remove', '+repage', path.join(tmp, 'f_%04d.ppm')]);
    const delaysCs = execFileSync('identify', ['-format', '%T\n', file])
      .toString().trim().split('\n').map(Number);
    const frames = fs.readdirSync(tmp).filter(f => f.endsWith('.ppm')).sort()
      .map(f => readPpm(path.join(tmp, f)));
    return frames.map((fr, i) => ({ ...fr, holdMs: Math.max((delaysCs[i] || 8) * 10, 20) }));
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

// ── Grid recovery ────────────────────────────────────────────────────────────

const px = (fr, x, y) => {
  const o = (y * fr.w + x) * 3;
  return [fr.data[o], fr.data[o + 1], fr.data[o + 2]];
};
const samePx = (a, b) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];

// Fit the grid phase along one axis: find offset t minimizing the squared
// distance from observed color boundaries to the nearest grid line t + k*pitch.
function fitPhase(boundaries, pitch) {
  let best = 0, bestScore = Infinity;
  for (let t = 0; t < pitch; t += pitch / 200) {
    let score = 0;
    for (const b of boundaries) {
      const m = (b - t) % pitch;
      const d = Math.min(m < 0 ? m + pitch : m, pitch - ((m < 0 ? m + pitch : m)));
      score += d * d;
    }
    if (score < bestScore) { bestScore = score; best = t; }
  }
  return best;
}

function findBoundaries(frames, axis /* 'x'|'y' */) {
  // Union of positions where adjacent columns/rows differ, over a few frames.
  const set = new Set();
  const picks = [0, Math.floor(frames.length / 2), frames.length - 1];
  for (const fi of new Set(picks)) {
    const fr = frames[fi];
    const N = axis === 'x' ? fr.w : fr.h;
    const M = axis === 'x' ? fr.h : fr.w;
    for (let n = 1; n < N; n++) {
      for (let m = 0; m < M; m += 7) {
        const a = axis === 'x' ? px(fr, n - 1, m) : px(fr, m, n - 1);
        const b = axis === 'x' ? px(fr, n, m) : px(fr, m, n);
        if (!samePx(a, b)) { set.add(n); break; }
      }
    }
  }
  return [...set];
}

// Sample every frame at fitted cell centers → per-frame color grids covering
// the full canvas. Returns { grids, cols, rows } with grids[f][row][col] = [r,g,b].
function sampleGrids(frames) {
  const { w, h } = frames[0];
  const pitchX = w / STAGE_W;
  const pitchY = h / STAGE_H;
  const tx = fitPhase(findBoundaries(frames, 'x'), pitchX);
  const ty = fitPhase(findBoundaries(frames, 'y'), pitchY);
  const centers = (t, pitch, limit) => {
    const cs = [];
    for (let k = -1; ; k++) {
      const c = Math.round(t + (k + 0.5) * pitch);
      if (c >= limit) break;
      if (c >= 0) cs.push(c);
    }
    return cs;
  };
  const cxs = centers(tx, pitchX, w);
  const cys = centers(ty, pitchY, h);
  const grids = frames.map(fr =>
    cys.map(cy => cxs.map(cx => px(fr, cx, cy))));
  return { grids, cols: cxs.length, rows: cys.length };
}

// ── Lottie decoding (Laptop / Soccer exports: axis-aligned 50 px rects,
//    per-group opacity keyframes select which frames a group shows in) ───────

function decodeLottie(file) {
  const j = JSON.parse(fs.readFileSync(file, 'utf8'));
  const groups = [];
  for (let li = j.layers.length - 1; li >= 0; li--) {   // bottom-up z-order
    for (const g of j.layers[li].shapes) {
      let color = null, opacityAt = () => 100;
      for (const it of g.it || []) {
        if (it.ty === 'fl') {
          const c = it.c.k;
          color = c.slice(0, 3).map(v => Math.round(v * 255));
          if (it.o) {
            const o = it.o;
            opacityAt = o.a === 0 ? () => o.k : (t) => {
              let v = o.k[0].s ? o.k[0].s[0] : 0;
              for (const kf of o.k) if (t >= kf.t && kf.s) v = kf.s[0];
              return v;
            };
          }
        }
      }
      const rects = [];
      for (const it of g.it || []) {
        if (it.ty !== 'sh') continue;
        const v = it.ks.k.v;
        const xs = v.map(p => p[0]), ys = v.map(p => p[1]);
        rects.push([Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)]);
      }
      if (color && rects.length) groups.push({ color, rects, opacityAt });
    }
  }
  // Grid offset: rect x-coords are ≡ tx (mod 50), y ≡ ty (mod 50). The GIF
  // sampler counts a leading partial cell as column/row 0 whenever the phase
  // exceeds half a cell — mirror that here so Lottie and GIF conversions land
  // on identical stage coordinates (verified: idle Clawd = x 15..38, y 21..36
  // in every animation).
  const tx = ((groups[0].rects[0][0] % 50) + 50) % 50;
  const ty = ((groups[0].rects[0][1] % 50) + 50) % 50;
  const lead = (t) => (t >= 25 ? 1 : 0);
  const grids = [];
  for (let t = 0; t < j.op; t++) {
    const grid = Array.from({ length: STAGE_H }, () =>
      Array.from({ length: STAGE_W }, () => [255, 0, 255]));
    for (const g of groups) {
      if (g.opacityAt(t) <= 50) continue;
      for (const [x1, y1, x2, y2] of g.rects) {
        for (let r = Math.round((y1 - ty) / 50) + lead(ty); r < Math.round((y2 - ty) / 50) + lead(ty); r++)
          for (let c = Math.round((x1 - tx) / 50) + lead(tx); c < Math.round((x2 - tx) / 50) + lead(tx); c++)
            if (r >= 0 && r < STAGE_H && c >= 0 && c < STAGE_W) grid[r][c] = g.color;
      }
    }
    grids.push(grid);
  }
  return { grids, holdsMs: grids.map(() => LOTTIE_HOLD_MS) };
}

// ── Palette building + frame quantization ────────────────────────────────────

// The GIFs draw the eyes as transparent holes (the page shows through on
// claude.ai). We always draw them: any background-colored cell NOT connected
// to the grid border is an interior hole and gets the canonical eye ink
// (#141413, the color the Lottie exports paint the eyes with).
const EYE_INK = [0x14, 0x14, 0x13];

function inkInteriorHoles(grid, bg) {
  const h = grid.length, w = grid[0].length;
  const outside = Array.from({ length: h }, () => new Array(w).fill(false));
  const stack = [];
  const push = (r, c) => {
    if (r < 0 || r >= h || c < 0 || c >= w) return;
    if (outside[r][c]) return;
    if (!samePx(grid[r][c], bg)) return;
    outside[r][c] = true;
    stack.push([r, c]);
  };
  for (let r = 0; r < h; r++) { push(r, 0); push(r, w - 1); }
  for (let c = 0; c < w; c++) { push(0, c); push(h - 1, c); }
  while (stack.length) {
    const [r, c] = stack.pop();
    push(r - 1, c); push(r + 1, c); push(r, c - 1); push(r, c + 1);
  }
  for (let r = 0; r < h; r++)
    for (let c = 0; c < w; c++)
      if (samePx(grid[r][c], bg) && !outside[r][c]) grid[r][c] = EYE_INK;
}

function buildAnim(grids, holdsMs, bg) {
  for (const grid of grids) inkInteriorHoles(grid, bg);
  // Histogram over all sampled colors (excluding background).
  const hist = new Map();
  for (const grid of grids)
    for (const row of grid)
      for (const c of row) {
        if (samePx(c, bg)) continue;
        const key = (c[0] << 16) | (c[1] << 8) | c[2];
        hist.set(key, (hist.get(key) || 0) + 1);
      }
  // Greedy clustering by frequency: AA remnants collapse into the dominant
  // crisp colors they hug. Index 0 is reserved for the background.
  const clusters = [];   // [r,g,b] representatives
  const remap = new Map();
  const dist2 = (a, b) => (a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2;
  for (const [key] of [...hist.entries()].sort((a, b) => b[1] - a[1])) {
    const c = [(key >> 16) & 255, (key >> 8) & 255, key & 255];
    let hit = clusters.findIndex(r => dist2(r, c) <= COLOR_MERGE_DIST2);
    if (hit < 0 && clusters.length < PALETTE_SIZE - 1) { clusters.push(c); hit = clusters.length - 1; }
    if (hit < 0) { // palette full — snap to nearest
      let bi = 0, bd = Infinity;
      clusters.forEach((r, i) => { const d = dist2(r, c); if (d < bd) { bd = d; bi = i; } });
      hit = bi;
    }
    remap.set(key, hit + 1);
  }

  const w0 = grids[0][0].length, h0 = grids[0].length;
  const cellFrames = grids.map(grid => {
    const cells = new Uint8Array(w0 * h0);
    for (let r = 0; r < h0; r++)
      for (let c = 0; c < w0; c++) {
        const col = grid[r][c];
        cells[r * w0 + c] = samePx(col, bg) ? 0
          : remap.get((col[0] << 16) | (col[1] << 8) | col[2]);
      }
    return cells;
  });

  // Union bounding box over all frames, then crop.
  let x0 = w0, y0 = h0, x1 = -1, y1 = -1;
  for (const cells of cellFrames)
    for (let r = 0; r < h0; r++)
      for (let c = 0; c < w0; c++)
        if (cells[r * w0 + c]) {
          if (c < x0) x0 = c; if (c > x1) x1 = c;
          if (r < y0) y0 = r; if (r > y1) y1 = r;
        }
  if (x1 < 0) throw new Error('animation is entirely background');
  const w = x1 - x0 + 1, h = y1 - y0 + 1;
  let frames = cellFrames.map(cells => {
    const out = new Uint8Array(w * h);
    for (let r = 0; r < h; r++)
      for (let c = 0; c < w; c++)
        out[r * w + c] = cells[(r + y0) * w0 + (c + x0)];
    return out;
  });

  // Collapse consecutive identical frames into longer holds.
  let holds = [];
  let kept = [];
  for (let i = 0; i < frames.length; i++) {
    const prev = kept[kept.length - 1];
    if (prev && Buffer.compare(Buffer.from(prev), Buffer.from(frames[i])) === 0) {
      holds[holds.length - 1] += holdsMs[i];
    } else {
      kept.push(frames[i]);
      holds.push(holdsMs[i]);
    }
  }

  // Detect a repeating cycle (the source GIFs play their gait/scene loop
  // several times). Trim the repeats — keep intro + one cycle + outro — and
  // record the loop region so the engine can hold or release it at will.
  const eq = (a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;
  let loop = null;
  {
    let best = null;
    const n = kept.length;
    for (let st = 0; st < n; st++)
      for (let p = 2; st + 2 * p <= n; p++) {
        let k = p;
        while (st + k < n && eq(kept[st + k], kept[st + (k % p)])) k++;
        if (k >= 2 * p) {
          const savings = k - p;
          if (!best || savings > best.savings) best = { st, p, k, savings };
        }
      }
    if (best) {
      kept = kept.slice(0, best.st + best.p).concat(kept.slice(best.st + best.k));
      holds = holds.slice(0, best.st + best.p).concat(holds.slice(best.st + best.k));
      loop = [best.st, best.st + best.p - 1];
    }
  }

  // Index 0 = background: true black (matches THEME_BG). The synthesized
  // #141413 eye ink stays a separate entry, so eyes remain drawn.
  const palette = [[0, 0, 0], ...clusters];
  return { w, h, ox: x0, oy: y0, frames: kept, holds, palette, loop };
}

// ── Verification renders ─────────────────────────────────────────────────────

function writeVerifyPng(anim, ident, dir) {
  const scale = 8;
  const { w, h, frames, palette } = anim;
  const fi = Math.floor(frames.length * 2 / 5);
  const W = w * scale, H = h * scale;
  let ppm = `P6\n${W} ${H}\n255\n`;
  const buf = Buffer.alloc(W * H * 3);
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const c = palette[frames[fi][Math.floor(y / scale) * w + Math.floor(x / scale)]];
      const o = (y * W + x) * 3;
      buf[o] = c[0]; buf[o + 1] = c[1]; buf[o + 2] = c[2];
    }
  const tmp = path.join(dir, `${ident}.ppm`);
  fs.writeFileSync(tmp, Buffer.concat([Buffer.from(ppm), buf]));
  try {
    execFileSync('convert', [tmp, path.join(dir, `${ident}.png`)]);
    fs.rmSync(tmp);
  } catch { /* leave the .ppm if ImageMagick is unavailable */ }
}

// ── Main ─────────────────────────────────────────────────────────────────────

function main() {
  if (VERIFY_DIR) fs.mkdirSync(VERIFY_DIR, { recursive: true });

  let out = '';
  out += '// ============================================================\n';
  out += '// Splash animations — generated by tools/convert_official_clawd.js.\n';
  out += '// Source: official Anthropic Clawd animations, archived in\n';
  out += '// research/clawd-official/ (see its CLAUDE.md for provenance).\n';
  out += '// Frames are bounding-box crops on the shared 55x37 art stage;\n';
  out += '// ox/oy give the crop origin in stage cells.\n';
  out += '// Do not edit by hand — re-run the converter to refresh.\n';
  out += '// ============================================================\n';
  out += '#pragma once\n#include <stdint.h>\n\n';
  out += `#define SPLASH_PALETTE_SIZE ${PALETTE_SIZE}\n\n`;
  out += 'typedef struct {\n';
  out += '    const char *name;\n';
  out += '    const char *category;      // "core" | "persona"\n';
  out += '    uint8_t  w, h;             // frame size, cells\n';
  out += '    uint8_t  ox, oy;           // crop origin on the 55x37 stage\n';
  out += '    uint16_t frame_count;\n';
  out += '    uint16_t loop_start, loop_end; // loopable region (defaults to whole file)\n';
  out += '    uint8_t  palette_count;\n';
  out += '    const uint16_t *palette;   // RGB565; index 0 = background\n';
  out += '    const uint8_t  *frames;    // frame_count x (w*h) cells\n';
  out += '    const uint16_t *holds;     // ms per frame\n';
  out += '} splash_anim_def_t;\n\n';

  const entries = [];
  let totalBytes = 0;

  for (const meta of ANIMS) {
    const src = path.join(IN_DIR, meta.file);
    if (!fs.existsSync(src)) {
      console.error(`missing ${src} — run research/clawd-official/fetch.sh`);
      process.exit(1);
    }
    let anim;
    if (meta.kind === 'lottie') {
      const { grids, holdsMs } = decodeLottie(src);
      anim = buildAnim(grids, holdsMs, [255, 0, 255]);
    } else {
      const frames = decodeGif(src);
      const { grids } = sampleGrids(frames);
      const bg = grids[0][0][0];  // top-left cell of frame 0 is background
      anim = buildAnim(grids, frames.map(f => f.holdMs), bg);
    }

    const ident = safeIdent(meta.name);
    entries.push({ ident, meta, anim });
  }

  // The sailing scene's loop region is defined by the standalone sailing-loop
  // asset (its frames are an extract of the scene's middle). Match on resolved
  // colors — the two files quantize to separately-ordered palettes.
  {
    const get = (n) => { const e = entries.find(e => e.meta.name === n); return e ? e.anim : null; };
    const sl = get('sailing loop'), sc = get('sailing scene');
    if (sl && sc && sl.w === sc.w && sl.h === sc.h) {
      const resolve = (a, f) => Array.from(a.frames[f]).map(i => a.palette[i].join()).join(';');
      for (let i = 0; i + sl.frames.length <= sc.frames.length && !sc.loop; i++) {
        let all = true;
        for (let j = 0; j < sl.frames.length; j++)
          if (resolve(sc, i + j) !== resolve(sl, j)) { all = false; break; }
        if (all) sc.loop = [i, i + sl.frames.length - 1];
      }
      if (!sc.loop) console.warn('warning: sailing loop not found inside sailing scene');
    }
  }
  for (const { meta, anim } of entries) {
    if (!anim.loop && meta.loop) anim.loop = meta.loop;
    if (!anim.loop) anim.loop = [0, anim.frames.length - 1];
    console.log(`  loop ${meta.name.padEnd(14)} [${anim.loop[0]}..${anim.loop[1]}] of ${anim.frames.length}`);
  }

  // ── Contrast recolors (user-directed) ─────────────────────────────────────
  // The #141413 ink doubles as detail color in two animations where it has no
  // contrast against the dark splash ground. Component analysis separates the
  // features from same-colored details that must keep their authored color.
  {
    const dist2 = (a, b) => (a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2;
    const palIdx = (anim, rgb, tol = 900) => {
      let bi = -1, bd = Infinity;
      anim.palette.forEach((c, i) => { const d = dist2(c, rgb); if (d < bd) { bd = d; bi = i; } });
      return bd <= tol ? bi : -1;
    };
    const addColor = (anim, rgb) => {
      if (anim.palette.length >= PALETTE_SIZE)
        throw new Error(`palette full (${PALETTE_SIZE}) — cannot add recolor entry`);
      anim.palette.push(rgb);
      return anim.palette.length - 1;
    };
    // Yields { cells, neighbors:Set<idx>, border:bool } per connected
    // component of `idx` cells in one frame.
    function* components(f, W, H, idx) {
      const seen = new Array(W * H).fill(false);
      for (let i = 0; i < W * H; i++) {
        if (f[i] !== idx || seen[i]) continue;
        const cells = [], neighbors = new Set(), stack = [i];
        let border = false;
        seen[i] = true;
        while (stack.length) {
          const j = stack.pop(); cells.push(j);
          const r = (j / W) | 0, c = j % W;
          for (const [dr, dc] of [[1,0],[-1,0],[0,1],[0,-1]]) {
            const nr = r + dr, nc = c + dc;
            if (nr < 0 || nr >= H || nc < 0 || nc >= W) { border = true; continue; }
            const k = nr * W + nc;
            if (f[k] === idx) { if (!seen[k]) { seen[k] = true; stack.push(k); } }
            else neighbors.add(f[k]);
          }
        }
        yield { cells, neighbors, border };
      }
    }
    const get = (n) => { const e = entries.find(e => e.meta.name === n); return e ? e.anim : null; };

    // Trumpet: floating #141413 components are the music notes → ivory
    // #F9F8F4. Components touching the body (eyes, valve detail) keep ink.
    const tr = get('trumpet');
    if (tr) {
      const ink = palIdx(tr, EYE_INK);
      const ivory = addColor(tr, [0xF9, 0xF8, 0xF4]);
      for (const f of tr.frames)
        for (const comp of components(f, tr.w, tr.h, ink))
          if (![...comp.neighbors].some(n => n !== 0))
            for (const j of comp.cells) f[j] = ivory;
    }

    // Magnifier: the fedora (#141413) → gray #8B8B8B — except the lens
    // interior, which is the same ink but enclosed by the monocle rim: it
    // touches the rim and never the background.
    const mg = get('magnifier');
    if (mg) {
      const ink = palIdx(mg, EYE_INK);
      const rim = palIdx(mg, [0x3E, 0x38, 0x7D]);
      const gray = addColor(mg, [0x8B, 0x8B, 0x8B]);
      for (const f of mg.frames)
        for (const comp of components(f, mg.w, mg.h, ink)) {
          const lens = rim >= 0 && comp.neighbors.has(rim) &&
                       !comp.neighbors.has(0) && !comp.border;
          if (!lens) for (const j of comp.cells) f[j] = gray;
        }
    }
  }

  const emitted = entries.filter(e => e.meta.emit !== false);
  for (const { ident, meta, anim } of emitted) {
    const cellBytes = anim.frames.length * anim.w * anim.h;
    totalBytes += cellBytes;
    console.log(`${meta.name.padEnd(14)} ${String(anim.w).padStart(2)}x${String(anim.h).padEnd(2)} ` +
      `${String(anim.frames.length).padStart(3)} frames (${meta.kind === 'lottie' ? 'lottie' : 'gif'}, ` +
      `${anim.palette.length} colors, ${(cellBytes / 1024).toFixed(1)} KB)`);

    if (anim.palette.length > PALETTE_SIZE)
      throw new Error(`${meta.name}: ${anim.palette.length} palette entries exceed PALETTE_SIZE`);
    const pal565 = new Array(PALETTE_SIZE).fill(0);
    anim.palette.forEach((c, i) => { pal565[i] = rgb565(c[0], c[1], c[2]); });
    out += `static const uint16_t splash_${ident}_palette[${PALETTE_SIZE}] = {`;
    out += pal565.map(c => `0x${c.toString(16).toUpperCase().padStart(4, '0')}`).join(',');
    out += '};\n';

    out += `static const uint8_t splash_${ident}_frames[${cellBytes}] = {\n`;
    for (const f of anim.frames) out += '    ' + Array.from(f).join(',') + ',\n';
    out += '};\n';

    out += `static const uint16_t splash_${ident}_holds[${anim.frames.length}] = {`;
    out += anim.holds.join(',');
    out += '};\n\n';

    if (VERIFY_DIR) writeVerifyPng(anim, ident, VERIFY_DIR);
  }

  out += `#define SPLASH_ANIM_COUNT ${emitted.length}\n`;
  out += 'static const splash_anim_def_t splash_anims[SPLASH_ANIM_COUNT] = {\n';
  for (const { ident, meta, anim } of emitted) {
    out += `    {"${meta.name}", "${meta.category}", ${anim.w}, ${anim.h}, ` +
      `${anim.ox}, ${anim.oy}, ${anim.frames.length}, ${anim.loop[0]}, ${anim.loop[1]}, ` +
      `${anim.palette.length}, ` +
      `splash_${ident}_palette, splash_${ident}_frames, splash_${ident}_holds},\n`;
  }
  out += '};\n';

  fs.writeFileSync(OUT_FILE, out);
  console.log(`\nWrote ${OUT_FILE} (${emitted.length} animations, ` +
    `${(totalBytes / 1024).toFixed(0)} KB cell data, ${(out.length / 1024).toFixed(0)} KB source)`);
}

main();
