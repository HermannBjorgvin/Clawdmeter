#!/usr/bin/env node
/**
 * Export a frame of an existing animation as a makebead editor file, so it can
 * be opened and drawn over instead of redrawn from scratch.
 *
 * The point is alignment. The creature's proportions carry meaning that isn't
 * obvious by eye — the eyes sit at specific columns, the body is a specific
 * height, and there is deliberate empty space below him that the animations
 * use to bob. A new creature drawn freehand tends to land close but not equal,
 * and then reads as a slightly different character in the rotation.
 *
 *   node anim_to_makebead.js tools/claudepix_data/idle_breathe.json 0 out.json
 *
 * Arguments: <animation.json> [frame index, default 0] [output, default stdout]
 */

const fs = require('fs');

const [src, frameArg, out] = process.argv.slice(2);
if (!src) {
  console.error('usage: anim_to_makebead.js <animation.json> [frame] [out.json]');
  process.exit(1);
}

const anim = JSON.parse(fs.readFileSync(src, 'utf8'));
const idx = parseInt(frameArg || '0', 10);
const frame = anim.frames[idx];
if (!frame) {
  console.error(`${src} has ${anim.frames.length} frames; no frame ${idx}`);
  process.exit(1);
}

// makebead keys cells into a dict; our slot 0 is "empty", which is null there.
// Every other palette entry becomes a dict entry, in palette order, so the
// indices stay recognisable when the file comes back.
const dict = [];
const slotToDict = new Map();
anim.palette.forEach((hex, slot) => {
  if (slot === 0 || hex === 'transparent') return;
  slotToDict.set(slot, dict.length);
  const h = hex.toUpperCase();
  dict.push({
    id: `px_${slot}`,
    brand: 'Digital',
    code: h,
    name: `${anim.name} ${slot}`,
    hex: h,
    rgb: [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16)),
    category: 'imported',
    available: true,
  });
});

const cells = [];
for (const row of frame.grid) {
  for (const v of row) cells.push(v === 0 ? null : (slotToDict.get(v) ?? null));
}

const doc = {
  v: 4,
  beadStyle: 'square',
  paletteId: 'pixel-art-256',
  size: [20, 20],
  dict,
  cells,
};

const json = JSON.stringify(doc);
if (out) {
  fs.writeFileSync(out, json);
  console.error(`Wrote ${out}  (${anim.name} frame ${idx}, ${dict.length} colours)`);
} else {
  process.stdout.write(json + '\n');
}
