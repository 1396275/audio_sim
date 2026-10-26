import { memo, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Pause, Play } from 'lucide-react';
import { Bridge } from './Bridge';
import { Checkbox, NodeHandle, NodeHeader, NodeHeaderButton, SettingsPanelHeader,
  useNodeCollapsed, useNodeDelete, useNodeSettings } from './NodeUtils';
import ValueBox from './ValueBox';
import { NodeSelect } from './NodeSelect';
import { HintContext } from './HintPanel';
import { themeColor, useTheme } from './Theme';

export interface AudioMonitorSettings {
  customName: string;
  layout: 'overlay' | 'stacked';
  selectedChannels: number[] | null; // null follows all actual input channels
  triggerMode: 'free' | 'rising' | 'falling';
  triggerLevel: number;
  amplitudeMode: 'auto' | 'fixed';
  amplitudeZoom: number;
  timeWindowMs: number;
  showClipping: boolean;
  clipThresholdDb: number;
  paused: boolean;
}
const DEFAULTS: AudioMonitorSettings = {
  customName: '', layout: 'overlay', selectedChannels: null,
  triggerMode: 'free', triggerLevel: 0, amplitudeMode: 'fixed', amplitudeZoom: 1,
  timeWindowMs: 50, showClipping: true, clipThresholdDb: 0, paused: false,
};
export interface AudioMonitorNodeData {
  label: string; nodeType: 6;
  ports: { id: string; label: string; type: 'midi' | 'audio'; direction: 'input' | 'output' }[];
  settings?: Partial<AudioMonitorSettings>; settingsJson?: string;
  [key: string]: unknown;
}
function readSettings(json?: string, initial?: Partial<AudioMonitorSettings>): AudioMonitorSettings {
  try {
    const saved = json ? JSON.parse(json) : initial ?? {};
    return { ...DEFAULTS, ...saved,
      layout: saved.layout ?? (saved.channelMode === 'stacked' ? 'stacked' : 'overlay'),
      selectedChannels: saved.selectedChannels ?? (saved.channelMode === 'L' ? [0] : saved.channelMode === 'R' ? [1] : null),
    };
  } catch { return { ...DEFAULTS }; }
}
interface Frame { channels: Float32Array[]; sampleRate: number; windowMs: number; envelope: boolean; }
const WIDTH = 360;
const colour = (channel: number, light: boolean) => `hsl(${(channel * 137.508 + 145) % 360}, 70%, ${light ? 35 : 65}%)`;
const visibleChannels = (settings: AudioMonitorSettings, count: number) =>
  Array.from({ length: count }, (_, i) => i).filter(i => settings.selectedChannels === null || settings.selectedChannels.includes(i));

