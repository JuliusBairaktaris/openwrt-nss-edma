// node .github/workflows/scripts/issue-triage.test.js
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { parseForm, triage, F } = require('./issue-triage.js');

const form = fs.readFileSync(path.join(__dirname, '../../ISSUE_TEMPLATE/bug.yml'), 'utf8');

// The one failure this cannot recover from at runtime: a label reworded in the
// form, leaving triage reading a key that no issue will ever contain.
const labels = [...form.matchAll(/^\s+label:\s*(.+)$/gm)]
	.map((m) => m[1].trim().toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, ''));
for (const [k, v] of Object.entries(F))
	assert.ok(labels.includes(v), `bug.yml has no field labelled "${v}" (F.${k})`);

// Dropdown answers the routing rules match on must still be offerable.
for (const opt of ['qosmio/openwrt-ipq image', 'Official OpenWrt image (no NSS)', 'Another NSS build',
	'Yes — and an official OpenWrt image is broken the same way', 'Not tested yet'])
	assert.ok(form.includes(opt), `bug.yml no longer offers "${opt}"`);

const good = {
	[F.image]: 'Release image from this repo',
	[F.gate]: 'No — with the offload off the box is fine',
	[F.state]: '```text\n{"kernel":"6.18.1",...}\nnss.general.enabled=\'1\'\nNSS offload status: up\n```',
	[F.what]: 'The WAN port stops transmitting after a few hours and never comes back.',
	[F.steps]: '1. Flash the release image\n2. Enable SQM on wan\n3. Run iperf3 -c host -R for an hour',
	[F.expected]: 'Traffic keeps flowing.',
	[F.regression]: 'v6.18-r3',
	[F.logs]: '```text\n' + 'kern.err ... something went wrong\n'.repeat(20) + '```',
	[F.netcfg]: "```text\nconfig interface 'wan'\n\toption proto 'pppoe'\n```",
};
const body = (over = {}) => Object.entries({ ...good, ...over })
	.map(([k, v]) => `### ${k.replace(/_/g, ' ')}\n\n${v}`).join('\n\n');

// parseForm: sections, fences and unanswered optionals
const p = parseForm(body({ [F.diffconfig]: '_No response_' }));
assert.strictEqual(p[F.diffconfig], '');
assert.ok(p[F.state].startsWith('{"kernel"'), 'code fence not stripped');

assert.strictEqual(triage('not an issue form at all'), null);
assert.strictEqual(triage(body()).verdict, 'ok', JSON.stringify(triage(body()).comment));

// Routing: the answer, not the symptom, decides the repo.
for (const [field, answer] of [
	[F.image, 'qosmio/openwrt-ipq image'],
	[F.image, 'Official OpenWrt image (no NSS)'],
	[F.image, 'Another NSS build'],
	[F.gate, 'Yes — and an official OpenWrt image is broken the same way'],
]) {
	const r = triage(body({ [field]: answer }));
	assert.strictEqual(r.verdict, 'route', `${answer} should route away`);
	assert.ok(r.close && r.add.includes('upstream'));
}
// ...and the sibling answers must not.
for (const answer of ['Yes — but an official OpenWrt image is fine',
	'Yes — this device has no official OpenWrt image to compare against',
	'Not applicable — the bug is that the offload itself will not come up'])
	assert.strictEqual(triage(body({ [F.gate]: answer })).verdict, 'ok', answer);

const incomplete = (over, needle) => {
	const r = triage(body(over));
	assert.strictEqual(r.verdict, 'incomplete', `${needle}: expected incomplete`);
	assert.ok(r.add.includes('needs-info') && !r.close);
	assert.ok(r.comment.includes(needle), `${needle}: not named in\n${r.comment}`);
};

incomplete({ [F.gate]: 'Not tested yet' }, 'Whose bug this is');
incomplete({ [F.state]: 'see attached screenshot' }, '`ubus call system board`');
incomplete({ [F.state]: good[F.state].replace('NSS offload status: up', '') }, '`nss-status -d`');
incomplete({ [F.image]: 'Self-built from this repo (nss-edma-rework)' }, 'Diffconfig');
incomplete({ [F.what]: 'N/A' }, 'What happens');
incomplete({ [F.steps]: 'browsing the web' }, 'Steps to reproduce');
incomplete({ [F.regression]: '' }, 'Last image that worked');
incomplete({ [F.logs]: 'nothing in the log' }, 'Logs');
incomplete({ [F.netcfg]: 'default config' }, 'Network and wireless config');
incomplete({ [F.what]: 'The router panics after a day of uptime.' }, 'The crash log');

// A crash report that brought its ramoops is complete.
assert.strictEqual(triage(body({
	[F.what]: 'The router panics after a day of uptime.',
	[F.logs]: `${good[F.logs]}\nattached /root/pstore/20260803-abc/console-ramoops-0`,
})).verdict, 'ok');

// 11.4 / mesh reports stay visibly as-is, complete or not.
assert.ok(triage(body({ [F.what]: 'Mesh peers drop after a reconnect.' })).add.includes('as-is'));
assert.ok(!triage(body()).add.includes('as-is'));

console.log('issue-triage: ok');
