#!/usr/bin/env node
/**
 * Scrapes animations from claudepix.vercel.app.
 * Handles two on-site formats:
 *   1) PRESET via creature-engine.js (idle_*, expression_*, work_*, dance_bounce, dance_sway)
 *   2) Standalone with window.FRAMES + window.PAL (dance_*_dj, dance_djmix)
 *
 * Output: tools/claudepix_data/<name>.json with shape:
 *   { filename, name, category, description, palette: ['#RRGGBB',...],
 *     frame_count, frames: [{ hold, grid: number[20][20] }] }
 *
 * Usage: node scrape_claudepix.js [--base URL] [--out DIR]
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const args = process.argv.slice(2);
const opt = (k, def) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : def; };

const BASE = opt('--base', 'https://claudepix.vercel.app');

// Served but absent from the site's MANIFEST — see the note where they're merged in.
const UNLISTED = ['dance_bob.html', 'work_type.html'];
const OUT_DIR = path.resolve(opt('--out', path.join(__dirname, 'claudepix_data')));

async function fetchText(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} fetching ${url}`);
  return res.text();
}

// Build a sandbox that stubs the DOM so animations that touch it don't crash.
function makeSandbox() {
  const fakeStyle = new Proxy({}, { get: () => '', set: () => true });
  const fakeEl = {
    style: fakeStyle,
    appendChild: () => fakeEl,
    addEventListener: () => {},
    innerHTML: '',
    children: [],
  };
  const sandbox = {
    window: {},
    document: {
      getElementById: () => fakeEl,
      createElement: () => ({ ...fakeEl, style: new Proxy({}, { get: () => '', set: () => true }) }),
    },
    requestAnimationFrame: () => 0,
    cancelAnimationFrame: () => {},
    performance: { now: () => 0 },
    console,
  };
  sandbox.window.addEventListener = () => {};
  return sandbox;
}

// Default creature-engine palette (CSS colors used by mount())
const ENGINE_PALETTE = ['transparent', '#CD7F6A', '#0f0f0f'];

// Derive category from filename prefix (matches site logic)
function deriveCategory(filename) {
  const base = filename.replace(/\.html?$/i, '');
  const prefix = base.split('_')[0];
  if (!base.includes('_')) return 'Idle';
  const aliases = { idle: 'Idle', expression: 'Expressions', dance: 'Dance', work: 'Work', code: 'Coding', coding: 'Coding' };
  return aliases[prefix] || (prefix.charAt(0).toUpperCase() + prefix.slice(1));
}
function displayName(filename) {
  return filename.replace(/\.html?$/i, '').replace(/_/g, ' ');
}

async function main() {
  fs.mkdirSync(OUT_DIR, { recursive: true });

  console.log(`Fetching manifest from ${BASE}/app.js`);
  const appJs = await fetchText(`${BASE}/app.js`);
  const manifestMatch = appJs.match(/const\s+MANIFEST\s*=\s*(\[[^\]]+\])/);
  if (!manifestMatch) throw new Error('MANIFEST not found in app.js');
  const manifest = eval(manifestMatch[1]);
  console.log(`Found ${manifest.length} animations in the site manifest`);

  // The site's MANIFEST doesn't list everything it serves. The source repo
  // (github.com/amaancoderx/claudepix) carries these two as well, and both are
  // live at /animations/<name>.html — they're just missing from the gallery
  // index, so a manifest-only scrape silently skips them.
  for (const extra of UNLISTED) {
    if (!manifest.includes(extra)) manifest.push(extra);
  }
  console.log(`Scraping ${manifest.length} (incl. ${UNLISTED.length} unlisted)`);

  console.log(`Fetching engine`);
  const engineJs = await fetchText(`${BASE}/animations/creature-engine.js`);

  const results = [];

  for (const filename of manifest) {
    process.stdout.write(`  ${filename} ... `);
    let html;
    try { html = await fetchText(`${BASE}/animations/${filename}`); }
    catch (e) { console.log(`FETCH ERR: ${e.message}`); continue; }

    const scripts = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)]
      .map(m => m[1]).filter(s => s.trim().length > 50);
    if (scripts.length === 0) { console.log('SKIP (no scripts)'); continue; }
    const animScript = scripts[scripts.length - 1];

    const sandbox = makeSandbox();
    vm.createContext(sandbox);

    const isStandalone = animScript.includes('STANDALONE') || animScript.includes('window.FRAMES');

    try {
      if (!isStandalone) {
        vm.runInContext(engineJs, sandbox);
        sandbox.window.PixelEngine.mount = () => ({});
      }
      vm.runInContext(animScript, sandbox);
    } catch (e) {
      console.log(`ERR: ${e.message}`);
      continue;
    }

    let resolved, palette, name, category, description;

    if (sandbox.window.PRESET) {
      const preset = sandbox.window.PRESET;
      const CREATURE = sandbox.window.PixelEngine.CREATURE;
      resolved = preset.frames.map(f => ({
        hold: f.hold,
        grid: f.frame ? f.frame : CREATURE,
      }));
      palette = ENGINE_PALETTE;
      // Most presets carry a spaced display name, but the two unlisted ones
      // fall back to their raw key ("dance_bob"). Normalize so every animation
      // is addressable the same way — splash.cpp matches these by literal
      // string, so an inconsistent name is a silently unreachable animation.
      name = displayName(preset.name);
      category = preset.category;
      description = preset.description || '';
    } else if (sandbox.window.FRAMES) {
      resolved = sandbox.window.FRAMES.map(f => ({ hold: f.hold, grid: f.frame }));
      palette = sandbox.window.PAL || ENGINE_PALETTE;
      // Standalone files don't expose PRESET metadata — derive from filename
      name = displayName(filename);
      category = deriveCategory(filename);
      description = '';
    } else {
      console.log('SKIP (no PRESET or FRAMES)');
      continue;
    }

    const out = {
      filename,
      name,
      category,
      description,
      palette,
      frame_count: resolved.length,
      frames: resolved,
    };

    const outPath = path.join(OUT_DIR, filename.replace(/\.html?$/, '.json'));
    fs.writeFileSync(outPath, JSON.stringify(out, null, 2));
    console.log(`${resolved.length} frames, ${palette.length}-color palette`);
    results.push(out);
  }

  fs.writeFileSync(
    path.join(OUT_DIR, '_index.json'),
    JSON.stringify(results.map(r => ({
      filename: r.filename, name: r.name, category: r.category,
      frame_count: r.frame_count, palette_size: r.palette.length,
    })), null, 2)
  );

  console.log(`\nDone. ${results.length}/${manifest.length} animations saved.`);
}

main().catch(e => { console.error(e); process.exit(1); });
