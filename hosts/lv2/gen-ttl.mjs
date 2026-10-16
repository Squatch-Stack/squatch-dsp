#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// LV2 bundles from the control contracts (hosts/lv2/contracts/*.params.json, profile
// cc.squatch.control/1). The control ports map exactly as squatch-skin's lib/contract.mjs toLv2()
// maps them (index in contract order, key = lv2:symbol, units, toggled/integer), and then this adds
// what a loadable plugin needs and that fragment leaves out:
//   - audio in/out ports after the control ports, doap/project/licence, lv2:Class
//   - lv2:enumeration + lv2:scalePoint from "choices"; pprops:trigger from "trigger": true;
//     pprops:notOnGUI from "hidden": true; units:midiNote from unit "midinote" (the last three are
//     extensions to the skin schema, which should adopt them when the contracts move there)
//   - required features and the state interface, from product.lv2Features / product.lv2State
//   - manifest.ttl, and a C++ header of port indices and ranges the plugin is built against.
//
//   node hosts/lv2/gen-ttl.mjs            write <bundle>.lv2/*.ttl and <bundle>_ports.hpp
//   node hosts/lv2/gen-ttl.mjs --check    fail if any generated file is stale
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PROFILE = 'cc.squatch.control/1';
const VALUE_ROLES = new Set(['param', 'bypass', 'meter']);
const UNITS = { db: 'units:db', hz: 'units:hz', ms: 'units:ms', s: 'units:s', pc: 'units:pc', cent: 'units:cent',
  midinote: 'units:midiNote' };
const PREFIXES = {
  lv2: 'http://lv2plug.in/ns/lv2core#', units: 'http://lv2plug.in/ns/extensions/units#',
  pprops: 'http://lv2plug.in/ns/ext/port-props#', doap: 'http://usefulinc.com/ns/doap#',
  foaf: 'http://xmlns.com/foaf/0.1/', rdf: 'http://www.w3.org/1999/02/22-rdf-syntax-ns#',
  rdfs: 'http://www.w3.org/2000/01/rdf-schema#', urid: 'http://lv2plug.in/ns/ext/urid#',
  state: 'http://lv2plug.in/ns/ext/state#',
};
const CONTRACTS = path.join(HERE, 'contracts');
// Every contract here becomes a bundle of the same name: contracts/<bundle>.params.json.
const bundles = () => fs.readdirSync(CONTRACTS).filter((f) => f.endsWith('.params.json')).map((f) => f.slice(0, -12)).sort();

/** Problems with a contract, as squatch-skin checkDescriptor() reports them, plus our extensions. */
export function check(src) {
  const out = src.profile === PROFILE ? [] : [`profile must be ${PROFILE}`];
  const ids = new Set();
  const keys = new Set();
  for (const p of src.params ?? []) {
    const at = `param ${p.key ?? p.id}`;
    if (!fresh(p, ids, keys)) out.push(`${at}: id and key must be unique (an integer and an LV2 symbol)`);
    if (VALUE_ROLES.has(p.role)) out.push(...VALUE_RULES.filter(([bad]) => bad(p)).map(([, why]) => `${at}: ${why}`));
  }
  return out;
}

// A new integer id and a new LV2 symbol; records both.
function fresh(p, ids, keys) {
  const ok = Number.isInteger(p.id) && !ids.has(p.id) && /^[A-Za-z_][A-Za-z0-9_]*$/.test(p.key ?? '') && !keys.has(p.key);
  ids.add(p.id);
  keys.add(p.key);
  return ok;
}

const inRange = (p, v) => v >= p.min && v <= p.max;
// [is it wrong?, what to say] for each value parameter (param, bypass or meter).
const VALUE_RULES = [
  [(p) => !(p.min < p.max), 'needs min < max'],
  [(p) => !inRange(p, p.default), 'default outside [min, max]'],
  [(p) => p.unit != null && !(p.unit in UNITS), 'unit has no LV2 unit'],
  [(p) => (p.choices ?? []).some((c) => !inRange(p, c.value)), 'a choice is out of range'],
  [(p) => p.trigger && p.role !== 'param', 'only an input can be a trigger'],
];