function draw(ctx: CanvasRenderingContext2D, frame: Frame, s: AudioMonitorSettings, light: boolean, height: number) {
  const visible = visibleChannels(s, frame.channels.length);
  ctx.fillStyle = themeColor('--scope-bg'); ctx.fillRect(0, 0, WIDTH, height);
  if (!visible.length) {
    ctx.fillStyle = themeColor('--text-muted'); ctx.font = '11px monospace';
    ctx.fillText(frame.channels.length ? 'Select channels to display' : 'Connect an audio signal', 12, 30);
    return;
  }
  const stride = frame.envelope ? 2 : 1;
  const len = frame.channels[visible[0]].length / stride;
  const count = Math.max(2, Math.min(len, Math.floor(len * s.timeWindowMs / frame.windowMs)));
  let start = Math.max(0, len - count);
  const rows = s.layout === 'stacked' ? visible.length : 1;
  const rowHeight = height / rows;
  ctx.font = '10px monospace';
  for (let row = 0; row < rows; ++row) {
    const top = row * rowHeight;
    ctx.strokeStyle = themeColor('--plot-grid'); ctx.lineWidth = 0.5;
    ctx.beginPath();
    for (let i = 1; i < 8; ++i) {
      ctx.moveTo(WIDTH * i / 8, top); ctx.lineTo(WIDTH * i / 8, top + rowHeight);
    }
    for (let i = 0; i <= 4; ++i) {
      ctx.moveTo(0, top + rowHeight * i / 4); ctx.lineTo(WIDTH, top + rowHeight * i / 4);
    }
    ctx.stroke();
    ctx.fillStyle = themeColor('--plot-label');
    ctx.fillText(`${s.timeWindowMs} ms`, WIDTH - 46, top + rowHeight - 4);
    ctx.fillText('0', 3, top + rowHeight / 2 - 2);
  }
  let sharedPeak = 0.001;
  if (s.amplitudeMode === 'auto') for (const ch of visible)
    for (let i = start * stride; i < (start + count) * stride; ++i) sharedPeak = Math.max(sharedPeak, Math.abs(frame.channels[ch][i] ?? 0));
  const zoom = s.amplitudeMode === 'auto' ? 1 / sharedPeak : s.amplitudeZoom;
  const threshold = Math.pow(10, s.clipThresholdDb / 20);
  visible.forEach((ch, index) => {
    const top = s.layout === 'stacked' ? index * rowHeight : 0;
    const samples = frame.channels[ch];
    const mid = top + rowHeight / 2;
    ctx.save(); ctx.beginPath(); ctx.rect(0, top, WIDTH, rowHeight); ctx.clip();
    ctx.strokeStyle = colour(ch, light); ctx.lineWidth = 1.2;
    ctx.beginPath(); let clipped = false;
    for (let i = 0; i < count; ++i) {
      const value = samples[(start + i) * stride] ?? 0;
      const high = frame.envelope ? samples[(start + i) * stride + 1] ?? value : value;
      clipped ||= Math.max(Math.abs(value), Math.abs(high)) >= threshold;
      const x = i * (WIDTH - 1) / (count - 1);
      const y = mid - value * zoom * (rowHeight / 2 - 14);
      if (frame.envelope) {
        // A vertical min/max segment describes this pixel interval; connecting
        // sparse point samples would turn unresolved high frequencies into aliases.
        ctx.moveTo(x, y);
        ctx.lineTo(x + 0.25, mid - high * zoom * (rowHeight / 2 - 14));
      } else if (!i) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    if (s.layout === 'stacked') { ctx.fillStyle = colour(ch, light); ctx.fillText(`Ch ${ch + 1}`, 6, top + 13); }
    if (s.showClipping && clipped) { ctx.fillStyle = themeColor('--wave-clip'); ctx.fillText(`Ch ${ch + 1} CLIP`, WIDTH - 84, top + 13 + (s.layout === 'overlay' ? index * 12 : 0)); }
    ctx.restore();
  });
}

function AudioMonitorNode({ id, data, selected }: NodeProps) {
  const d = data as AudioMonitorNodeData;
  const [settings, setSettings] = useState(() => readSettings(d.settingsJson, d.settings));
  const [inputChannels, setInputChannels] = useState(0);
  const [envelope, setEnvelope] = useState(false);
  const [sampleRate, setSampleRate] = useState(48000);
  const theme = useTheme();
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, d._forceCollapsed as boolean | undefined);
  const { handleDelete } = useNodeDelete(id);
  const { setHint } = useContext(HintContext);
  const canvas = useRef<HTMLCanvasElement>(null);
  const body = useRef<HTMLDivElement>(null);
  const frame = useRef<Frame>({ channels: [], sampleRate: 48000, windowMs: 200, envelope: false });
  const scheduleDraw = useRef<() => void>(() => {});
  const settingsRef = useRef(settings); settingsRef.current = settings;
  useEffect(() => setSettings(readSettings(d.settingsJson, d.settings)), [d.settingsJson]);
  const patch = useCallback((values: Partial<AudioMonitorSettings>, commit = true) => {
    const next = { ...settingsRef.current, ...values };
    settingsRef.current = next; setSettings(next);
    if (commit) Bridge.commitSettingsChange(id, next); else Bridge.setNodeSettings(id, next);
    if ('customName' in values) Bridge.setNodeLabel(id, values.customName ?? '');
  }, [id]);
  useEffect(() => Bridge.onAudioSnapshot(snapshots => {
    const snap = snapshots.find(s => s.nodeId === id);
    if (!snap || settingsRef.current.paused) return;
    frame.current = { sampleRate: snap.sr, windowMs: snap.windowMs, envelope: snap.envelope,
      channels: snap.channels.map(text => Float32Array.from(text.split(','), value => Number(value) / 1000)) };
    setInputChannels(snap.channels.length); setSampleRate(snap.sr); setEnvelope(snap.envelope);
    scheduleDraw.current();
  }), [id]);
  const visible = visibleChannels(settings, inputChannels);
  const height = settings.layout === 'stacked' ? Math.max(1, visible.length) * 90 : 160;
  useEffect(() => {
    if (collapsed) return;
    let raf = 0;
    const render = () => {
      raf = 0;
      const ctx = canvas.current?.getContext('2d');
      if (ctx) draw(ctx, frame.current, settingsRef.current, theme === 'light', canvas.current!.height);
    };
    // Draw once per new snapshot or display-setting change. Idle/paused
    // monitors should not repaint large multichannel canvases at 60 Hz.
    scheduleDraw.current = () => { if (!raf) raf = requestAnimationFrame(render); };
    scheduleDraw.current();
    return () => { scheduleDraw.current = () => {}; cancelAnimationFrame(raf); };
  }, [collapsed, height, theme]);
  useEffect(() => { scheduleDraw.current(); }, [settings]);
  const buttonStyle = { fontSize: 10, border: '1px solid var(--border)', borderRadius: 3,
    padding: '3px 6px', background: 'var(--surface2)', color: 'var(--text-dim)', cursor: 'pointer' };
  const select = (value: string, onChange: (v: string) => void, options: { id: string; name: string }[]) =>
    <NodeSelect value={value} onChange={onChange} options={options} accent="var(--audio)" showEmpty={false} />;
  const row = (label: string, child: React.ReactNode) => <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
    <span style={{ width: 100, fontSize: 10, color: 'var(--text-dim)' }}>{label}</span><div style={{ flex: 1 }}>{child}</div>
  </div>;
  return <div style={{ width: WIDTH + 16, position: 'relative', background: 'var(--surface)',
    border: `1px solid ${selected ? 'var(--audio)' : 'var(--border)'}`, borderTop: '3px solid var(--audio)',
    borderRadius: 'var(--radius)', boxShadow: 'var(--shadow-node)', zIndex: showSettings ? 9999 : undefined }}>
    <NodeHandle nodeId={id} label="Audio In" direction="in" colour="var(--audio)" index={0} total={1} anchor="wave" portBodyRef={body} />
    <NodeHeader title={settings.customName || 'AUDIO MONITOR'} accent="var(--audio)"
      rename={{ value: settings.customName, placeholder: 'Audio Monitor', onCommit: name => patch({ customName: name }) }}
      showSettings={showSettings} onToggleSettings={toggleSettings} onDelete={handleDelete}
      collapsed={collapsed} onToggleCollapsed={toggleCollapsed}>
      <NodeHeaderButton onClick={() => patch({ paused: !settings.paused })} active={settings.paused} title={settings.paused ? 'Resume display' : 'Pause display'}>
        {settings.paused ? <Play size={14} /> : <Pause size={14} />}
      </NodeHeaderButton>
    </NodeHeader>
    {!collapsed && <div ref={body} className="nodrag" onPointerDown={e => e.stopPropagation()} style={{ padding: 7 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 5, marginBottom: 6 }}>
        <button style={{ ...buttonStyle, color: settings.layout === 'overlay' ? 'var(--audio)' : 'var(--text-muted)' }} onClick={() => patch({ layout: 'overlay' })}>Single plot</button>
        <button style={{ ...buttonStyle, color: settings.layout === 'stacked' ? 'var(--audio)' : 'var(--text-muted)' }} onClick={() => patch({ layout: 'stacked' })}>Subplots</button>
        <span style={{ marginLeft: 'auto', fontSize: 9, color: 'var(--text-muted)' }}>{inputChannels} input ch · {sampleRate} Hz</span>
      </div>
      <div data-port-anchor="wave" className="nowheel" style={{ maxHeight: 450, overflowY: 'auto' }}>
        <canvas ref={canvas} width={WIDTH} height={height} style={{ display: 'block', borderRadius: 3 }} />
      </div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 9, color: 'var(--text-muted)', marginTop: 6 }}>
        <span>Window</span><ValueBox value={settings.timeWindowMs} min={10} max={200} step={5} format={v => `${Math.round(v)} ms`}
          onLive={v => patch({ timeWindowMs: v }, false)} onCommit={v => { patch({ timeWindowMs: v }, false); Bridge.commitNodeSettings(id); }} accent="var(--audio)" width={64} />
        <button style={buttonStyle} onClick={() => patch({ selectedChannels: null })}>All</button>
        <button style={buttonStyle} onClick={() => patch({ selectedChannels: [] })}>None</button>
        {envelope && <span title="Min/max envelope. Shorten the window to resolve individual samples." style={{ marginLeft: 'auto' }}>Peak envelope</span>}
      </div>
      <div className="nowheel" style={{ display: 'flex', flexWrap: 'wrap', gap: '5px 10px', maxHeight: 105, overflowY: 'auto', paddingTop: 7 }}>
        {Array.from({ length: inputChannels }, (_, channel) => <label key={channel} style={{ display: 'flex', gap: 3, alignItems: 'center', fontSize: 10, color: colour(channel, theme === 'light'), cursor: 'pointer' }}>
          <Checkbox checked={visible.includes(channel)} accent={colour(channel, theme === 'light')} onChange={checked => patch({
            selectedChannels: checked ? [...visible, channel].sort((a, b) => a - b) : visible.filter(c => c !== channel),
          })} />Ch {channel + 1}
        </label>)}
      </div>
    </div>}
    {showSettings && <div className="nodrag" onPointerDown={e => e.stopPropagation()} style={{ position: 'absolute', left: '100%', top: 0, marginLeft: 6,
      width: 300, padding: 12, background: 'var(--surface2)', border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)', boxShadow: 'var(--shadow-panel)' }}>
      <SettingsPanelHeader title="Audio Monitor" onClose={closeSettings} onReset={() => patch(DEFAULTS)} />
      <p style={{ fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.5 }} onMouseEnter={() => setHint({ title: 'Monitor Channels', body: 'Channels follow the signal connected to the monitor input. Use the channel checkboxes to choose visible traces.' })} onMouseLeave={() => setHint(null)}>
        {inputChannels} channels received. Connect this input as a branch from the signal to observe. Single plot overlays selected channels; Subplots gives each selected channel its own axes.
      </p>
      {row('Layout', select(settings.layout, v => patch({ layout: v as AudioMonitorSettings['layout'] }), [{ id: 'overlay', name: 'Single plot' }, { id: 'stacked', name: 'Subplots' }]))}
      {row('Amplitude', select(settings.amplitudeMode, v => patch({ amplitudeMode: v as AudioMonitorSettings['amplitudeMode'] }), [{ id: 'fixed', name: 'Fixed' }, { id: 'auto', name: 'Auto' }]))}
      {row('Zoom', <ValueBox value={settings.amplitudeZoom} min={0.1} max={8} step={0.1} onCommit={v => patch({ amplitudeZoom: v })} accent="var(--audio)" />)}
      {row('Trigger', select(settings.triggerMode, v => patch({ triggerMode: v as AudioMonitorSettings['triggerMode'] }), [{ id: 'free', name: 'Free' }, { id: 'rising', name: 'Rising' }, { id: 'falling', name: 'Falling' }]))}
      {row('Trigger level', <ValueBox value={settings.triggerLevel} min={-1} max={1} step={0.01} onCommit={v => patch({ triggerLevel: v })} accent="var(--audio)" />)}
      {row('Clip warning', <Checkbox checked={settings.showClipping} onChange={v => patch({ showClipping: v })} accent="var(--audio)" />)}
      {row('Clip threshold', <ValueBox value={settings.clipThresholdDb} min={-24} max={0} step={0.5} format={v => `${v} dBFS`} onCommit={v => patch({ clipThresholdDb: v })} accent="var(--audio)" />)}
    </div>}
  </div>;
}
export default memo(AudioMonitorNode);
