import { memo, useCallback, useEffect, useRef, useState } from 'react';
import type { CSSProperties } from 'react';
import type { NodeProps } from '@xyflow/react';
import { Bridge, AudioModuleStatus, RawPort } from './Bridge';
import { Checkbox, NodeHandle, NodeHeader, SettingsPanelHeader, useNodeCollapsed, useNodeDelete, useNodeSettings } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import ValueBox from './ValueBox';
import { AudioModuleSettings, EqBand, EQ_TYPES, moduleDefaults, readAudioModuleSettings, formatDisplayValue } from './AudioModulesSettings';

const titles: Record<number, string> = { 30: 'RMS', 31: 'MUX', 32: 'DEMUX', 33: 'PARAMETRIC EQ', 34: 'VALUE DISPLAY' };
const button: CSSProperties = { border: '1px solid var(--border)', background: 'var(--surface2)', color: 'var(--text-dim)',
  borderRadius: 3, padding: '4px 7px', fontSize: 10, cursor: 'pointer' };
const caption: CSSProperties = { fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.6 };

function AudioModulesNode({ id, data, selected }: NodeProps) {
  const type = Number(data.nodeType), eq = type === 33, mux = type === 31, demux = type === 32;
  const valueDisplay = type === 34, accent = valueDisplay ? 'var(--value)' : 'var(--audio)';
  const [settings, setSettings] = useState(() => readAudioModuleSettings(data.settingsJson as string));
  const settingsRef = useRef(settings); settingsRef.current = settings;
  const [status, setStatus] = useState<AudioModuleStatus | null>(null);
  const body = useRef<HTMLDivElement>(null);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, data._forceCollapsed as boolean | undefined);
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete } = useNodeDelete(id);
  useEffect(() => setSettings(readAudioModuleSettings(data.settingsJson as string)), [data.settingsJson]);
  useEffect(() => Bridge.onPortActivity(entries => {
    const entry = entries.find(e => e.id === id);
    if (entry?.audioModule) setStatus(entry.audioModule);
  }), [id]);
  const patch = useCallback((values: Partial<AudioModuleSettings>) => {
    const next = { ...settingsRef.current, ...values };
    settingsRef.current = next; setSettings(next); Bridge.commitSettingsChange(id, next);
    if ('customName' in values) Bridge.setNodeLabel(id, values.customName ?? '');
  }, [id]);
  const bandPatch = (index: number, values: Partial<EqBand>) => patch({ bands: settings.bands.map((b, i) => i === index ? { ...b, ...values } : b) });
  const inputs = ((data.ports ?? []) as RawPort[]).filter(p => p.direction === 'input');
  const outputs = ((data.ports ?? []) as RawPort[]).filter(p => p.direction === 'output');
  const rate = status?.sampleRate ?? 48000, maximumFrequency = rate * 0.49;
  const response = status?.response ?? [];
  const curve = response.map(([f, db]) => `${38 + Math.log(f / 5) / Math.log(maximumFrequency / 5) * 420},${80 - Math.max(-48, Math.min(48, status?.bypass ? 0 : db)) / 48 * 64}`).join(' ');
  const portRows = Math.max(inputs.length, outputs.length);
  return <div data-audio-module={type} style={{ width: eq ? 500 : 335, position: 'relative', border: `1px solid ${selected ? accent : 'var(--border)'}`,
    borderTop: `3px solid ${accent}`, borderRadius: 'var(--radius)', background: 'var(--surface)', boxShadow: 'var(--shadow-node)' }}>
    {inputs.map((p, i) => <NodeHandle key={p.id} nodeId={id} portId={p.id} label={p.label} direction="in" colour={p.type === 'value' ? 'var(--value)' : 'var(--audio)'} index={i} total={inputs.length} portBodyRef={body} anchor={`module-port-${i}`} />)}
    {outputs.map((p, i) => <NodeHandle key={p.id} nodeId={id} portId={p.id} label={p.label} direction="out" colour={p.type === 'value' ? 'var(--value)' : 'var(--audio)'} index={i} total={outputs.length} portBodyRef={body} anchor={`module-port-${i}`} />)}
    <NodeHeader title={String(data.customName || settings.customName || titles[type])} accent={accent} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
      rename={{ value: settings.customName, placeholder: titles[type], onCommit: customName => patch({ customName }) }}
      showSettings={showSettings} onToggleSettings={toggleSettings} onDelete={handleDelete} />
    {!collapsed && <div ref={body} className="nodrag nowheel" onPointerDown={e => e.stopPropagation()} style={{ padding: 9 }}>
      <div style={{ marginBottom: 9 }}>
        {Array.from({ length: portRows }, (_, i) => <div key={i} data-port-anchor={`module-port-${i}`} style={{ display: 'flex', justifyContent: 'space-between', minHeight: 23, fontSize: 10, color: accent }}>
          <span>{inputs[i]?.label}{inputs[i] && mux ? ` · ${status?.inputWidths[i] ?? 0} ch` : ''}</span>
          <span>{outputs[i]?.label}{outputs[i]?.type === 'audio' ? ` · ${status?.outputWidths[i] ?? 0} ch` : type === 30 ? ' · RMS values' : ''}</span>
        </div>)}
      </div>
      {valueDisplay && <>
        <div style={{ display: 'flex', alignItems: 'end', gap: 8, marginBottom: 10 }}>
          <div data-value-format style={{ flex: 1 }}><div style={caption}>Number format</div>
            <NodeSelect value={settings.displayFormat} options={[{ id: 'linear', name: 'Linear' }, { id: 'scientific', name: 'Scientific' }, { id: 'db', name: 'Amplitude dB' }]}
              onChange={displayFormat => patch({ displayFormat: displayFormat as AudioModuleSettings['displayFormat'] })} accent={accent} showEmpty={false} />
          </div>
          <div><div style={caption}>Significant digits</div><ValueBox value={settings.displayDigits} min={1} max={10} step={1} width={70}
            onCommit={displayDigits => patch({ displayDigits: Math.floor(displayDigits) })} accent={accent} /></div>
        </div>
        <div style={{ maxHeight: 300, overflowY: 'auto' }}>
          <table style={{ width: '100%', textAlign: 'right', fontSize: 12, color: 'var(--text)', fontVariantNumeric: 'tabular-nums' }}>
            <thead style={caption}><tr><th>Channel / Key</th><th>Value</th></tr></thead>
            <tbody>{(status?.displayValues ?? []).map(({ key, value }) => <tr key={key}>
              <td title={`Value key: ${key}`}>{key >= 1 && key <= 64 ? `Ch ${key}` : `Key ${key}`}</td>
              <td data-display-key={key} style={{ padding: '5px 2px', color: accent }}>{formatDisplayValue(value, settings.displayFormat, settings.displayDigits)}</td>
            </tr>)}</tbody>
          </table>
        </div>
        {!status?.displayValues?.length && <p role="status" style={{ ...caption, padding: '10px 0' }}>Waiting for numeric values. Connect Value Out → Value In.</p>}
        <p style={caption}>Last received values, grouped by channel/key. Connect RMS Value Out to read each channel here.</p>
        {settings.displayFormat === 'db' && <p style={caption}>20·log₁₀(value), reference 1; RMS amplitudes give dBFS. Zero shows −∞; negative values cannot be converted.</p>}
        {status?.valueOverflow && <p role="alert" style={{ ...caption, color: 'var(--wave-trigger)' }}>Showing the first 64 unique keys. Additional keys are ignored.</p>}
      </>}
      {type === 30 && <>
        <div style={{ display: 'flex', gap: 10, alignItems: 'center', marginBottom: 8 }}><span style={caption}>RMS window</span>
          <ValueBox value={settings.windowMs} min={1} max={2000} step={1} width={80} format={v => `${v} ms`} onCommit={windowMs => patch({ windowMs })} accent="var(--audio)" />
        </div>
        <div style={{ maxHeight: 280, overflowY: 'auto' }}>
          <table style={{ width: '100%', fontSize: 11, textAlign: 'right', color: 'var(--text)' }}><thead style={caption}><tr><th>Channel</th><th>RMS (linear)</th><th>dBFS</th></tr></thead>
            <tbody>{(status?.rms ?? []).map((value, i) => <tr key={i}><td>Ch {i + 1}</td><td data-rms-linear={i}>{status?.rmsReady ? value.toPrecision(6) : '—'}</td>
              <td>{!status?.rmsReady ? '—' : value > 0 ? (20 * Math.log10(value)).toFixed(2) : '−∞'}</td></tr>)}</tbody></table>
        </div>
        <p style={caption}>{!status?.inputChannels ? 'Connect an audio signal.' : !status.rmsReady ? 'Collecting a complete RMS window…' : `${status.inputChannels} channels · ${status.windowSamples} samples per window`}</p>
        <p style={caption}>Value Out carries one RMS value per channel, keyed Ch 1, Ch 2, …</p>
      </>}
      {mux && <>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}><span style={caption}>Input ports</span>
          <ValueBox value={settings.portCount} min={1} max={64} step={1} width={65} onCommit={portCount => patch({ portCount: Math.floor(portCount) })} accent="var(--audio)" /></div>
        <p style={caption}>Concatenates input channels in port order. Unconnected ports add no channels. Inputs sharing one port mix before concatenation.</p>
        {status?.valid === false && <p role="alert" style={{ ...caption, color: 'var(--wave-trigger)' }}>Combined input exceeds 64 channels. Reduce the input widths; output is muted.</p>}
      </>}
      {demux && <>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}><span style={caption}>Output ports</span>
          <ValueBox value={settings.widths.length} min={1} max={64} step={1} width={60} onCommit={count => patch({ widths: Array.from({ length: Math.floor(count) }, (_, i) => settings.widths[i] ?? 1) })} accent="var(--audio)" />
          <button style={button} disabled={!status?.inputChannels} onClick={() => patch({ widths: Array(status!.inputChannels).fill(1) })}>Split to mono</button>
        </div>
        <div style={{ maxHeight: 230, overflowY: 'auto', marginTop: 7 }}>
          {settings.widths.map((width, i) => <div key={i} style={{ display: 'flex', gap: 8, alignItems: 'center', marginBottom: 5 }}><span style={caption}>Out {i + 1} channels</span>
            <ValueBox value={width} min={1} max={64} step={1} width={65} onCommit={value => patch({ widths: settings.widths.map((w, j) => j === i ? Math.floor(value) : w) })} accent="var(--audio)" /></div>)}
        </div>
        <p style={caption}>Splits consecutive channel ranges in output-port order. Output widths must total the input width.</p>
        {status?.valid === false && <p role="alert" style={{ ...caption, color: 'var(--wave-trigger)' }}>Input: {status.inputChannels} ch · configured outputs: {status.requiredChannels} ch. Match these totals to enable output.</p>}
      </>}
      {eq && <>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}><Checkbox checked={settings.bypass} label="Bypass EQ" onChange={bypass => patch({ bypass })} accent="var(--audio)" />
          <span style={{ ...caption, marginLeft: 'auto' }}>Output gain</span><ValueBox value={settings.outputGainDb} min={-24} max={24} step={0.1} width={65} format={v => `${v.toFixed(1)} dB`} onCommit={outputGainDb => patch({ outputGainDb })} accent="var(--audio)" /></div>
        <svg viewBox="0 0 478 160" role="img" aria-label="EQ frequency response" style={{ width: '100%', background: 'var(--scope-bg)', borderRadius: 3 }}>
          {[-48, -24, 0, 24, 48].map(db => <g key={db}><line x1="38" x2="458" y1={80 - db / 48 * 64} y2={80 - db / 48 * 64} stroke="var(--plot-grid)" /><text x="2" y={83 - db / 48 * 64} fill="var(--plot-label)" fontSize="9">{db} dB</text></g>)}
          {[10, 100, 1000, 10000].filter(f => f < maximumFrequency).map(f => <text key={f} x={38 + Math.log(f / 5) / Math.log(maximumFrequency / 5) * 420} y="157" fill="var(--plot-label)" fontSize="9">{f >= 1000 ? `${f / 1000}k` : f}</text>)}
          <polyline points={curve || '38,80 458,80'} fill="none" stroke="var(--audio)" strokeWidth="1.5" />
        </svg>
        <div style={{ display: 'grid', gridTemplateColumns: '28px 120px 100px 82px 65px', gap: 5, alignItems: 'center', marginTop: 9, fontSize: 10, color: 'var(--text-muted)' }}>
          <span>On</span><span>Filter</span><span>Frequency (Hz)</span><span>Gain (dB)</span><span>Q</span>
          {settings.bands.map((b, i) => <EqBandRow key={i} band={b} index={i} maxFrequency={maximumFrequency} onChange={values => bandPatch(i, values)} />)}
        </div>
        <p style={caption}>{status?.inputChannels ?? 0} channels · {rate} Hz · frequencies limited to {maximumFrequency.toFixed(0)} Hz at this sample rate. Each channel has independent filter state.</p>
      </>}
      {showSettings && <div style={{ borderTop: '1px solid var(--border)', marginTop: 9, paddingTop: 7 }}>
        <SettingsPanelHeader title={titles[type]} onClose={closeSettings} onReset={() => patch(moduleDefaults())} />
        <p style={caption}>Settings are saved with the project and support undo/redo.{mux || demux ? ' Reducing routing port counts removes connections on deleted ports.' : ''}</p>
      </div>}
    </div>}
  </div>;
}
function EqBandRow({ band, index, maxFrequency, onChange }: { band: EqBand; index: number; maxFrequency: number; onChange: (v: Partial<EqBand>) => void }) {
  return <>
    <Checkbox label={String(index + 1)} checked={band.enabled} onChange={enabled => onChange({ enabled })} accent="var(--audio)" />
    <NodeSelect value={band.type} options={EQ_TYPES} onChange={type => onChange({ type })} accent="var(--audio)" showEmpty={false} />
    <ValueBox value={Math.min(band.frequency, maxFrequency)} min={5} max={maxFrequency} step={1} width={90} onCommit={frequency => onChange({ frequency })} accent="var(--audio)" />
    <ValueBox value={band.gainDb} min={-24} max={24} step={0.1} width={75} disabled={band.type === 'lowPass' || band.type === 'highPass'} onCommit={gainDb => onChange({ gainDb })} accent="var(--audio)" />
    <ValueBox value={band.q} min={0.1} max={20} step={0.05} width={60} onCommit={q => onChange({ q })} accent="var(--audio)" />
  </>;
}
export default memo(AudioModulesNode);
