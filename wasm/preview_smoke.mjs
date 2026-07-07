// SPDX-License-Identifier: GPL-3.0-or-later
// Headless smoke test for the local WASM preview harness.
// Usage (playwright is resolved from the website repo's node_modules):
//   node preview_smoke.mjs http://127.0.0.1:8011/preview.html
import { createRequire } from 'module';
import { fileURLToPath } from 'url';
// Resolve playwright from cyberfidget_website/node_modules regardless of cwd.
const require = createRequire('d:/_Steele/Code_Sandbox/cyberfidget/cyberfidget_website/package.json');
const { chromium } = require('playwright');

const url = process.argv[2] || 'http://127.0.0.1:8011/preview.html';
const errors = [];
const browser = await chromium.launch();
const page = await browser.newPage();
page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
page.on('pageerror', e => errors.push('pageerror: ' + e.message));

await page.goto(url, { waitUntil: 'load' });

// Wait for the harness to report "running" (module instantiated + main started)
let status = '';
for (let i = 0; i < 60; i++) {
  status = await page.$eval('#status', el => el.textContent);
  if (/running/i.test(status) || /error/i.test(status)) break;
  await page.waitForTimeout(250);
}

// Let a few frames render, then steer + fire a moment for good measure
await page.waitForTimeout(800);
await page.keyboard.down('ArrowLeft'); await page.waitForTimeout(150); await page.keyboard.up('ArrowLeft');
await page.keyboard.down(' ');         await page.waitForTimeout(150); await page.keyboard.up(' ');
await page.waitForTimeout(400);

// Count lit pixels on the OLED canvas to prove it's actually drawing
const lit = await page.evaluate(() => {
  const c = document.getElementById('oled');
  const g = c.getContext('2d');
  const d = g.getImageData(0, 0, c.width, c.height).data;
  let n = 0;
  for (let i = 0; i < d.length; i += 4) if (d[i] || d[i+1] || d[i+2]) n++;
  return n;
});

const fps = await page.$eval('#fps', el => el.textContent).catch(() => '');
await page.screenshot({ path: fileURLToPath(new URL('./preview_smoke.png', import.meta.url)) });
await browser.close();

console.log('status :', status);
console.log('fps    :', fps);
console.log('litpx  :', lit, '/ 8192');
console.log('errors :', errors.length ? errors : 'none');

const ok = /running/i.test(status) && lit > 50 && errors.length === 0;
console.log(ok ? 'SMOKE: PASS' : 'SMOKE: FAIL');
process.exit(ok ? 0 : 1);
