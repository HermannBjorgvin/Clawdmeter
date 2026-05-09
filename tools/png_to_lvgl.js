#!/usr/bin/env node
// Convert a PNG with alpha channel to an LVGL RGB565A8 C array.
// Layout: w*h RGB565 pixels (little-endian) followed by w*h alpha bytes.
// Usage: node png_to_lvgl.js <input.png> <symbol_name> [W_MACRO] [H_MACRO] [--tint=RRGGBB]
// Default tint=FFFFFF — Lucide PNGs are black-on-transparent, so without a tint
// the icon would render as invisible black; white is the typical choice for UI overlays.

const fs = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

function rgb565(r, g, b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

function convert(pngPath, symbol, wMacro, hMacro, tint) {
    const buf = fs.readFileSync(pngPath);
    const png = PNG.sync.read(buf);
    const w = png.width, h = png.height;
    const total = w * h;

    const colorBytes = Buffer.alloc(total * 2);
    const alphaBytes = Buffer.alloc(total);

    const tr = tint ? (tint >> 16) & 0xFF : null;
    const tg = tint ? (tint >> 8) & 0xFF : null;
    const tb = tint ? tint & 0xFF : null;

    for (let i = 0; i < total; i++) {
        const r = tint !== null ? tr : png.data[i * 4 + 0];
        const g = tint !== null ? tg : png.data[i * 4 + 1];
        const b = tint !== null ? tb : png.data[i * 4 + 2];
        const a = png.data[i * 4 + 3];
        const c = rgb565(r, g, b);
        colorBytes[i * 2 + 0] = c & 0xFF;
        colorBytes[i * 2 + 1] = (c >> 8) & 0xFF;
        alphaBytes[i] = a;
    }

    const all = Buffer.concat([colorBytes, alphaBytes]);

    let out = `#define ${wMacro} ${w}\n#define ${hMacro} ${h}\n`;
    out += `// RGB565A8: ${total} RGB565 pixels (little-endian) followed by ${total} alpha bytes\n`;
    out += `static const uint8_t ${symbol}[${all.length}] = {\n    `;
    const lines = [];
    for (let i = 0; i < all.length; i += 16) {
        const row = [];
        for (let j = 0; j < 16 && i + j < all.length; j++) {
            row.push('0x' + all[i + j].toString(16).padStart(2, '0').toUpperCase());
        }
        lines.push(row.join(', '));
    }
    out += lines.join(',\n    ');
    out += '\n};\n';
    return out;
}

if (require.main === module) {
    const args = process.argv.slice(2);
    let tint = 0xFFFFFF;
    const positional = [];
    for (const a of args) {
        if (a.startsWith('--tint=')) tint = parseInt(a.slice(7), 16);
        else if (a === '--no-tint') tint = null;
        else positional.push(a);
    }
    const [pngPath, symbol, wMacro, hMacro] = positional;
    if (!pngPath || !symbol) {
        console.error('Usage: node png_to_lvgl.js <input.png> <symbol_name> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]');
        process.exit(1);
    }
    const base = symbol.toUpperCase().replace(/_DATA$/, '');
    process.stdout.write(convert(pngPath, symbol, wMacro || `${base}_W`, hMacro || `${base}_H`, tint));
}

module.exports = { convert, rgb565 };
