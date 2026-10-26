import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';

const source = await readFile(new URL('../src/FrequencyAnalysisSettings.ts', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext } });
const { FREQUENCY_DEFAULTS: defaults, readFrequencySettings: read, frequencyRange: range } =
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
