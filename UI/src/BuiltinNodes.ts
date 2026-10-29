// Keep type 26 and the .patchy format compatible with existing audio projects.
interface BuiltinNode {
  type: number; label: string; desc: string; icon: string;
  initialSettings?: { mode: 'file' | 'sine' };
}
export const BUILTIN_GROUPS: { label: string; accent: string; dim: string; nodes: BuiltinNode[]; paxNames?: string[] }[] = [
  {
    label: 'Audio I/O', accent: 'var(--audio)', dim: 'var(--audio-dim)',
    nodes: [
      { type: 3, label: 'Audio In Device', desc: 'Receive from audio device', icon: '◀' },
      { type: 4, label: 'Audio Out Device', desc: 'Send to audio device', icon: '▶' },
    ],
  },
  {
    label: 'Sources', accent: 'var(--audio)', dim: 'var(--audio-dim)',
    nodes: [
      { type: 26, label: 'Audio File', desc: 'Load and play a multi-channel audio file', icon: '▷', initialSettings: { mode: 'file' } },
      { type: 26, label: 'Waveform Generator', desc: 'Sine, square, triangle, saw and noise', icon: '∿', initialSettings: { mode: 'sine' } },
    ],
  },
  { label: 'Channel Routing', accent: 'var(--audio)', dim: 'var(--audio-dim)', nodes: [
    { type: 31, label: 'Mux', desc: 'Concatenate input channels in port order', icon: '⋙' },
    { type: 32, label: 'Demux', desc: 'Split channels into configured output groups', icon: '⋘' },
  ] },
  { label: 'Measurement & Analysis', accent: 'var(--audio)', dim: 'var(--audio-dim)', nodes: [
    { type: 30, label: 'RMS', desc: 'Per-channel RMS, linear values and dBFS', icon: '√' },
    { type: 6, label: 'Audio Monitor', desc: 'View selected audio channels', icon: '◎' },
    { type: 29, label: 'Frequency Analysis', desc: 'Welch PSD and multi-channel subplots', icon: '≋' },
  ] },
  { label: 'Filters & EQ', accent: 'var(--audio)', dim: 'var(--audio-dim)', nodes: [
    { type: 33, label: 'Parametric EQ', desc: 'Five bands, gain, frequency and Q', icon: '≎' },
  ] },
  { label: 'Gain', accent: 'var(--audio)', dim: 'var(--audio-dim)', nodes: [], paxNames: ['Amp'] },
];
