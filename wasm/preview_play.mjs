// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the Spaceship app through its modes and captures canvas screenshots.
//   node preview_play.mjs http://127.0.0.1:8011/preview.html
import { createRequire } from 'module';
import { fileURLToPath } from 'url';
const require = createRequire('d:/_Steele/Code_Sandbox/cyberfidget/cyberfidget_website/package.json');
const { chromium } = require('playwright');

const url = process.argv[2] || 'http://127.0.0.1:8011/preview.html';
const here = (n) => fileURLToPath(new URL('./' + n, import.meta.url));
const errors = [];

const browser = await chromium.launch();
const page = await browser.newPage();
page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
page.on('pageerror', e => errors.push('pageerror: ' + e.message));

async function shot(name) { await page.locator('#oled').screenshot({ path: here(name) }); }
async function litPx() {
  return page.evaluate(() => {
    const c = document.getElementById('oled'), g = c.getContext('2d');
    const d = g.getImageData(0, 0, c.width, c.height).data; let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] || d[i+1] || d[i+2]) n++;
    return n;
  });
}
// trailing gap keeps consecutive menu presses outside the ~20ms button debounce
async function tap(key, ms = 60) { await page.keyboard.down(key); await page.waitForTimeout(ms); await page.keyboard.up(key); await page.waitForTimeout(150); }

await page.goto(url, { waitUntil: 'load' });
for (let i = 0; i < 40 && !/running/i.test(await page.$eval('#status', e => e.textContent)); i++) await page.waitForTimeout(200);
await page.waitForTimeout(500);

// 1) Mode chooser
await shot('shot_1_menu.png');
const menuLit = await litPx();

// 2) Play Now  (highlight is on "Play Now" already -> Space confirms)
await tap(' ');
await page.waitForTimeout(300);
// play for a few seconds: weave + fire so asteroids spawn and get shot
for (let i = 0; i < 5; i++) {
  await tap('ArrowLeft', 220); await tap(' ', 90);
  await tap('ArrowRight', 220); await tap(' ', 90);
  await page.waitForTimeout(250);
}
await page.waitForTimeout(400);
await shot('shot_2_play.png');
const playLit = await litPx();

// 3) Auto mode: reload, pick "Auto" (Down,Down) then Space -> screensaver
await page.reload({ waitUntil: 'load' });
for (let i = 0; i < 40 && !/running/i.test(await page.$eval('#status', e => e.textContent)); i++) await page.waitForTimeout(200);
await page.waitForTimeout(400);
await tap('ArrowDown'); await tap('ArrowDown'); await page.waitForTimeout(150);
await tap(' ');
await page.waitForTimeout(1200);
await shot('shot_3_screensaver.png');
const saverLit = await litPx();

// 4) Pick up -> WARNING placard
await page.click('#pickup');
await page.waitForTimeout(450);
await shot('shot_4_warning.png');

// wait for WARNING -> PLAYING (1.7s placard), then idle to trigger AUTOPILOT
await page.waitForTimeout(1700);
await page.click('#putdown');      // settle the device so the idle timer can run
await page.waitForTimeout(500);
await shot('shot_5_autoplay.png'); // should be PLAYING (HUD visible)
await page.waitForTimeout(7400);   // > IDLE_MS (6000) of stillness -> AUTOPILOT placard
await shot('shot_6_autopilot.png');

const fps = await page.$eval('#fps', e => e.textContent).catch(() => '');
await browser.close();

console.log('menu litpx     :', menuLit);
console.log('play litpx     :', playLit);
console.log('saver litpx    :', saverLit);
console.log('fps            :', fps);
console.log('errors         :', errors.length ? errors : 'none');
const ok = menuLit > 80 && playLit > 80 && saverLit > 60 && errors.length === 0;
console.log(ok ? 'PLAYTEST: PASS' : 'PLAYTEST: FAIL');
process.exit(ok ? 0 : 1);
