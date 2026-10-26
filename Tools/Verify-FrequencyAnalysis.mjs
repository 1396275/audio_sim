// Headless UI interaction check against the production bundle. No dependencies
// beyond Node 22+ and a locally installed Chrome/Edge. Uses a mocked native
// bridge; DSP correctness is exercised separately by AudioRoutingTests.
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';

const root = fileURLToPath(new URL('../', import.meta.url));
const output = path.join(root, 'build', 'frequency-ui-check');
await mkdir(output, { recursive: true });
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
const server = createServer(async (req, res) => {
  try {
    const relative = decodeURIComponent(new URL(req.url, 'http://localhost').pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.resolve(root, 'UI', 'dist', relative);
    if (!file.startsWith(path.join(root, 'UI', 'dist') + path.sep)) { res.writeHead(403).end(); return; }
    res.setHeader('Content-Type', mime[path.extname(file)] || 'application/octet-stream');
    res.end(await readFile(file));
  } catch { res.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const browserPath = process.env.AUDIOSIM_TEST_BROWSER || 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const browser = spawn(browserPath, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--remote-debugging-port=0', `--user-data-dir=${path.join(output, 'browser-profile')}`, 'about:blank'],
  { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });
let socket;
try {
  const endpoint = await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('Browser did not start')), 15000);
    browser.once('error', reject);
    browser.stderr.on('data', data => {
      const match = data.toString().match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) { clearTimeout(timeout); resolve(match[1]); }
    });
  });
  const debugBase = new URL(endpoint); debugBase.protocol = 'http:'; debugBase.pathname = '/json/list';
  const pages = await (await fetch(debugBase)).json();
  socket = new WebSocket(pages.find(page => page.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  let sequence = 0;
  const pending = new Map(), errors = [];
  socket.onmessage = event => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id); pending.delete(message.id);
      if (message.error) reject(new Error(JSON.stringify(message.error))); else resolve(message.result);
    }
    if (message.method === 'Runtime.exceptionThrown') errors.push(message.params.exceptionDetails.text);
  };
  const call = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++sequence; pending.set(id, { resolve, reject }); socket.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async expression => {
    const result = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  const waitFor = async expression => {
    for (let attempt = 0; attempt < 100; ++attempt) {
      if (await evaluate(expression)) return;
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error(`Timed out waiting for ${expression}`);
  };
  const click = async label => {
    const found = await evaluate(`(() => {
      const element = [...document.querySelectorAll('button')].find(e => e.textContent.trim() === ${JSON.stringify(label)});
      if (!element || element.disabled) return false; element.click(); return true;
    })()`);
    assert.ok(found, `Button ${label} must be enabled`);
  };
  await call('Runtime.enable'); await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', { width: 1360, height: 900, deviceScaleFactor: 1, mobile: false });
  await call('Page.navigate', { url: `http://127.0.0.1:${server.address().port}/` });
  await waitFor('!!window.__bridge && !!document.querySelector(".react-flow")');
  const graph = { nodes: [{ id: 'welch_ui_test', label: 'Frequency Analysis', nodeType: 29, x: 300, y: 60,
    ports: [{ id: 'welch_ui_test_Audio In_in', label: 'Audio In', type: 'audio', direction: 'input' }], settingsJson: '{}' }],
    connections: [], wraps: [], viewportX: 0, viewportY: 0, viewportZoom: 1 };
  await evaluate(`window.testMessages = []; window.__JUCE__ = {backend: {emitEvent: (_, m) => window.testMessages.push(m)}};
    window.__bridge.onGraphUpdate(${JSON.stringify(JSON.stringify(graph))});`);
  await waitFor(`!!document.querySelector('canvas[aria-label="Welch power spectral density"]')`);
  const snapshot = { nodeId: 'welch_ui_test', sr: 48000, fftSize: 2048, window: 'hann', channel: 0,
    inputChannels: 8, averages: 8, requestedAverages: 8, decibels: Array.from({ length: 1025 }, (_, i) => i === 43 ? -24 : -110).join(',') };
  const sendSpectrum = async snap => {
    await evaluate(`window.__bridge.onWelchSnapshot(${JSON.stringify(JSON.stringify([snap]))})`);
    await waitFor(`document.body.textContent.includes(${JSON.stringify(`${snap.averages}/${snap.requestedAverages} segments`)})`);
    await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
  };
  await sendSpectrum(snapshot);
  const firstRender = await evaluate('document.querySelector("canvas").toDataURL()');
  await sendSpectrum(snapshot);
  assert.equal(await evaluate('document.querySelector("canvas").toDataURL()'), firstRender,
    'Drawing an identical spectrum must not accumulate alpha or leave old traces');
  await click('Hold spectrum');
  let captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.held.decibels[43], -24);
  assert.equal(captured.held.sr, 48000);
  await sendSpectrum({ ...snapshot, decibels: snapshot.decibels.replace('-24', '-44') });
  // Held copy must not follow live updates.
  assert.equal((await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`)).held.decibels[43], -24);
  await click('Replace held');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.held.decibels[43], -44);
  await click('20 Hz–20 kHz');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.freqMin, 20); assert.equal(captured.freqMax, 20000);
  // Reload a saved reference with a different grid and a low-rate live view.
  graph.nodes[0].settingsJson = JSON.stringify({ ...captured, scale: 'log', freqMin: 0, freqMax: 20000 });
  await evaluate(`window.__bridge.onGraphUpdate(${JSON.stringify(JSON.stringify(graph))})`);
  await sendSpectrum({ ...snapshot, sr: 1500 });
  assert.ok(await evaluate('document.body.textContent.includes("Held: Ch 1 · 2048 points · Hann · 48000 Hz")'));
  await click('Full range');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.freqMax, 750);
  const screenshot = async name => {
    await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
    const result = await call('Page.captureScreenshot', { format: 'png' });
    await writeFile(path.join(output, name), Buffer.from(result.data, 'base64'));
  };
  await screenshot('welch-dark.png');
  const togglePreferences = () => evaluate(`document.querySelector('button[aria-label="Preferences"]').dispatchEvent(new MouseEvent('mousedown', {bubbles:true}))`);
  await togglePreferences();
  await waitFor('!![...document.querySelectorAll("button")].find(e => e.textContent.trim() === "Light")');
  await click('Light'); await togglePreferences();
  await screenshot('welch-light.png');
  await click('Clear held');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.held, null);
  await sendSpectrum({ ...snapshot, sr: 1500, inputChannels: 0, averages: 0, decibels: '' });
  assert.deepEqual(errors, [], 'UI must have no runtime exceptions');
  console.log('PASS: live PSD, hold/replace/clear, saved reference, low-rate range, dark/light rendering');
  console.log(`Screenshots: ${output}`);
} finally {
  if (socket) socket.close();
  browser.kill();
  server.close();
}
