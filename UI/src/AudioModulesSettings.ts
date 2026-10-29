export interface EqBand { type: string; frequency: number; gainDb: number; q: number; enabled: boolean; }
export interface AudioModuleSettings {
  customName: string; windowMs: number; portCount: number; widths: number[];
  bands: EqBand[]; bypass: boolean; outputGainDb: number;
}
export const EQ_TYPES = [
  { id: 'peak', name: 'Bell' }, { id: 'lowShelf', name: 'Low shelf' }, { id: 'highShelf', name: 'High shelf' },
  { id: 'highPass', name: 'High pass' }, { id: 'lowPass', name: 'Low pass' },
];
export const moduleDefaults = (): AudioModuleSettings => ({
  customName: '', windowMs: 100, portCount: 2, widths: [1, 1], bypass: false, outputGainDb: 0,
  bands: [100, 400, 1000, 4000, 10000].map((frequency, i) => ({
    frequency, type: i === 0 ? 'lowShelf' : i === 4 ? 'highShelf' : 'peak', gainDb: 0, q: 0.70710678, enabled: true,
  })),
});
const number = (v: unknown, fallback: number, min: number, max: number) =>
  typeof v === 'number' && Number.isFinite(v) ? Math.max(min, Math.min(max, v)) : fallback;
export function readAudioModuleSettings(json?: string): AudioModuleSettings {
  const defaults = moduleDefaults();
  try {
    const s = JSON.parse(json || '{}') ?? {};
    return {
      customName: typeof s.customName === 'string' ? s.customName : '',
      windowMs: number(s.windowMs, 100, 1, 2000), portCount: Math.floor(number(s.portCount, 2, 1, 64)),
      widths: Array.isArray(s.widths) && s.widths.length ? s.widths.slice(0, 64).map((v: unknown) => Math.floor(number(v, 1, 1, 64))) : [1, 1],
      bypass: s.bypass === true, outputGainDb: number(s.outputGainDb, 0, -24, 24),
      bands: defaults.bands.map((fallback, i) => {
        const band = Array.isArray(s.bands) ? s.bands[i] ?? {} : {};
        return { type: EQ_TYPES.some(t => t.id === band.type) ? band.type : fallback.type,
          frequency: number(band.frequency, fallback.frequency, 5, 95000), gainDb: number(band.gainDb, 0, -24, 24),
          q: number(band.q, fallback.q, 0.1, 20), enabled: band.enabled !== false };
      }),
    };
  } catch { return defaults; }
}