const num = (v) => (Number.isInteger(v) ? `${v}.0` : `${v}`);
const str = (s) => `"${String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
const toggled = (p) => p.steps === 1 && p.min === 0 && p.max === 1;

function portProps(p) {
  const props = [];
  if (toggled(p)) props.push('lv2:toggled');
  else if (p.steps > 1) props.push('lv2:integer');
  if (p.choices?.length) props.push('lv2:enumeration');
  if (p.taper === 'log') props.push('pprops:logarithmic');
  if (p.trigger) props.push('pprops:trigger');
  if (p.hidden) props.push('pprops:notOnGUI');
  return props;
}

function controlPort(p, index) {
  const io = p.role === 'meter' ? 'OutputPort' : 'InputPort';
  const lines = [`a lv2:${io}, lv2:ControlPort`, `lv2:index ${index}`, `lv2:symbol ${str(p.key)}`, `lv2:name ${str(p.name)}`,
    `lv2:default ${num(p.default)}`, `lv2:minimum ${num(p.min)}`, `lv2:maximum ${num(p.max)}`];
  if (p.unit) lines.push(`units:unit ${UNITS[p.unit]}`);
  const props = portProps(p);
  if (props.length) lines.push(`lv2:portProperty ${props.join(', ')}`);
  for (const c of p.choices ?? []) lines.push(`lv2:scalePoint [ rdfs:label ${str(c.label)} ; rdf:value ${num(c.value)} ]`);
  if (p.note) lines.push(`rdfs:comment ${str(p.note)}`);
  return lines;
}

function audioPort(io, index, symbol, name) {
  return [`a lv2:${io}, lv2:AudioPort`, `lv2:index ${index}`, `lv2:symbol ${str(symbol)}`, `lv2:name ${str(name)}`];
}

/** Ports in index order: the contract's value parameters, then mono audio in and out. */
export function ports(src) {
  const values = src.params.filter((p) => VALUE_ROLES.has(p.role) && p.role !== 'bypass');
  return values.map((p, i) => ({ p, index: i, lines: controlPort(p, i) }))
    .concat([{ audio: 'in', index: values.length, lines: audioPort('InputPort', values.length, 'in', 'In') },
      { audio: 'out', index: values.length + 1, lines: audioPort('OutputPort', values.length + 1, 'out', 'Out') }]);
}

function header(uses) {
  return uses.map((k) => `@prefix ${k}: <${PREFIXES[k]}> .`).join('\n');
}

export function pluginTtl(src) {
  const out = [header(['lv2', 'units', 'pprops', 'doap', 'foaf', 'rdf', 'rdfs', 'urid', 'state']), '',
    '<urn:squatch-stack:lv2>', '  a doap:Project ;', '  doap:name "Squatch Stack LV2" ;',
    '  doap:license <https://spdx.org/licenses/MIT> ;',
    '  doap:maintainer [ a foaf:Person ; foaf:name "Squatch Stack" ; foaf:homepage <https://github.com/squatch-stack> ] .', '',
    `<${src.product.lv2}>`, `  a lv2:Plugin, lv2:${src.product.lv2Class} ;`, `  doap:name ${str(src.product.name)} ;`,
    '  doap:license <https://spdx.org/licenses/MIT> ;', '  lv2:project <urn:squatch-stack:lv2> ;',
    '  lv2:minorVersion 1 ;', '  lv2:microVersion 0 ;', `  rdfs:comment ${str(src.product.comment ?? src.product.name)} ;`,
    '  lv2:optionalFeature lv2:hardRTCapable ;'];
  for (const f of src.product.lv2Features ?? []) out.push(`  lv2:requiredFeature ${f} ;`);
  if (src.product.lv2State) out.push('  lv2:extensionData state:interface ;');
  const blocks = ports(src).map((x) => `    [\n      ${x.lines.join(' ;\n      ')}\n    ]`);
  out.push(`  lv2:port\n${blocks.join(' ,\n')} .`, '');
  return out.join('\n');
}

export function manifestTtl(src, bundle) {
  return [header(['lv2', 'rdfs']), '', `<${src.product.lv2}>`, '  a lv2:Plugin ;', `  lv2:binary <${bundle}.so> ;`,
    `  rdfs:seeAlso <${bundle}.ttl> .`, ''].join('\n');
}

function cppName(key) {
  return `k${key.split('_').map((w) => w[0].toUpperCase() + w.slice(1)).join('')}`;
}

export function portsHpp(src, bundle) {
  const list = ports(src);
  const ns = bundle.replace(/^squatch-/, '').replace(/-/g, '_');
  const rows = list.map((x) => (x.audio
    ? `    {"${x.audio}", 0.0f, 0.0f, 0.0f, ${x.audio === 'in'}, true},`
    : `    {"${x.p.key}", ${num(x.p.min)}f, ${num(x.p.max)}f, ${num(x.p.default)}f, ${x.p.role !== 'meter'}, false},`));
  const names = list.map((x) => `  ${x.audio ? (x.audio === 'in' ? 'kAudioIn' : 'kAudioOut') : cppName(x.p.key)} = ${x.index},`);
  return ['// SPDX-License-Identifier: MIT', `// Generated by hosts/lv2/gen-ttl.mjs from contracts/${bundle}.params.json. Do not edit.`,
    '#pragma once', '', `namespace squatch::lv2::${ns} {`, '', `constexpr const char* kUri = "${src.product.lv2}";`, '',
    'enum Port : unsigned {', ...names, `  kPortCount = ${list.length},`, '};', '',
    'struct PortInfo {', '  const char* symbol;', '  float min, max, def;', '  bool input, audio;', '};', '',
    'constexpr PortInfo kPorts[kPortCount] = {', ...rows, '};', '', `}  // namespace squatch::lv2::${ns}`, ''].join('\n');
}

function outputs(src, bundle) {
  return {
    [`${bundle}.lv2/${bundle}.ttl`]: pluginTtl(src),
    [`${bundle}.lv2/manifest.ttl`]: manifestTtl(src, bundle),
    [`${bundle}_ports.hpp`]: portsHpp(src, bundle),
  };
}

function emit(rel, body, checkOnly) {
  const p = path.join(HERE, rel);
  if (checkOnly) return fs.existsSync(p) && fs.readFileSync(p, 'utf8') === body ? true : (console.log(`stale hosts/lv2/${rel}`), false);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, body);
  return true;
}

function main() {
  const checkOnly = process.argv.includes('--check');
  let ok = true;
  for (const bundle of bundles()) {
    const src = JSON.parse(fs.readFileSync(path.join(CONTRACTS, `${bundle}.params.json`), 'utf8'));
    const problems = check(src);
    if (problems.length) {
      console.log(`${bundle}:\n  ${problems.join('\n  ')}`);
      ok = false;
      continue;
    }
    for (const [rel, body] of Object.entries(outputs(src, bundle))) ok = emit(rel, body, checkOnly) && ok;
    console.log(`${bundle}: ${ports(src).length} ports`);
  }
  process.exit(ok ? 0 : 1);
}

main();
