import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';

const source = await readFile(new URL('../src/FrequencyAnalysisSettings.ts', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext } });
const { FREQUENCY_DEFAULTS: defaults, readFrequencySettings: read, frequencyRange: range, visibleFrequencyChannels: visible } =
  await import(`data:text/javascript;base64,${Buffer.from(outputText).toString('base64')}`);

test('held spectrum survives a project round trip while live FFT and channel settings differ', () => {
  const held = { sr: 44100, fftSize: 1024, channel: 7, window: 'hamming', averages: 7,
    capturedAt: '2026-10-23T00:00:00.000Z', decibels: Array.from({ length: 513 }, (_, i) => -80 + i / 10) };
  const settings = { ...defaults, fftSize: 16384, channel: 0, window: 'flatTop', held };
  const restored = read(JSON.stringify(settings));
  assert.deepEqual(restored.held, held);
  assert.equal(restored.fftSize, 16384);
  assert.equal(restored.channel, 0);
  assert.deepEqual(read(JSON.stringify({ ...restored, held: null })).held, null);
});
test('saved frequency range remains valid after switching to a lower simulation rate', () => {
  const settings = read(JSON.stringify({ freqMin: 10000, freqMax: 20000 }));
  assert.deepEqual(range(settings, 1500), { minimum: 0, maximum: 750 });
  assert.deepEqual(range({ ...settings, scale: 'log' }, 1500), { minimum: 1, maximum: 750 });
});
test('switching to logarithmic scale on a 0–1 Hz view still produces a finite range', () => {
  const settings = read(JSON.stringify({ freqMin: 0, freqMax: 1, scale: 'log' }));
  const { minimum, maximum } = range(settings, 48000);
  assert.ok(minimum > 0 && minimum < maximum);
  assert.ok(Number.isFinite(Math.log(maximum / minimum)));
});
test('malformed saved data cannot create an invalid FFT, axis, or held trace', () => {
  assert.deepEqual(read('{broken json'), defaults);
  const settings = read(JSON.stringify({ fftSize: 777, window: 'unknown', channel: -2,
    freqMin: 900, freqMax: 5, dbMin: 10, dbMax: -100,
    held: { sr: 48000, fftSize: 1024, decibels: [-20] } }));
  assert.equal(settings.fftSize, 2048); assert.equal(settings.window, 'hann');
  assert.equal(settings.channel, 0); assert.equal(settings.held, null);
  assert.ok(settings.freqMin < settings.freqMax); assert.ok(settings.dbMin < settings.dbMax);
});

test('subplot selections and independent held references survive save and reload', () => {
  const trace = channel => ({ sr: 48000, fftSize: 256, channel, window: 'hann', averages: 2,
    decibels: Array.from({ length: 129 }, (_, i) => -100 + channel + i / 10), capturedAt: '2026-10-26T00:00:00Z' });
  const saved = { layout: 'stacked', selectedChannels: [7, 0, 7, 63, -1, 64, '3', 1.5], heldChannels: [trace(0), trace(7)] };
  const restored = read(JSON.stringify(saved));
  assert.equal(restored.layout, 'stacked');
  assert.deepEqual(restored.selectedChannels, [0, 7, 63]);
  assert.deepEqual(restored.heldChannels, saved.heldChannels);
  assert.deepEqual(visible(restored, 8), [0, 7]);
  assert.deepEqual(visible(restored, 2), [0]);
  assert.deepEqual(visible(restored, 0), []);
  assert.deepEqual(read(JSON.stringify(restored)), restored);
});

test('all channels follows growth, none stays empty, and old settings retain single mode', () => {
  const all = read('{"layout":"stacked"}');
  assert.deepEqual(visible(all, 2), [0, 1]);
  assert.equal(visible(all, 64).length, 64);
  assert.deepEqual(visible(read('{"layout":"stacked","selectedChannels":[]}'), 8), []);
  assert.deepEqual(visible(read('{"channel":7}'), 8), [7]);
  assert.equal(read('{"layout":"unknown","heldChannels":[{}]}').layout, 'single');
  assert.deepEqual(read('{"heldChannels":[{}]}').heldChannels, []);
});
