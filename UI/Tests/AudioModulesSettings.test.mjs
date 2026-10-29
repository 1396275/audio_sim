import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';
const source = await readFile(new URL('../src/AudioModulesSettings.ts', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext } });
const { readAudioModuleSettings: read, moduleDefaults: defaults, formatDisplayValue: format } = await import(`data:text/javascript;base64,${Buffer.from(outputText).toString('base64')}`);

test('routing partitions, RMS window and all EQ bands survive project restoration', () => {
  const settings = defaults();
  settings.portCount = 12; settings.widths = [2, 1, 5, 8]; settings.windowMs = 37;
  settings.bands[2] = { type: 'peak', frequency: 1800, q: 2.5, gainDb: -7.5, enabled: true };
  settings.outputGainDb = 3; settings.bypass = true; settings.displayFormat = 'scientific'; settings.displayDigits = 9;
  assert.deepEqual(read(JSON.stringify(settings)), settings);
});
test('corrupt settings cannot request unbounded ports or invalid DSP parameters', () => {
  const settings = read(JSON.stringify({ portCount: 1000, widths: Array(100).fill(-1), windowMs: 0,
    outputGainDb: 100, bands: [{ type: 'invalid', frequency: -10, gainDb: 100, q: 0 }] }));
  assert.equal(settings.portCount, 64); assert.equal(settings.widths.length, 64);
  assert.ok(settings.widths.every(w => w === 1)); assert.equal(settings.windowMs, 1);
  assert.equal(settings.bands[0].type, 'lowShelf'); assert.equal(settings.bands[0].frequency, 5);
  assert.equal(settings.bands[0].gainDb, 24); assert.equal(settings.bands[0].q, 0.1);
  assert.deepEqual(read('{bad json'), defaults());
});

test('numeric display retains small values and converts RMS amplitudes to dB correctly', () => {
  assert.equal(format(1e-5, 'linear', 6), '0.0000100000');
  assert.equal(format(1e-5, 'scientific', 6), '1.00000e-5');
  assert.equal(format(Math.SQRT1_2, 'db', 6), '-3.0103 dB');
  assert.equal(format(0, 'db', 6), '−∞ dB');
  assert.equal(format(-1, 'db', 6), '—');
  assert.equal(format(-2.5, 'linear', 6), '-2.50000');
  assert.equal(format(NaN, 'linear', 6), '—');
  assert.equal(read('{"displayDigits":100,"displayFormat":"invalid"}').displayDigits, 10);
  assert.equal(read('{"displayFormat":"invalid"}').displayFormat, 'linear');
});
