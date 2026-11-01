#!/usr/bin/env node
/**
 * provision.mjs — one-time setup for the Qoder side of a Qoduck device.
 *
 * Creates (or reuses) a Qoder Cloud Agents template, identity and realtime
 * conversation, then prints the WebSocket URL the firmware needs.
 *
 *   QODER_ACCESS_TOKEN=pt-... node provision.mjs
 *   QODER_ACCESS_TOKEN=pt-... node provision.mjs --new-identity   # one identity per device
 *
 * Creating a conversation costs credits (242 at the time of writing; reconnects
 * later cost far less), so the result is cached in qca-conversation.json and
 * reused on the next run. Pass --force to create another one.
 */
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { randomUUID } from 'node:crypto';

const API = 'https://api.qoder.com/api/v1/forward';
const OUT = 'qca-conversation.json';
const TOKEN = process.env.QODER_ACCESS_TOKEN
  || (existsSync('.env') && readFileSync('.env', 'utf8').match(/QODER_ACCESS_TOKEN=(.*)/)?.[1]);

if (!TOKEN) {
  console.error('QODER_ACCESS_TOKEN is not set (or put it in .env).');
  process.exit(1);
}
const H = { Authorization: `Bearer ${TOKEN}`, 'Content-Type': 'application/json' };

/* Spoken-reply persona: the answer is read aloud, so it has to be one or two
 * short sentences with no markup, no lists and no URLs. */
const VOICE_SYSTEM = [
  'You speak to the user through a small speaker. The reply is read aloud, so answer in one or two',
  'short spoken sentences, under 60 characters. No markdown, lists, code blocks, emoji or URLs.',
  'For anything that needs live data (weather, news, prices, scores) use the web tools and answer',
  'with the conclusion in one sentence. If you cannot find it, say you did not find it.',
].join(' ');

const get = async (p) => (await fetch(API + p, { headers: H })).json();
const post = async (p, body) => (await fetch(API + p, {
  method: 'POST', headers: { ...H, 'Idempotency-Key': randomUUID() }, body: JSON.stringify(body),
})).json();

if (existsSync(OUT) && !process.argv.includes('--force')) {
  console.log(`${OUT} already exists — reusing it (pass --force to create a new conversation)`);
  console.log(readFileSync(OUT, 'utf8'));
  process.exit(0);
}

const envs = await get('/environments');
const cloud = (envs.data || []).find((e) => e.config?.type === 'cloud');
if (!cloud) { console.error('no cloud environment on this account'); process.exit(1); }

let tpl = (await get('/templates')).data?.find((t) => t.name === 'qoduck-voice');
if (!tpl) {
  tpl = await post('/templates', {
    name: 'qoduck-voice',
    description: 'Voice gadget template: short spoken answers, web tools only',
    model: { id: 'qmodel' },
    environment_id: cloud.id,
    max_tool_rounds: 6,
    system: VOICE_SYSTEM,
    tools: [{ type: 'agent_toolset_20260401', enabled_tools: ['WebSearch', 'WebFetch'] }],
  });
  if (tpl.error) { console.error('template failed:', tpl.error); process.exit(1); }
  console.log('created template', tpl.id);
} else console.log('reusing template', tpl.id);

let idn = process.argv.includes('--new-identity')
  ? null
  : (await get('/identities')).data?.find((i) => i.external_id === 'qoduck-001');
if (!idn) {
  idn = await post('/identities', { name: 'qoduck', external_id: 'qoduck-001' });
  if (idn.error) { console.error('identity failed:', idn.error); process.exit(1); }
  console.log('created identity', idn.id);
} else console.log('reusing identity', idn.id);

const conv = await post('/realtime/conversations', {
  identity_id: idn.id,
  template_id: tpl.id,
  title: `qoduck ${new Date().toISOString().slice(0, 10)}`,
  config: { audio: { output: { voice: 'longanqian' } } },
});
if (conv.error) { console.error('conversation failed:', conv.error); process.exit(1); }

const out = {
  conversation_id: conv.id,
  ws_path: `/api/v1/forward/realtime?conversation_id=${conv.id}`,
  ws_url: `wss://api.qoder.com/api/v1/forward/realtime?conversation_id=${conv.id}`,
  template_id: tpl.id,
  identity_id: idn.id,
  voice: conv.config?.audio?.output?.voice,
};
writeFileSync(OUT, JSON.stringify(out, null, 2) + '\n');

console.log(`\nwrote ${OUT}`);
console.log(`\nPaste this into Settings -> QODER on the device, or into qca_config.local.h:\n`);
console.log(`  API url      wss://api.qoder.com/api/v1/forward/realtime`);
console.log(`  conversation ${conv.id}`);
console.log(`  token        <your pt-... PAT>`);
