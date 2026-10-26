// Keep type 26 and the .patchy format compatible with existing audio projects.
interface BuiltinNode {
  type: number; label: string; desc: string; icon: string;
  initialSettings?: { mode: 'file' | 'sine' };
}
export const BUILTIN_GROUPS: { label: string; accent: string; dim: string; nodes: BuiltinNode[] }[] = [
  {
    label: 'Audio', accent: 'var(--audio)', dim: 'var(--audio-dim)',
    nodes: [
      { type: 3, label: 'Audio In Device', desc: 'Receive from audio device', icon: '◀' },
      { type: 4, label: 'Audio Out Device', desc: 'Send to audio device', icon: '▶' },
      { type: 6, label: 'Audio Monitor', desc: 'Select and view audio channels', icon: '◎' },
      { type: 29, label: 'Frequency Analysis', desc: 'Welch power spectrum, FFT settings and held reference', icon: '≋' },
    ],
  },
  {
    label: 'Source', accent: 'var(--audio)', dim: 'var(--audio-dim)',
    nodes: [
      { type: 26, label: 'Audio File', desc: 'Load and play a multi-channel audio file', icon: '▷', initialSettings: { mode: 'file' } },
      { type: 26, label: 'Waveform Generator', desc: 'Sine, square, triangle, saw and noise', icon: '∿', initialSettings: { mode: 'sine' } },
    ],
  },
];
