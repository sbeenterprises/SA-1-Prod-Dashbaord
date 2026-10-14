#!/usr/bin/env node
// Runs every ECharts instrument's getOption() from the generated dashboards
// in Node with a mocked Grafana context, the way volkovlabs-echarts-panel
// calls it: once with no data (vessel offline) and once with sample frames.
// Each call must return an option object with a series array, without
// throwing. The Traffic View gets extra checks for the contact symbols, the
// speed vectors, the own-ship pin and the rings on contacts the helm avoids.
//
//   node tools/check_echarts_panels.js [repo]     exit 1 on any failure
'use strict';
const fs = require('fs');
const path = require('path');

const repo = process.argv[2] || path.join(__dirname, '..');
const dir = path.join(repo, 'grafana', 'dashboards');
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  [PASS] ' + m); };
const bad = (m) => { fail++; console.log('  [FAIL] ' + m); };

function frame(refId, cols) {
  return { refId, fields: Object.entries(cols).map(([name, values]) => ({
    name, type: typeof values.find((v) => v !== null) === 'number' ? 'number' : 'string',
    values: { toArray: () => values } })) };
}
function context(series, isDark) {
  return { grafana: { theme: { isDark } }, panel: { data: { series } } };
}
// Every numeric refId A..H gets one plausible value.
function genericSeries() {
  return 'ABCDEFGH'.split('').map((r, i) => frame(r, { _value: [10 + i * 7] }));
}
const TRAFFIC = [
  frame('A', { contact: ['AIS 367123', 'R1', 'R2', 'AIS 338991', 'NOBRG'],
    source: ['report', 'radar', 'radar', 'report', 'report'],
    range_m: [1500, 800, 2500, 9000, 400], rel_bearing: [45, 300, 180, 250, null],
    hdg: [270, null, 0, 120, 10], spd_kn: [12, 5, 0.2, null, 3],
    risk: [2, 1, 0, 0, 0], cpa_m: [100, 400, 2000, 1500, 300], tcpa_s: [120, 300, -1, 600, 50] }),
  frame('B', { _value: [90] }),
  // Helm avoiding (COLAV_ALERTS): AIS 367123 in extremis, R1 avoided.
  frame('V', { contact: ['AIS 367123', 'R1'], active: [1, 1], extremis: [1, 0] }),
];

const files = fs.readdirSync(dir).filter((f) => f.endsWith('.json')).sort();
let panels = 0;
for (const f of files) {
  const d = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
  for (const p of d.panels || []) {
    if (p.type !== 'volkovlabs-echarts-panel') { continue; }
    panels++;
    const code = p.options && p.options.getOption;
    const name = `${f} / ${p.title}`;
    let fn;
    try { fn = new Function('context', code); } catch (e) { bad(`${name}: syntax ${e.message}`); continue; }
    const isTraffic = code.includes('HEADING UP');
    const cases = [['no data', []], ['sample data', isTraffic ? TRAFFIC : genericSeries()]];
    for (const dark of [true, false]) {
      for (const [label, series] of cases) {
        try {
          const o = fn(context(series, dark));
          if (!o || !Array.isArray(o.series)) { throw new Error('no series array'); }
          if (isTraffic && label === 'sample data') { checkTraffic(o, name, dark); }
        } catch (e) { bad(`${name} (${label}, ${dark ? 'dark' : 'light'}): ${e.message}`); }
      }
    }
  }
}
ok(`${panels} ECharts panels return an option (no data and sample data, dark and light)`);

function checkTraffic(o, name, dark) {
  const tag = dark ? 'dark' : 'light';
  const contacts = o.series[0];
  if (!/^path:\/\//.test(contacts.symbol)) { throw new Error('contacts are not hull paths'); }
  // NOBRG has no relative bearing: not plotted.
  if (contacts.data.length !== 4) { throw new Error(`expected 4 plotted contacts, got ${contacts.data.length}`); }
  const byName = Object.fromEntries(contacts.data.map((c) => [c.name, c]));
  // Heading-up: AIS 367123 heading 270 with own heading 90 -> pointing astern.
  if (byName['AIS 367123'].symbolRotate !== -180) { throw new Error('hull rotation wrong'); }
  if (byName['AIS 367123'].itemStyle.color !== '#ff6b6b') { throw new Error('HIGH risk not red'); }
  if (byName['R2'].itemStyle.color !== '#f0b429') { throw new Error('radar not amber'); }
  if (byName['AIS 338991'].itemStyle.color !== '#5ad8a6') { throw new Error('AIS not green'); }
  // Vectors: AIS 367123 (12 kn) and R2 is 0.2 kn (none), R1 has no heading
  // (none), AIS 338991 has no speed (none) -> exactly one vector.
  const vec = o.series.filter((s) => s.type === 'line' && s.lineStyle && s.lineStyle.type !== 'dashed');
  if (vec.length !== 1) { throw new Error(`expected 1 speed vector, got ${vec.length}`); }
  const [[r0, a0], [r1, a1]] = vec[0].data;
  // From 0.81 nm @ 045, heading 180 relative, 6 min at 12 kn = 1.2 nm,
  // kept between 6 % and 40 % of the display range.
  const maxR = o.radiusAxis.max;
  const len = Math.min(Math.max(1.2, maxR * 0.06), maxR * 0.4);
  const x = r0 * Math.sin(a0 * Math.PI / 180), y = r0 * Math.cos(a0 * Math.PI / 180);
  const x1 = r1 * Math.sin(a1 * Math.PI / 180), y1 = r1 * Math.cos(a1 * Math.PI / 180);
  if (Math.abs(x1 - x) > 1e-9 || Math.abs((y - y1) - len) > 1e-9) { throw new Error('speed vector geometry wrong'); }
  const own = o.series.find((s) => s.z === 5);
  if (!own || !/^image:\/\/data:image\/svg\+xml/.test(own.symbol)) { throw new Error('own-ship pin missing'); }
  const svg = decodeURIComponent(own.symbol.split(',').slice(1).join(','));
  if (!svg.startsWith('<svg') || !svg.endsWith('</svg>')) { throw new Error('own-ship pin SVG malformed'); }
  if (own.symbolOffset[1] !== '-50%') { throw new Error('pin tip not on the centre'); }
  const cpa = o.series.find((s) => s.lineStyle && s.lineStyle.type === 'dashed');
  if (!cpa) { throw new Error('CPA line missing'); }
  const ring = o.series.find((s) => s.symbol === 'circle');
  if (!ring || ring.data.length !== 2) { throw new Error('helm-avoiding rings missing'); }
  const rc = ring.data.map((d) => d.itemStyle.borderColor).sort().join(',');
  if (rc !== '#ff4d4f,#ff9830') { throw new Error(`ring colours wrong: ${rc}`); }
  if (!byName['R1'].label.show) { throw new Error('avoided contact not labelled'); }
  ok(`Traffic View symbols, vectors, pin, CPA line and avoidance rings (${tag})`);
}

console.log(`  echarts panels: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
