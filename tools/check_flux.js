#!/usr/bin/env node
// Type-checks every distinct Flux query in the generated dashboards and
// alert rules with the Flux language server's semantic checker
// (@influxdata/flux-lsp-node: npm i -g @influxdata/flux-lsp-node).
//   node tools/check_flux.js [repo] [alert_queries.json]
// Grafana variables are replaced by a vessel name and v.* by a time range,
// so a query fails only on real Flux errors (bad types, unknown functions,
// wrong argument names). Exit 2 when the checker is not installed.
'use strict';
const fs = require('fs');
const path = require('path');
let Flux;
try { ({ Flux } = require('@influxdata/flux-lsp-node')); } catch (e) { process.exit(2); }

const repo = process.argv[2] || path.join(__dirname, '..');
const queries = new Map();
const dir = path.join(repo, 'grafana', 'dashboards');
for (const f of fs.readdirSync(dir).filter((x) => x.endsWith('.json'))) {
  const d = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
  for (const p of d.panels || []) {
    for (const t of p.targets || []) {
      if (t.query && !queries.has(t.query)) { queries.set(t.query, `${f} / ${p.title} / ${t.refId}`); }
    }
  }
  // Dashboard variables backed by a Flux query (engine_type, engine_model ...).
  for (const v of (d.templating && d.templating.list) || []) {
    const q = typeof v.query === 'string' ? v.query : (v.query && v.query.query);
    if (v.type === 'query' && q && q.includes('|>') && !queries.has(q)) {
      queries.set(q, `${f} / variable ${v.name}`);
    }
  }
}
// Alert rule queries: a JSON list (the smoke test extracts it from alerts.yml).
if (process.argv[3]) {
  for (const q of JSON.parse(fs.readFileSync(process.argv[3], 'utf8'))) {
    if (!queries.has(q)) { queries.set(q, 'alerts.yml'); }
  }
}

function prep(q) {
  q = q.replace(/\$\{\w+(:\w+)?\}/g, 'usv-smoke');
  const imports = [];
  q = q.replace(/^import .*$/mg, (m) => { imports.push(m); return ''; });
  return imports.join('\n') + '\nv = {timeRangeStart: -1h, timeRangeStop: now(), windowPeriod: 10s}\n' + q;
}
let bad = 0;
for (const [q, where] of queries) {
  let ok = false;
  try { ok = new Flux(prep(q)).is_valid(); } catch (e) { ok = false; }
  if (!ok) { bad++; console.log('  [FAIL] Flux does not type-check: ' + where); }
}
// The checker must reject a known-bad query, or the run proves nothing.
if (new Flux('import "math"\nx = math.cos(y: 1.0)').is_valid()) { bad++; console.log('  [FAIL] checker accepted a bad query'); }
console.log(`  ${queries.size} distinct Flux queries checked, ${bad} failed`);
process.exit(bad ? 1 : 0);
