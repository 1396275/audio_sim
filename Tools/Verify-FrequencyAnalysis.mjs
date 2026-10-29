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
  await click('Subplots');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.layout, 'stacked');
  const sendChannels = async (channels, inputChannels = 8, sr = 48000) => {
    const frames = channels.map(channel => ({ ...snapshot, layout: 'stacked', channel, inputChannels, sr,
      decibels: Array.from({ length: 1025 }, (_, i) => i === 20 + channel * 60 ? -22 - channel : -115).join(',') }));
    await evaluate(`window.__bridge.onWelchSnapshot(${JSON.stringify(JSON.stringify(frames))})`);
    await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
  };
  await sendChannels([0, 1, 2, 3, 4, 5, 6, 7]);
  await waitFor('document.querySelectorAll("[data-spectrum-channel]").length === 8');
  assert.notEqual(await evaluate('document.querySelectorAll("canvas")[0].toDataURL()'),
    await evaluate('document.querySelectorAll("canvas")[1].toDataURL()'), 'Subplots must draw different spectra for different channels');
  const clickChannel = (channel, label) => evaluate(`(() => {
    const plot = document.querySelector('[data-spectrum-channel="${channel}"]');
    const button = [...plot.querySelectorAll('button')].find(e => e.textContent.trim() === ${JSON.stringify(label)});
    if (!button || button.disabled) throw new Error('Missing enabled per-channel button'); button.click();
  })()`);
  await clickChannel(0, 'Hold spectrum'); await clickChannel(1, 'Hold spectrum');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.equal(captured.heldChannels.find(h => h.channel === 0).decibels[20], -22);
  assert.equal(captured.heldChannels.find(h => h.channel === 1).decibels[80], -23);
  const setPlotsPerRow = async columns => {
    await evaluate(`document.querySelector('[data-frequency-columns] .nodrag > div').click()`);
    await evaluate(`(() => {
      const option = [...document.querySelectorAll('[data-frequency-columns] .nodrag > div + div > div')]
        .find(e => e.querySelector('span')?.textContent === ${JSON.stringify(String(columns))});
      if (!option) throw new Error('Missing plots-per-row option'); option.click();
    })()`);
    await waitFor(`!!document.querySelector('[data-spectrum-columns="${columns}"]')`);
    await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
  };
  for (const columns of [2, 3, 4, 8, 1, 2]) {
    await setPlotsPerRow(columns);
    const rectangles = await evaluate(`[...document.querySelectorAll('[data-spectrum-channel]')].map(e => {
      const r = e.getBoundingClientRect(); return {x: r.x, y: r.y, width: r.width};
    })`);
    assert.equal(rectangles.length, 8);
    for (let i = 0; i < rectangles.length; ++i) {
      const firstInRow = Math.floor(i / columns) * columns;
      assert.ok(Math.abs(rectangles[i].y - rectangles[firstInRow].y) < 1, `${columns} plots per row must share a row`);
      if (i % columns) assert.ok(rectangles[i].x > rectangles[i - 1].x, 'Columns must progress from left to right');
      if (i >= columns) assert.ok(rectangles[i].y > rectangles[i - columns].y, 'Remaining channels must wrap to subsequent rows');
    }
    // Rearranging a stopped capture must retain live data and held references,
    // without waiting for a fresh native FFT snapshot.
    assert.ok(await evaluate(`!document.querySelector('[data-spectrum-channel="0"] button').disabled`));
    captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
    assert.equal(captured.plotsPerRow, columns);
    assert.equal(captured.heldChannels.find(h => h.channel === 1).decibels[80], -23);
  }
  await evaluate(`(() => {
    const canvas = document.querySelector('[data-spectrum-channel="1"] canvas');
    const r = canvas.getBoundingClientRect();
    canvas.dispatchEvent(new MouseEvent('mousemove', {clientX: r.left + (48 + (canvas.width - 14 - 48) / 2) * r.width / canvas.width, bubbles: true}));
  })()`);
  await waitFor(`document.querySelector('[data-spectrum-channel="1"]').textContent.includes('Hz · Live')`);
  const cursorReadout = await evaluate(`document.querySelector('[data-spectrum-channel="1"]').textContent`);
  const cursorFrequency = cursorReadout.match(/([\d.]+) Hz · Live/);
  // MouseEvent coordinates are integer CSS pixels; allow one plot pixel's
  // quantisation around the geometric midpoint of the 1–750 Hz log axis.
  assert.ok(cursorFrequency && Math.abs(Number(cursorFrequency[1]) - Math.sqrt(750)) < 1,
    `Resized canvas cursor must match its frequency axis: ${cursorReadout}`);
  await screenshot('welch-grid-two-columns.png');
  const savedSubplots = structuredClone(captured);
  await clickChannel(0, 'Clear held');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.deepEqual(captured.heldChannels.map(h => h.channel), [1], 'Clearing one subplot must retain other channel references');
  await evaluate(`document.querySelector('[data-spectrum-channel="1"] canvas').dispatchEvent(new MouseEvent('mousemove', {clientX: 650, bubbles: true}))`);
  await waitFor(`document.querySelector('[data-spectrum-channel="1"]').textContent.includes('Live ')`);
  // Channel checkboxes, empty selection, and restoration through native graph updates.
  await evaluate(`[...document.querySelectorAll('label')].find(e => e.textContent.trim() === 'Ch 2').querySelector('div').click()`);
  await waitFor('document.querySelectorAll("[data-spectrum-channel]").length === 7');
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.ok(!captured.selectedChannels.includes(1));
  await click('None'); await waitFor('document.querySelectorAll("canvas").length === 0');
  await click('All channels'); await waitFor('document.querySelectorAll("canvas").length === 8');
  graph.nodes[0].settingsJson = JSON.stringify({ ...savedSubplots, selectedChannels: [0, 1] });
  await evaluate(`window.__bridge.onGraphUpdate(${JSON.stringify(JSON.stringify(graph))})`);
  await sendChannels([0, 1]);
  await waitFor('document.querySelectorAll("canvas").length === 2');
  assert.ok(await evaluate(`!!document.querySelector('[data-spectrum-columns="2"]')`), 'Saved plots-per-row must survive graph reload');
  await waitFor(`document.querySelector('[data-spectrum-channel="1"]').textContent.includes('Held: Ch 2')`);
  await screenshot('welch-subplots-light.png');
  // A shrinking input layout must remove the disconnected channel's old live plot.
  await sendChannels([0], 1, 1500);
  await waitFor('document.querySelectorAll("canvas").length === 1');
  await evaluate(`window.__bridge.onWelchSnapshot(${JSON.stringify(JSON.stringify([{ ...snapshot, layout: 'stacked', sr: 1500, channel: -1, inputChannels: 0, averages: 0, decibels: '' }]))})`);
  await waitFor('document.querySelectorAll("canvas").length === 0');
  await click('Single channel');
  await sendSpectrum({ ...snapshot, sr: 1500, inputChannels: 0, averages: 0, decibels: '' });
  // Verify the new audio modules against the same production bundle.
  await call('Emulation.setDeviceMetricsOverride', { width: 1760, height: 1100, deviceScaleFactor: 1, mobile: false });
  const audioPort = (id, label, direction, type = 'audio') => ({ id: `${id}_${label}_${direction === 'input' ? 'in' : 'out'}`, label, type, direction });
  const modules = [
    { id: 'test_rms', label: 'RMS', nodeType: 30, x: 50, y: 40, settingsJson: '{"windowMs":100}',
      ports: [audioPort('test_rms', 'Audio In', 'input'), audioPort('test_rms', 'Value Out', 'output', 'value')] },
    { id: 'test_mux', label: 'Mux', nodeType: 31, x: 420, y: 40, settingsJson: '{"portCount":2}',
      ports: [audioPort('test_mux', 'Audio In', 'input'), audioPort('test_mux', 'Audio In 2', 'input'), audioPort('test_mux', 'Audio Out', 'output')] },
    { id: 'test_demux', label: 'Demux', nodeType: 32, x: 50, y: 430, settingsJson: '{"widths":[2,3]}',
      ports: [audioPort('test_demux', 'Audio In', 'input'), audioPort('test_demux', 'Audio Out', 'output'), audioPort('test_demux', 'Audio Out 2', 'output')] },
    { id: 'test_eq', label: 'Parametric EQ', nodeType: 33, x: 790, y: 40, settingsJson: '{}',
      ports: [audioPort('test_eq', 'Audio In', 'input'), audioPort('test_eq', 'Audio Out', 'output')] },
  ];
  await evaluate(`window.__bridge.onPaxList(${JSON.stringify(JSON.stringify({ paxItems: [{name: 'Amp', nodeType: 2, audioInputs: 1, audioOutputs: 1, params: []}] }))});
    window.__bridge.onGraphUpdate(${JSON.stringify(JSON.stringify({ ...graph, nodes: modules }))});`);
  await waitFor('document.querySelectorAll("[data-audio-module]").length === 4');
  for (const [category, names] of [['Channel Routing', ['Mux', 'Demux']], ['Measurement & Analysis', ['RMS', 'Frequency Analysis']], ['Filters & EQ', ['Parametric EQ']], ['Gain', ['Amp']]]) {
    const contents = await evaluate(`(() => {
      const header = [...document.querySelectorAll('aside button')].find(e => e.textContent.includes(${JSON.stringify(category)}));
      return header?.parentElement.textContent ?? '';
    })()`);
    for (const name of names) assert.ok(contents.includes(name), `${name} must be grouped under ${category}`);
  }
  const moduleStatus = (id, status) => ({ id, midi: 0, l: 0, r: 0, portRms: [], notes: '', bytes: 0,
    isMk2: false, dmxValue: 0, paxReadOnly: [], genericValuePortValues: [], inChMask: 0, outChMask: 0, audioModule: status });
  const statuses = [
    moduleStatus('test_rms', { sampleRate: 48000, inputChannels: 2, inputWidths: [2], outputWidths: [], rms: [Math.SQRT1_2, 0.00001], rmsReady: true, windowSamples: 4800 }),
    moduleStatus('test_mux', { sampleRate: 48000, inputChannels: 5, inputWidths: [2, 3], outputWidths: [5], valid: true }),
    moduleStatus('test_demux', { sampleRate: 48000, inputChannels: 5, inputWidths: [5], outputWidths: [2, 3], requiredChannels: 5, valid: true }),
    moduleStatus('test_eq', { sampleRate: 48000, inputChannels: 5, inputWidths: [5], outputWidths: [5], response: [[5, 0], [1000, 6], [23520, 0]], bypass: false }),
  ];
  await evaluate(`window.__bridge.onPortActivity(${JSON.stringify(JSON.stringify(statuses))})`);
  await waitFor(`document.querySelector('[data-audio-module="30"]').textContent.includes('-3.01')`);
  assert.ok(await evaluate(`document.querySelector('[data-audio-module="30"]').textContent.includes('-100.00')`), 'RMS must retain quiet-channel precision');
  assert.ok(await evaluate(`document.querySelector('[data-audio-module="31"]').textContent.includes('5 ch')`));
  assert.ok(await evaluate(`!!document.querySelector('svg[aria-label="EQ frequency response"] polyline')`));
  const editModuleNumber = async (type, index, value) => {
    await evaluate(`document.querySelectorAll('[data-audio-module="${type}"] span.nodrag[title^="Drag up/down"]')[${index}].dispatchEvent(new MouseEvent('dblclick', {bubbles: true}))`);
    await waitFor(`!!document.querySelector('[data-audio-module="${type}"] input')`);
    await evaluate(`(() => { const input = document.querySelector('[data-audio-module="${type}"] input'); input.focus(); input.select(); })()`);
    await call('Input.insertText', { text: String(value) });
    await call('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
    await waitFor(`!document.querySelector('[data-audio-module="${type}"] input')`);
    return evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  };
  assert.equal((await editModuleNumber(30, 0, 37)).windowMs, 37);
  assert.equal((await editModuleNumber(31, 0, 4)).portCount, 4);
  captured = await editModuleNumber(33, 2, -9);
  assert.equal(captured.bands[0].gainDb, -9, 'EQ gain must commit the actual edited band');
  await evaluate(`[...document.querySelectorAll('[data-audio-module="32"] button')].find(e => e.textContent.trim() === 'Split to mono').click()`);
  captured = await evaluate(`JSON.parse(window.testMessages.filter(m => m.type === 'commitSettingsChange').at(-1).settings)`);
  assert.deepEqual(captured.widths, [1, 1, 1, 1, 1]);
  statuses[2].audioModule.valid = false; statuses[2].audioModule.requiredChannels = 2;
  await evaluate(`window.__bridge.onPortActivity(${JSON.stringify(JSON.stringify(statuses))})`);
  await waitFor(`!!document.querySelector('[data-audio-module="32"] [role="alert"]')`);
  await screenshot('audio-modules-light.png');
  assert.deepEqual(errors, [], 'UI must have no runtime exceptions');
  console.log('PASS: module categories, RMS numeric precision, Mux ports, Demux partition controls, EQ response and edited parameters');
  console.log('PASS: live PSD, hold/replace/clear, subplots, 1/2/3/4/8-column grid geometry and cursor, per-channel holds, channel selection, save/reload, input shrink/disconnect, low-rate range, dark/light rendering');
  console.log(`Screenshots: ${output}`);
} finally {
  if (socket) socket.close();
  browser.kill();
  server.close();
}
