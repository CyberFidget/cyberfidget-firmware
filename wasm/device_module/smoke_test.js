// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Node smoke-test harness for the device wasm app modules (spike/wasm3-apps).
// Mocks the "cf" import surface (see cf_hal_imports.h), runs a module through
// begin -> scripted frames + button events -> back-button exit, and asserts
// per-app expectations (pixels drawn, LEDs lit, exit_to_menu requested, ...).
//
// Usage:
//   node smoke_test.js                    # run every app module
//   node smoke_test.js breakout.wasm     # run one module
//
// Button indices (lib/HAL): 0=TopLeft/Up 1=TopRight/Down 2=MiddleLeft/Left
// 3=MiddleRight/Right 4=BottomLeft/Back 5=BottomRight/Enter.
// Event types (lib/ButtonManager): 1=Pressed 2=Released 3=Held.

"use strict";

const fs = require("fs");
const path = require("path");

// Deterministic PRNG so failures reproduce.
function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0; a = (a + 0x6d2b79f5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function makeHost() {
    const host = {
        simMs: 1000,          // pretend the device has been up 1 s
        rand: mulberry32(0xC0FFEE),
        counts: {},           // import name -> call count
        exitRequested: false,
        logs: [],
        strings: [],          // decoded draw_string payloads
        xbmPtrs: new Set(),   // distinct bitmap pointers blitted
        memory: null,         // set after instantiate
    };
    const bump = (k) => { host.counts[k] = (host.counts[k] || 0) + 1; };
    const mem = () => new Uint8Array(host.memory.buffer);
    const readStr = (ptr, len) =>
        Buffer.from(host.memory.buffer, ptr, len).toString("utf8");

    const cf = {
        nop: (x) => { bump("nop"); return x + 1; },
        display_clear: () => bump("display_clear"),
        display_show: () => bump("display_show"),
        display_set_color: (_c) => bump("display_set_color"),
        display_set_pixel: (_x, _y) => bump("display_set_pixel"),
        display_draw_line: () => bump("display_draw_line"),
        display_draw_rect: () => bump("display_draw_rect"),
        display_fill_rect: () => bump("display_fill_rect"),
        display_draw_circle: () => bump("display_draw_circle"),
        display_fill_circle: () => bump("display_fill_circle"),
        display_draw_hline: () => bump("display_draw_hline"),
        display_draw_vline: () => bump("display_draw_vline"),
        display_draw_triangle: () => bump("display_draw_triangle"),
        display_set_align: (_a) => bump("display_set_align"),
        display_set_font: (_f) => bump("display_set_font"),
        display_draw_string: (x, y, ptr, len) => {
            bump("display_draw_string");
            host.strings.push(readStr(ptr, len));
        },
        display_string_width: (ptr, len) => { bump("display_string_width"); return len * 6; },
        display_draw_xbm: (x, y, w, h, ptr) => {
            bump("display_draw_xbm");
            const byteLen = ((w + 7) >> 3) * h;
            if (ptr < 0 || ptr + byteLen > host.memory.buffer.byteLength) {
                throw new Error(`draw_xbm out of bounds: ptr=${ptr} len=${byteLen}`);
            }
            host.xbmPtrs.add(ptr);
        },
        led_set: (i, r, g, b, w) => {
            bump("led_set");
            if (r || g || b || w) bump("led_set_nonzero");
        },
        led_all_off: () => bump("led_all_off"),
        tone_play: () => bump("tone_play"),
        tone_stop: () => bump("tone_stop"),
        seq_play: () => bump("seq_play"),
        seq_stop: () => bump("seq_stop"),
        millis: () => host.simMs >>> 0,
        random: (min, max) => {
            bump("random");
            if (max <= min) return min;
            return min + Math.floor(host.rand() * (max - min));
        },
        slider_pct: () => 50.0,
        accel_x: () => 0.2,
        accel_y: () => 9.6,
        accel_z: () => 0.3,
        exit_to_menu: () => { bump("exit_to_menu"); host.exitRequested = true; },
        keepalive: () => bump("keepalive"),
        log: (ptr, len) => { bump("log"); host.logs.push(readStr(ptr, len)); },
    };

    const wasi = {
        proc_exit: (code) => { throw new Error(`proc_exit(${code})`); },
        fd_write: () => 8,   // EBADF, matches device stubs
        fd_close: () => 8,
        fd_seek: () => 8,
        environ_sizes_get: (countPtr, sizePtr) => {
            const dv = new DataView(host.memory.buffer);
            dv.setUint32(countPtr, 0, true);
            dv.setUint32(sizePtr, 0, true);
            return 0;
        },
        environ_get: () => 0,
        clock_time_get: (_id, _prec, outPtr) => {
            const dv = new DataView(host.memory.buffer);
            dv.setBigUint64(outPtr, BigInt(host.simMs) * 1000000n, true);
            return 0;
        },
        random_get: (ptr, len) => {
            const m = mem();
            for (let i = 0; i < len; i++) m[ptr + i] = Math.floor(host.rand() * 256);
            return 0;
        },
    };

    const env = { emscripten_notify_memory_growth: (_idx) => bump("memory_growth") };

    host.imports = { cf, wasi_snapshot_preview1: wasi, env };
    return host;
}

// Per-app scripts. Each returns extra assertions to run after the generic
// run; the generic run itself is: _initialize -> app_begin -> frames with
// scripted buttons -> back-button release -> app_end.
const APPS = {
    "reaction.wasm": {
        frames: 150, stepMs: 20,
        // Enter (5) starts a round; wait through the random 1-5 s delay.
        buttons: [{ at: 5, idx: 5, evs: [1, 2] }, { at: 6, idx: 5, evs: [1, 2] }],
        extraFrames: 300,
        assert: (h, c) => {
            if (!c.display_draw_string) throw new Error("no text drawn");
        },
    },
    "breakout.wasm": {
        // Regression for the level-skip crash path (2026-07 hardware
        // playtest): BottomRight during play clears the level; next tick
        // checkVictory() advances. Skip all 9 levels to the victory screen
        // (String float formatting every frame) and restart once.
        frames: 320, stepMs: 20,
        buttons: [
            { at: 5, idx: 5, evs: [1, 2] },   // Enter: menu -> game
            ...Array.from({ length: 9 }, (_, i) => ({ at: 30 + i * 20, idx: 5, evs: [1, 2] })),
            { at: 260, idx: 5, evs: [1, 2] }, // restart from victory screen
        ],
        assert: (h, c) => {
            if (!c.display_fill_rect && !c.display_draw_rect) throw new Error("no bricks drawn");
            if (!c.seq_play) throw new Error("victory jingle never played (skips did not reach victory)");
            if (!h.strings.some((s) => s.includes("YOU WIN"))) throw new Error("victory screen missing");
        },
    },
    "starburst.wasm": {
        frames: 150, stepMs: 20,
        buttons: [
            { at: 3, idx: 5, evs: [1, 2] },   // Enter: confirm Manual on mode select
            { at: 10, idx: 0, evs: [1, 2] },  // Up: spawn palm burst
            { at: 20, idx: 1, evs: [1, 2] },  // Down: spawn ring burst
            { at: 30, idx: 2, evs: [1, 2] },  // Left: cross
            { at: 40, idx: 3, evs: [1, 2] },  // Right: spray
            { at: 50, idx: 5, evs: [1, 2] },  // Enter (running): star
        ],
        assert: (h, c) => {
            if (!h.strings.some((s) => s.includes("Starburst"))) throw new Error("mode-select title missing");
            if (!c.display_set_pixel) throw new Error("no burst particles drawn");
            if (!c.led_set_nonzero) throw new Error("burst LEDs never lit");
        },
    },
    // T-114 asteroid shooter: opens on a mode chooser (Play Now/Screensaver/
    // Auto); Fire (5) confirms, Left/Right steer, Fire auto-repeats lasers.
    "spaceship.wasm": {
        frames: 200, stepMs: 20,
        buttons: [
            { at: 5, idx: 5, evs: [1, 2] },    // Fire: confirm "Play Now"
            { at: 10, idx: 2, evs: [1] },      // hold Left (steer)...
            { at: 60, idx: 2, evs: [2] },      // ...release
            { at: 70, idx: 5, evs: [1] },      // hold Fire: lasers auto-repeat
            { at: 150, idx: 5, evs: [2] },     // release Fire
        ],
        assert: (h, c) => {
            if (!h.strings.some((s) => s.includes("Spaceship"))) throw new Error("chooser title missing");
            if (!c.display_draw_line) throw new Error("no starfield/ship lines drawn");
            if (!c.led_set_nonzero) throw new Error("engine glow LEDs never lit");
            if (!c.display_draw_string) throw new Error("HUD text missing");
        },
    },
    "sphfluid.wasm": {
        frames: 100, stepMs: 20,
        buttons: [],
        assert: (h, c) => {
            if (!c.display_set_pixel) throw new Error("no particles drawn");
            if (c.display_show < 50) throw new Error("render loop stalled");
        },
    },
    "splooty.wasm": {
        // frameSpeed=376 ms and one animation frame advances per elapsed
        // period: 34 frames need >=12,784 ms, so 160 updates x 100 ms.
        frames: 160, stepMs: 100,
        buttons: [],
        assert: (h, c) => {
            if (!c.display_draw_xbm) throw new Error("no frames blitted");
            if (h.xbmPtrs.size < 34) throw new Error(`expected 34 distinct frames, saw ${h.xbmPtrs.size}`);
        },
    },
};

async function runApp(file, spec) {
    const host = makeHost();
    const bytes = fs.readFileSync(path.join(__dirname, file));
    const { instance } = await WebAssembly.instantiate(bytes, host.imports);
    const ex = instance.exports;
    host.memory = ex.memory;
    if (ex._initialize) ex._initialize();

    ex.app_begin();
    const totalFrames = spec.frames + (spec.extraFrames || 0);
    for (let f = 0; f < totalFrames; f++) {
        for (const b of spec.buttons) {
            if (b.at === f) for (const ev of b.evs) ex.app_handle_button(b.idx, ev);
        }
        ex.app_update(); // eslint-disable-line no-await-in-loop
        host.simMs += spec.stepMs;
    }
    // Back button (BottomLeft=4) release must request exit for every app.
    ex.app_handle_button(4, 2);
    if (!host.exitRequested) throw new Error("back button did not request exit_to_menu");
    ex.app_end();

    spec.assert(host, host.counts);

    const c = host.counts;
    const summary = Object.keys(c).sort().map((k) => `${k}=${c[k]}`).join(" ");
    console.log(`PASS ${file}\n     ${summary}`);
}

(async () => {
    const only = process.argv[2];
    const files = only ? [only] : Object.keys(APPS);
    let failed = 0;
    for (const f of files) {
        const spec = APPS[path.basename(f)];
        if (!spec) { console.error(`SKIP ${f}: no script defined`); continue; }
        try {
            await runApp(path.basename(f), spec);
        } catch (err) {
            failed++;
            console.error(`FAIL ${f}: ${err.message}`);
        }
    }
    process.exit(failed ? 1 : 0);
})();
