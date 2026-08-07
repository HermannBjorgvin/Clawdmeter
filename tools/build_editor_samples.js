#!/usr/bin/env node
/**
 * Embeds every animation in the catalog into tools/anim_editor.html, so an
 * existing one can be opened and fixed rather than rebuilt from scratch.
 *
 * The data goes inline rather than being fetched. The editor is opened straight
 * off disk (file://), where fetch/XHR of a sibling file is blocked — inlining
 * is what keeps it a single file that works with no server.
 *
 * Only the region between the BEGIN/END markers is rewritten; the rest of the
 * editor is hand-written and must survive this.
 *
 *   node tools/build_editor_samples.js
 *
 * Re-run it after adding or changing any animation.
 */

const fs = require('fs');
const path = require('path');

const SRC_DIRS = ['claudepix_data', 'drawn_anims']
  .map(d => path.join(__dirname, d));
const EDITOR = path.join(__dirname, 'anim_editor.html');
const BEGIN = '/*BEGIN-SAMPLES*/', END = '/*END-SAMPLES*/';

// The same remap convert_to_c.js applies on the way to C. Loading the raw
// source colour instead would preview a body colour the panel never shows.
const TINT = { '#CD7F6A': '#D97757' };

// Keyed by name, later directories winning — the same override rule
// convert_to_c.js applies. Without it the editor would offer two entries with
// the same name and no way to tell which one is the one actually on the device.
const byName = new Map();

for (const dir of SRC_DIRS) {
  if (!fs.existsSync(dir)) continue;
  for (const file of fs.readdirSync(dir).sort()) {
    if (!file.endsWith('.json') || file.startsWith('_')) continue;
    const a = JSON.parse(fs.readFileSync(path.join(dir, file), 'utf8'));
    if (!a.frames || !a.palette) continue;

    byName.set(a.name, {
      n: a.name,
      c: a.category || 'Idle',
      p: a.palette.map(h => h === 'transparent' ? 'transparent'
                                                : (TINT[h.toUpperCase()] || h.toUpperCase())),
      // One string per frame: 400 chars, '.' for empty. Indices are 0-9
      // because the palette can't exceed 10 entries, so a digit per cell is
      // lossless and roughly a third the size of a nested JSON array.
      f: a.frames.map(fr => ({
        h: fr.hold,
        g: fr.grid.map(row => row.map(v => v === 0 ? '.' : v).join('')).join(''),
      })),
    });
  }
}

const anims = [...byName.values()];
anims.sort((x, y) => x.c.localeCompare(y.c) || x.n.localeCompare(y.n));

const html = fs.readFileSync(EDITOR, 'utf8');
const i = html.indexOf(BEGIN), j = html.indexOf(END);
if (i < 0 || j < 0) {
  console.error(`Markers not found in ${EDITOR}; nothing was written.`);
  process.exit(1);
}

const payload = '[\n' + anims.map(a =>
  `{n:${JSON.stringify(a.n)},c:${JSON.stringify(a.c)},p:${JSON.stringify(a.p)},f:[` +
  a.f.map(f => `{h:${f.h},g:${JSON.stringify(f.g)}}`).join(',') + ']}'
).join(',\n') + '\n]';

const out = html.slice(0, i + BEGIN.length) + payload + html.slice(j);
fs.writeFileSync(EDITOR, out);

const frames = anims.reduce((s, a) => s + a.f.length, 0);
console.log(`Embedded ${anims.length} animations (${frames} frames) into ${path.basename(EDITOR)}`);
console.log(`  ${(out.length / 1024).toFixed(0)} KB total`);
