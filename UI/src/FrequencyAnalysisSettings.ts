export const FFT_SIZES = [256, 512, 1024, 2048, 4096, 8192, 16384];
export const WINDOWS = [
  { id: 'hann', name: 'Hann' }, { id: 'hamming', name: 'Hamming' },
  { id: 'blackman', name: 'Blackman' }, { id: 'blackmanHarris', name: 'Blackman-Harris' },
  { id: 'flatTop', name: 'Flat Top' }, { id: 'rectangular', name: 'Rectangular' },
];
export interface SpectrumTrace {
  sr: number; fftSize: number; channel: number; window: string; averages: number;
  decibels: number[]; capturedAt: string;
}
export interface FrequencyAnalysisSettings {
  customName: string; fftSize: number; window: string; averages: number; channel: number;
  freqMin: number; freqMax: number; scale: 'linear' | 'log';
  dbMin: number; dbMax: number; held: SpectrumTrace | null;
  layout: 'single' | 'stacked'; selectedChannels: number[] | null; heldChannels: SpectrumTrace[];
}
export const FREQUENCY_DEFAULTS: FrequencyAnalysisSettings = {
  customName: '', fftSize: 2048, window: 'hann', averages: 8, channel: 0,
  freqMin: 0, freqMax: 24000, scale: 'linear', dbMin: -140, dbMax: 0, held: null,
  layout: 'single', selectedChannels: null, heldChannels: [],
};
const finite = (value: unknown, fallback: number) => typeof value === 'number' && Number.isFinite(value) ? value : fallback;
function readHeld(h: any): SpectrumTrace | null {
  return h && FFT_SIZES.includes(h.fftSize) && Number.isFinite(h.sr) && h.sr >= 1000 && h.sr <= 192000
    && Array.isArray(h.decibels) && h.decibels.length === h.fftSize / 2 + 1
    && h.decibels.every((v: unknown) => typeof v === 'number' && Number.isFinite(v))
    ? { ...h, channel: Math.max(0, Math.min(63, Math.floor(finite(h.channel, 0)))),
        window: WINDOWS.some(w => w.id === h.window) ? h.window : 'hann',
        averages: Math.max(1, Math.min(16, Math.floor(finite(h.averages, 1)))),
        capturedAt: typeof h.capturedAt === 'string' ? h.capturedAt : '' } : null;
}
export function readFrequencySettings(json?: string): FrequencyAnalysisSettings {
  try {
    const saved = JSON.parse(json || '{}') ?? {};
    const s = { ...FREQUENCY_DEFAULTS, ...saved };
    s.customName = typeof s.customName === 'string' ? s.customName : '';
    s.fftSize = FFT_SIZES.includes(s.fftSize) ? s.fftSize : 2048;
    s.window = WINDOWS.some(w => w.id === s.window) ? s.window : 'hann';
    s.averages = [2, 4, 8, 16].includes(s.averages) ? s.averages : 8;
    s.channel = Math.max(0, Math.min(63, Math.floor(finite(s.channel, 0))));
    s.freqMin = Math.max(0, Math.min(95999, finite(s.freqMin, 0)));
    s.freqMax = Math.max(s.freqMin + 1, Math.min(96000, finite(s.freqMax, 24000)));
    s.scale = s.scale === 'log' ? 'log' : 'linear';
    s.dbMin = Math.max(-200, Math.min(19, finite(s.dbMin, -140)));
    s.dbMax = Math.max(s.dbMin + 1, Math.min(20, finite(s.dbMax, 0)));
    s.layout = s.layout === 'stacked' ? 'stacked' : 'single';
    s.selectedChannels = Array.isArray(s.selectedChannels)
      ? [...new Set<number>(s.selectedChannels.filter((v: unknown) => typeof v === 'number' && Number.isInteger(v) && v >= 0 && v < 64))].sort((a, b) => a - b) : null;
    s.held = readHeld(s.held);
    s.heldChannels = Array.isArray(s.heldChannels)
      ? [...new Map<number, SpectrumTrace>(s.heldChannels.slice(0, 64).map(readHeld).filter((h: SpectrumTrace | null): h is SpectrumTrace => !!h)
        .map((h: SpectrumTrace) => [h.channel, h] as const)).values()] : [];
    return s;
  } catch { return { ...FREQUENCY_DEFAULTS }; }
}
export function visibleFrequencyChannels(s: FrequencyAnalysisSettings, inputChannels: number): number[] {
  if (s.layout === 'single') return [s.channel];
  return Array.from({ length: Math.max(0, Math.min(64, inputChannels)) }, (_, i) => i)
    .filter(i => s.selectedChannels === null || s.selectedChannels.includes(i));
}
export function frequencyRange(s: FrequencyAnalysisSettings, sampleRate: number) {
  const maximum = Math.min(s.freqMax, sampleRate / 2);
  const minimum = s.freqMin < maximum ? s.freqMin : 0;
  return { minimum: s.scale === 'log' ? (minimum > 0 ? minimum : Math.min(1, maximum / 10)) : minimum, maximum };
}
