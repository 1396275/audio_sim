import { memo, useCallback, useEffect, useRef, useState } from 'react';
import type { CSSProperties, ReactNode } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, NodeHeader, SettingsPanelHeader, useNodeCollapsed, useNodeDelete, useNodeSettings } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import ValueBox from './ValueBox';
import { themeColor, useTheme } from './Theme';
import { FFT_SIZES, WINDOWS, FREQUENCY_DEFAULTS, FrequencyAnalysisSettings, SpectrumTrace,
  frequencyRange, readFrequencySettings } from './FrequencyAnalysisSettings';

const WIDTH = 500, HEIGHT = 240;
const PLOT = { left: 48, right: WIDTH - 14, top: 22, bottom: HEIGHT - 28 };
const hz = (v: number) => v >= 1000 ? `${Number((v / 1000).toFixed(2))}k` : `${Number(v.toFixed(1))}`;
const windowLabel = (name: string) => WINDOWS.find(w => w.id === name)?.name ?? name;
const button: CSSProperties = { fontSize: 10, border: '1px solid var(--border)', borderRadius: 3,
  padding: '4px 8px', background: 'var(--surface2)', color: 'var(--text-dim)', cursor: 'pointer' };

function draw(ctx: CanvasRenderingContext2D, s: FrequencyAnalysisSettings, live: SpectrumTrace | null,
  sampleRate: number, channels: number, cursor: number | null) {
  const range = frequencyRange(s, sampleRate);
  const width = PLOT.right - PLOT.left, height = PLOT.bottom - PLOT.top;
  const xAt = (f: number) => PLOT.left + width * (s.scale === 'log'
    ? Math.log(f / range.minimum) / Math.log(range.maximum / range.minimum)
    : (f - range.minimum) / (range.maximum - range.minimum));
  const yAt = (db: number) => PLOT.bottom - height * (db - s.dbMin) / (s.dbMax - s.dbMin);
  ctx.clearRect(0, 0, WIDTH, HEIGHT);
  ctx.fillStyle = themeColor('--scope-bg'); ctx.fillRect(0, 0, WIDTH, HEIGHT);
  ctx.font = '10px monospace'; ctx.lineWidth = 0.5;
  ctx.fillStyle = themeColor('--plot-label'); ctx.fillText('PSD · dBFS/Hz', 5, 12);
  ctx.fillText('Hz', WIDTH - 22, HEIGHT - 3);
  ctx.strokeStyle = themeColor('--plot-grid'); ctx.beginPath();
  for (let i = 0; i <= 5; ++i) {
    const db = s.dbMin + (s.dbMax - s.dbMin) * i / 5;
    const y = yAt(db); ctx.moveTo(PLOT.left, y); ctx.lineTo(PLOT.right, y);
    ctx.fillText(db.toFixed(0), 4, y + 3);
  }
  const ticks: number[] = [];
  if (s.scale === 'linear') for (let i = 0; i <= 5; ++i) ticks.push(range.minimum + (range.maximum - range.minimum) * i / 5);
  else {
    ticks.push(range.minimum);
    for (let decade = 1; decade <= range.maximum; decade *= 10)
      for (const multiple of [1, 2, 5]) if (decade * multiple > range.minimum && decade * multiple < range.maximum) ticks.push(decade * multiple);
    ticks.push(range.maximum);
  }
  let lastLabelX = -100;
  for (const f of ticks) {
    const x = xAt(f); ctx.moveTo(x, PLOT.top); ctx.lineTo(x, PLOT.bottom);
    if (x - lastLabelX > 37 && (range.maximum === f || x < PLOT.right - 35)) {
      ctx.fillText(hz(f), Math.min(PLOT.right - 15, x - 10), HEIGHT - 12); lastLabelX = x;
    }
  }
  ctx.stroke();
  ctx.save(); ctx.beginPath(); ctx.rect(PLOT.left, PLOT.top, width, height); ctx.clip();
  const trace = (spectrum: SpectrumTrace | null, colour: string, held: boolean) => {
    if (!spectrum?.decibels.length) return;
    const binHz = spectrum.sr / spectrum.fftSize;
    ctx.strokeStyle = colour; ctx.lineWidth = held ? 1.3 : 1.5; ctx.setLineDash(held ? [5, 3] : []);
    ctx.beginPath(); let started = false;
    const first = Math.max(s.scale === 'log' ? 1 : 0, Math.floor(range.minimum / binHz));
    const last = Math.min(spectrum.decibels.length - 1, Math.ceil(range.maximum / binHz));
    for (let bin = first; bin <= last; ++bin) {
      const x = xAt(bin * binHz), y = yAt(spectrum.decibels[bin]);
      if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
    }
    ctx.stroke(); ctx.setLineDash([]);
  };
  trace(s.held, themeColor('--wave-trigger'), true);
  trace(live, themeColor('--audio'), false);
  if (cursor !== null) {
    const x = xAt(cursor); ctx.strokeStyle = themeColor('--text-dim'); ctx.setLineDash([2, 3]);
    ctx.beginPath(); ctx.moveTo(x, PLOT.top); ctx.lineTo(x, PLOT.bottom); ctx.stroke(); ctx.setLineDash([]);
  }
  ctx.restore();
  if (!live?.decibels.length) {
    ctx.fillStyle = themeColor('--text-muted');
    ctx.fillText(channels === 0 ? 'Connect an audio signal' : s.channel >= channels ? 'Selected channel is unavailable' : 'Collecting samples for FFT…', PLOT.left + 12, PLOT.top + 20);
  }
}

