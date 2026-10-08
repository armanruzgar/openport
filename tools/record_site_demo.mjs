#!/usr/bin/env node
/**
 * Record the real terminal against an isolated, offline simulator.
 * Prerequisites (do not install or replace web/node_modules):
 *   (cd web && npm run build)
 *   /Users/armanruzgar/dev/openport-work/wtbuild.sh 8
 *   PLAYWRIGHT_DIR=/Users/armanruzgar/dev/neofund/node_modules/playwright \
 *     node tools/record_site_demo.mjs
 * Chromium must already be installed (PLAYWRIGHT_BROWSERS_PATH is respected).
 * Optional: FFMPEG=/opt/homebrew/bin/ffmpeg, FRAMES_DIR=/absolute/review/path.
 * Default frames: ../frames relative to this worktree. Port 9870 must be free.
 * Replaces the four site/media assets and updates site/index.html after success.
 * Uses native Playwright video; trims setup/loading and holds settled scenes.
 * Fixed scenario/date/seed/plan; repeatability requires the same simulator build.
 * No production account, credentials, or external market data are used. Only the
 * child PID is stopped, including on failure/SIGINT/SIGTERM. Temporary data is
 * deleted after success; retained on failure for diagnosis (path printed).
 */
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { mkdtemp, mkdir, rm, stat, copyFile, writeFile, readFile } from 'node:fs/promises';
import { tmpdir, homedir } from 'node:os';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import net from 'node:net';
import assert from 'node:assert/strict';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ffmpeg = process.env.FFMPEG || '/opt/homebrew/bin/ffmpeg';
const frames = resolve(process.env.FRAMES_DIR || join(root, '..', 'frames'));
const require = createRequire(import.meta.url);
if (!process.env.PLAYWRIGHT_DIR) throw new Error('Set PLAYWRIGHT_DIR to the existing playwright package directory.');
const { chromium } = require(resolve(process.env.PLAYWRIGHT_DIR));
const origin = 'http://127.0.0.1:9870';
const pause = ms => new Promise(r => setTimeout(r, ms));
async function command(bin, args) {
  await new Promise((ok, fail) => {
    const child = spawn(bin, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let stderr = ''; child.stderr.on('data', b => { stderr = (stderr + b).slice(-12000); });
    child.on('error', fail); child.on('exit', code => code === 0 ? ok() : fail(new Error(`${bin}: ${code}\n${stderr}`)));
  });
}
// Refuse to attach to (or stop) somebody else's listener.
await new Promise((ok, fail) => {
  const probe = net.createServer(); probe.once('error', fail);
  probe.listen(9870, '127.0.0.1', () => probe.close(ok));
});
await stat(join(root, 'build/apps/openportd'));
await stat(join(root, 'web/dist/index.html'));
const temp = await mkdtemp(join(tmpdir(), 'openport-site-demo-'));
console.log(`Isolated recording data: ${temp}`);
for (const dir of ['home', 'tmp', 'candles', 'series', 'recordings', 'scenarios', 'videos', 'output']) await mkdir(join(temp, dir));
const server = spawn(join(root, 'build/apps/openportd'), [
  '--provider', 'demo', '--address', '127.0.0.1', '--port', '9870',
  '--web-root', join(root, 'web/dist'), '--paper-journal', join(temp, 'paper.jsonl'),
  '--candle-dir', join(temp, 'candles'), '--series-dir', join(temp, 'series'),
  '--record-dir', join(temp, 'recordings'), '--scenario-dir', join(temp, 'scenarios'),
  '--no-history', '--no-series', '--no-cboe-holidays',
], { cwd: temp, env: { PATH: process.env.PATH, HOME: join(temp, 'home'), TMPDIR: join(temp, 'tmp'), TMP: join(temp, 'tmp'), TEMP: join(temp, 'tmp'), TZ: 'America/New_York' }, stdio: ['ignore', 'pipe', 'pipe'] });
let log = '', browser, succeeded = false;
server.stdout.on('data', b => { log += b; }); server.stderr.on('data', b => { log += b; });
server.on('error', error => { log += String(error); });
async function cleanup() {
  await browser?.close().catch(() => {});
  if (server.exitCode === null && server.signalCode === null) {
    server.kill('SIGTERM');
    await Promise.race([new Promise(r => server.once('exit', r)), pause(5000)]);
    if (server.exitCode === null && server.signalCode === null) { server.kill('SIGKILL'); await new Promise(r => server.once('exit', r)); }
  }
  await writeFile(join(temp, 'server.log'), log);
}
for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, async () => { await cleanup(); process.exit(1); });
async function api(path, body, signal = AbortSignal.timeout(180000)) {
  const response = await fetch(origin + path, { method: body ? 'POST' : 'GET', headers: { 'Content-Type': 'application/json' }, body: body ? JSON.stringify(body) : undefined, signal });
  const value = await response.json();
  if (!response.ok) throw new Error(`${path}: ${JSON.stringify(value)}`);
  return value;
}
async function waitForReplayReady() {
  const signal = AbortSignal.timeout(120000);
  try {
    for (;;) {
      signal.throwIfAborted();
      const { replay } = await api('/api/replay', undefined, signal);
      if (replay?.fast_forwarding === false) return replay;
      await pause(500);
    }
  } catch (error) {
    if (signal.aborted) throw new Error('Timed out after 120 s waiting for GET /api/replay to report fast_forwarding=false', { cause: error });
    throw error;
  }
}
async function settle(page) {
  await page.evaluate(() => document.fonts.ready);
  await page.waitForTimeout(700);
}
async function navigate(page, view) {
  await page.evaluate(view => { location.hash = `/SPX/${view}`; }, view);
  await settle(page);
}
async function focus(locator) {
  await locator.evaluate(el => el.scrollIntoView({ block: 'center', behavior: 'instant' }));
}
try {
  for (let i = 0; ; i++) {
    if (server.exitCode !== null) throw new Error(log);
    try { await api('/api/status'); break; } catch (e) { if (i === 120) throw e; await pause(500); }
  }
  browser = await chromium.launch({ headless: true, env: { ...process.env, HOME: homedir() } });
  for (const [name, width, height] of [['desktop', 1160, 840], ['mobile', 540, 800]]) {
    await api('/api/replay', { scenario: 'trend', date: '2026-09-15', seed: '20260915', plan: 'locking-50k', start_at: '10:00', paused: true });
    const replay = await waitForReplayReady();
    const context = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, locale: 'en-US', timezoneId: 'America/New_York', colorScheme: 'dark', recordVideo: { dir: join(temp, 'videos'), size: { width, height } } });
    await context.addInitScript(() => {
      localStorage.setItem('openport.welcome', 'seen');
      localStorage.setItem('openport-theme', 'dark');
    });
    const start = performance.now();
    const page = await context.newPage();
    page.setDefaultTimeout(20000);
    const clips = [];
    async function scene(label, seconds) {
      await settle(page);
      assert.equal(await page.evaluate(() => document.documentElement.classList.contains('dark')), true, `${name}: ${label} must use the dark theme`);
      await page.screenshot({ path: join(temp, `${name}-${clips.length}-${label}.png`) });
      const from = (performance.now() - start) / 1000;
      await page.waitForTimeout(seconds * 1000 + 350);
      clips.push({ label, from, seconds });
      console.log(`${name}: ${label}`);
    }
    try {
      await page.goto(`${origin}/#/SPX/replay`);
      await page.getByRole('button', { name: 'Trade this replay', exact: true }).click();
      await page.getByRole('status', { name: 'Replay', exact: true }).waitFor();
      await settle(page);
      await navigate(page, 'dashboard');
      await page.getByText('Consistency', { exact: true }).waitFor();
      await focus(page.getByRole('heading', { name: 'Objectives to pass', exact: true }));
      assert.equal(replay.start_at, '10:00', `${name}: replay must start at 10:00`);
      assert.match(await page.getByRole('banner').innerText(), /\b10:00\b/, `${name}: header must show replay start_at ${replay.start_at} before the first scene`);
      await scene('objectives', name === 'desktop' ? 2 : 3.5);
      await focus(page.getByText('Daily loss limit', { exact: true }));
      await scene('plan', name === 'desktop' ? 2 : 3.5);
      await navigate(page, 'chain');
      await page.getByRole('radio', { name: 'Strategy', exact: true }).click();
      await page.getByRole('button', { name: 'Templates', exact: true }).waitFor();
      if (name === 'desktop') await scene('chain', 1);
      await page.getByRole('button', { name: 'Templates', exact: true }).click();
      await page.getByRole('button', { name: 'Review in ticket', exact: true }).click();
      await page.getByRole('radio', { name: 'Market', exact: true }).click();
      await page.getByLabel('Spread exits', { exact: true }).check();
      const previewReady = page.waitForResponse(response => response.url().endsWith('/api/replay/orders/preview') && response.request().postDataJSON()?.bracket?.stop_loss?.trigger?.level === '2.00');
      await page.getByLabel('Stop level', { exact: true }).fill('2.00');
      const preview = await (await previewReady).json();
      assert.equal(preview.decision, 'ok', 'The bracket must pass the simulated preview');
      await page.getByText('Buying power after', { exact: true }).waitFor();
      await page.locator('dialog').evaluate(el => { el.scrollTop = 0; });
      await scene('ticket', name === 'desktop' ? 2 : 3.5);
      await focus(page.getByLabel('Stop level', { exact: true }));
      if (name === 'desktop') await scene('bracket', 2);
      await focus(page.getByRole('region', { name: 'Order preview', exact: true }));
      await scene('preview', name === 'desktop' ? 2 : 3.5);
      if (name === 'desktop') {
        const submitted = page.waitForResponse(response => response.url().endsWith('/api/replay/orders') && response.request().method() === 'POST');
        await page.getByRole('button', { name: 'Submit strategy order', exact: true }).click();
        assert.equal((await (await submitted).json()).order.status, 'filled');
        await page.getByText('filled', { exact: true }).first().waitFor();
        await page.keyboard.press('Escape');
        await navigate(page, 'positions');
        await page.getByRole('heading', { name: 'Strategies · 1', exact: true }).waitFor();
        await focus(page.getByRole('heading', { name: 'Strategies · 1', exact: true }));
        await scene('positions', 2);
        await focus(page.getByRole('region', { name: 'Margin breakdown', exact: true }));
        await scene('margin', 2);
        const portfolio = await api('/api/replay/portfolio');
        assert.equal(portfolio.positions.length, 2, 'The vertical must really be held');
        await writeFile(join(temp, 'portfolio.json'), JSON.stringify(portfolio, null, 2));
      }
      const video = page.video();
      await context.close();
      const raw = await video.path();
      const filters = clips.map((c, i) => `[0:v]trim=start=${c.from.toFixed(3)}:duration=${c.seconds},setpts=PTS-STARTPTS,fps=30,setsar=1[v${i}]`);
      filters.push(clips.map((_, i) => `[v${i}]`).join('') + `concat=n=${clips.length}:v=1:a=0[out]`);
      const output = join(temp, 'output', `trading-${name}.mp4`);
      await command(ffmpeg, ['-y', '-i', raw, '-filter_complex', filters.join(';'), '-map', '[out]', '-an', '-c:v', 'libx264', '-preset', 'slow', '-crf', '26', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', output]);
      await command(ffmpeg, ['-y', '-ss', '2.5', '-i', output, '-frames:v', '1', '-q:v', '2', join(temp, 'output', `trading-${name}.jpg`)]);
      await mkdir(frames, { recursive: true });
      const times = name === 'desktop' ? [0.8, 3, 8, 14] : [1, 5, 8.5, 12];
      for (const [i, time] of times.entries()) await command(ffmpeg, ['-y', '-ss', String(time), '-i', output, '-frames:v', '1', join(frames, `trading-${name}-${i + 1}.png`)]);
      await writeFile(join(temp, `${name}-clips.json`), JSON.stringify(clips, null, 2));
      console.log(`${name}: ${clips.reduce((n, c) => n + c.seconds, 0)} seconds, ${(await stat(output)).size} bytes`);
    } catch (error) {
      await page.screenshot({ path: join(temp, `${name}-error.png`) }).catch(() => {});
      console.error(await page.locator('body').innerText().catch(() => ''));
      await context.close();
      throw error;
    }
  }
  // Replace deliverables only after both runs succeed.
  const indexPath = join(root, 'site/index.html');
  let html = await readFile(indexPath, 'utf8');
  const replacements = [
    [/(<span id="demo-title">)[\s\S]*?(<span class="badge">)/, '$1From evaluation plan to protected spread $2'],
    [/(<span class="demo-meta">).*?(<\/span>)/, '$1Desktop 15 s · Mobile 14 s · No audio$2'],
    [/(<video class="demo-desktop"[^>]*aria-label=")[^"]*"/, '$1Desktop demo: evaluation objectives, SPX vertical spread with bracket exits and simulated preview, held strategy and margin breakdown"'],
    [/(<video class="demo-mobile"[^>]*aria-label=")[^"]*"/, '$1Mobile demo: evaluation plan status, SPX vertical-spread ticket and simulated order preview"'],
    [/(<figcaption id="demo-caption">)[\s\S]*?(<\/figcaption>)/, '$1Review a Locking 50K evaluation, build an SPX vertical with take-profit and stop-loss exits, and inspect its simulated preview. The desktop demo also shows the filled strategy and margin; mobile shows plan status and the ticket. Prices, volume and fills are simulated in a fixed-seed generated session. Use fullscreen for a closer look.$2'],
  ];
  for (const [pattern, replacement] of replacements) {
    assert.match(html, pattern, 'Site demo markup changed; review the copy before replacing it');
    html = html.replace(pattern, replacement);
  }
  for (const name of ['desktop', 'mobile']) for (const ext of ['mp4', 'jpg']) await copyFile(join(temp, 'output', `trading-${name}.${ext}`), join(root, 'site/media', `trading-${name}.${ext}`));
  await writeFile(indexPath, html);
  succeeded = true;
} finally {
  await cleanup();
  if (succeeded) await rm(temp, { recursive: true, force: true });
  else console.error(`Recording failed; diagnostic files retained at ${temp}`);
}