function FrequencyAnalysisNode({ id, data, selected }: NodeProps) {
  const [settings, setSettings] = useState(() => readFrequencySettings(data.settingsJson as string | undefined));
  const [live, setLive] = useState<SpectrumTrace | null>(null);
  const [sampleRate, setSampleRate] = useState(48000);
  const [inputChannels, setInputChannels] = useState(0);
  const [cursor, setCursor] = useState<number | null>(null);
  const settingsRef = useRef(settings); settingsRef.current = settings;
  const liveRef = useRef(live); liveRef.current = live;
  const canvas = useRef<HTMLCanvasElement>(null), body = useRef<HTMLDivElement>(null);
  const theme = useTheme();
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, data._forceCollapsed as boolean | undefined);
  const { handleDelete } = useNodeDelete(id);
  useEffect(() => { setSettings(readFrequencySettings(data.settingsJson as string | undefined)); }, [data.settingsJson]);
  const patch = useCallback((values: Partial<FrequencyAnalysisSettings>) => {
    const next = { ...settingsRef.current, ...values };
    settingsRef.current = next; setSettings(next); Bridge.commitSettingsChange(id, next);
    if ('customName' in values) Bridge.setNodeLabel(id, values.customName ?? '');
  }, [id]);
  useEffect(() => { setLive(null); liveRef.current = null; }, [settings.fftSize, settings.window, settings.channel, settings.averages]);
  useEffect(() => Bridge.onWelchSnapshot(snapshots => {
    const snap = snapshots.find(item => item.nodeId === id);
    if (!snap) return;
    setSampleRate(snap.sr); setInputChannels(snap.inputChannels);
    const s = settingsRef.current;
    if (snap.fftSize !== s.fftSize || snap.channel !== s.channel || snap.window !== s.window || snap.requestedAverages !== s.averages) return;
    setLive({ ...snap, decibels: snap.decibels ? snap.decibels.split(',').map(Number) : [], capturedAt: new Date().toISOString() });
  }), [id]);
  useEffect(() => {
    if (collapsed) return;
    const raf = requestAnimationFrame(() => {
      const ctx = canvas.current?.getContext('2d');
      if (ctx) draw(ctx, settings, live, sampleRate, inputChannels, cursor);
    });
    return () => cancelAnimationFrame(raf);
  }, [collapsed, settings, live, sampleRate, inputChannels, cursor, theme]);
  const range = frequencyRange(settings, sampleRate);
  const nyquist = sampleRate / 2;
  const frequencyStep = Math.min(1, (range.maximum - range.minimum) / 10);
  const select = (value: string, options: { id: string; name: string }[], onChange: (v: string) => void) =>
    <NodeSelect value={value} options={options} onChange={onChange} accent="var(--audio)" showEmpty={false} />;
  const field = (label: string, control: ReactNode) => <div style={{ flex: 1, minWidth: 0 }}>
    <div style={{ fontSize: 9, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>{control}
  </div>;
  const atCursor = (spectrum: SpectrumTrace | null) => {
    if (!spectrum || cursor === null || cursor > spectrum.sr / 2) return '—';
    const value = spectrum.decibels[Math.round(cursor * spectrum.fftSize / spectrum.sr)];
    return value === undefined ? '—' : `${value.toFixed(1)} dBFS/Hz`;
  };
  const hold = () => {
    if (liveRef.current?.decibels.length) patch({ held: { ...liveRef.current, decibels: [...liveRef.current.decibels] } });
  };
  return <div style={{ width: WIDTH + 16, position: 'relative', background: 'var(--surface)',
    border: `1px solid ${selected ? 'var(--audio)' : 'var(--border)'}`, borderTop: '3px solid var(--audio)',
    borderRadius: 'var(--radius)', boxShadow: 'var(--shadow-node)', zIndex: showSettings ? 9999 : undefined }}>
    <NodeHandle nodeId={id} label="Audio In" direction="in" colour="var(--audio)" index={0} total={1} anchor="spectrum" portBodyRef={body} />
    <NodeHeader title={settings.customName || 'FREQUENCY ANALYSIS'} accent="var(--audio)"
      rename={{ value: settings.customName, placeholder: 'Frequency Analysis', onCommit: name => patch({ customName: name }) }}
      showSettings={showSettings} onToggleSettings={toggleSettings} onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed} />
    {!collapsed && <div ref={body} className="nodrag nowheel" onPointerDown={e => e.stopPropagation()} style={{ padding: 7 }}>
      <div style={{ display: 'flex', gap: 8, marginBottom: 8 }}>
        {field('Window', select(settings.window, WINDOWS, v => patch({ window: v })))}
        {field('FFT length', select(String(settings.fftSize), FFT_SIZES.map(n => ({ id: String(n), name: `${n} points` })), v => patch({ fftSize: Number(v) })))}
        {field('Welch averages', select(String(settings.averages), [2, 4, 8, 16].map(n => ({ id: String(n), name: `${n} segments` })), v => patch({ averages: Number(v) })))}
      </div>
      <div data-port-anchor="spectrum">
        <canvas ref={canvas} width={WIDTH} height={HEIGHT} aria-label="Welch power spectral density"
          onMouseMove={event => {
            const bounds = event.currentTarget.getBoundingClientRect();
            const position = Math.max(0, Math.min(1, ((event.clientX - bounds.left) * WIDTH / bounds.width - PLOT.left) / (PLOT.right - PLOT.left)));
            setCursor(settings.scale === 'log' ? range.minimum * Math.pow(range.maximum / range.minimum, position) : range.minimum + (range.maximum - range.minimum) * position);
          }} onMouseLeave={() => setCursor(null)} style={{ display: 'block', borderRadius: 3, cursor: 'crosshair' }} />
      </div>
      <div style={{ display: 'flex', gap: 10, alignItems: 'center', marginTop: 7, fontSize: 10 }}>
        <span style={{ color: 'var(--audio)' }}>━ Live</span>
        <span style={{ color: 'var(--wave-trigger)' }}>┄ Held{settings.held ? ` · Ch ${settings.held.channel + 1}` : ''}</span>
        <button style={{ ...button, marginLeft: 'auto', opacity: live?.decibels.length ? 1 : 0.45 }} disabled={!live?.decibels.length} onClick={hold}
          title="Capture the currently displayed Welch spectrum; live analysis continues">{settings.held ? 'Replace held' : 'Hold spectrum'}</button>
        <button style={button} disabled={!settings.held} onClick={() => patch({ held: null })}>Clear held</button>
      </div>
      <div style={{ minHeight: 16, fontSize: 9, color: 'var(--text-muted)', marginTop: 5 }}>
        {cursor === null ? `${sampleRate} Hz · Δf ${(sampleRate / settings.fftSize).toFixed(2)} Hz · ${live?.averages ?? 0}/${settings.averages} segments · 50% overlap`
          : `${cursor.toFixed(1)} Hz · Live ${atCursor(live)} · Held ${atCursor(settings.held)}`}
      </div>
      <div style={{ display: 'flex', gap: 8, alignItems: 'end', marginTop: 6 }}>
        {field(`Channel (${inputChannels} input)`, select(String(settings.channel), Array.from({ length: Math.max(1, inputChannels, settings.channel + 1) }, (_, i) => ({ id: String(i), name: `Ch ${i + 1}${i >= inputChannels ? ' (unavailable)' : ''}` })), v => patch({ channel: Number(v) })))}
        {field('From (Hz)', <ValueBox value={range.minimum} min={0} max={range.maximum - frequencyStep} step={frequencyStep} width={80}
          onCommit={v => patch({ freqMin: v, freqMax: range.maximum })} accent="var(--audio)" />)}
        {field('To (Hz)', <ValueBox value={range.maximum} min={range.minimum + frequencyStep} max={nyquist} step={frequencyStep} width={80}
          onCommit={v => patch({ freqMin: range.minimum, freqMax: v })} accent="var(--audio)" />)}
        {field('Frequency axis', select(settings.scale, [{ id: 'linear', name: 'Linear' }, { id: 'log', name: 'Log' }], v => patch({ scale: v as 'linear' | 'log' })))}
      </div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 7 }}>
        <button style={button} onClick={() => patch({ freqMin: 0, freqMax: nyquist })}>Full range</button>
        <button style={button} onClick={() => patch({ freqMin: 20, freqMax: Math.min(20000, nyquist) })}>20 Hz–20 kHz</button>
        {settings.scale === 'log' && settings.freqMin === 0 && <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>Log starts at {hz(range.minimum)} Hz; DC hidden</span>}
      </div>
      {settings.held && <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 7 }} title={settings.held.capturedAt}>
        Held: Ch {settings.held.channel + 1} · {settings.held.fftSize} points · {windowLabel(settings.held.window)} · {settings.held.sr} Hz · {settings.held.averages} segments
      </div>}
    </div>}
    {showSettings && <div className="nodrag nowheel" onPointerDown={e => e.stopPropagation()} style={{ position: 'absolute', left: '100%', top: 0, marginLeft: 6,
      width: 270, padding: 12, background: 'var(--surface2)', border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)', boxShadow: 'var(--shadow-panel)' }}>
      <SettingsPanelHeader title="Frequency Analysis" onClose={closeSettings} onReset={() => patch({ ...FREQUENCY_DEFAULTS })} />
      <p style={{ fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.6 }}>Welch one-sided power spectral density with periodic windows and 50% overlap. Power is averaged before conversion to dBFS/Hz. DC is retained on the linear axis.</p>
      <div style={{ display: 'flex', gap: 8 }}>
        {field('Minimum (dBFS/Hz)', <ValueBox value={settings.dbMin} min={-200} max={settings.dbMax - 1} width={90} onCommit={v => patch({ dbMin: v })} />)}
        {field('Maximum (dBFS/Hz)', <ValueBox value={settings.dbMax} min={settings.dbMin + 1} max={20} width={90} onCommit={v => patch({ dbMax: v })} />)}
      </div>
      <p style={{ fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.6 }}>The history holds 65,536 samples. Large FFTs use as many complete segments as fit; the actual count appears below the plot. A held spectrum preserves its original sample rate and FFT grid, and is saved with the project.</p>
    </div>}
  </div>;
}
export default memo(FrequencyAnalysisNode);
