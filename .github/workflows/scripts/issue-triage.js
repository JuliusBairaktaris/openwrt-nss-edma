// Triage for the bug form: routes reports that belong to another project and
// names what a report is missing. GitHub's own form validation covers "is this
// field empty"; everything here is what it cannot see - which project the bug
// belongs to, and whether an answer is an answer.
//
// The keys below are the labels from ISSUE_TEMPLATE/bug.yml, lowercased with
// runs of non-alphanumerics collapsed to "_". Change a label there, change it
// here, and issue-triage.test.js will say so.

const F = {
	image: 'which_image',
	gate: 'does_it_still_happen_with_the_offload_off',
	state: 'build_and_runtime_state',
	diffconfig: 'diffconfig',
	what: 'what_happens',
	steps: 'steps_to_reproduce',
	expected: 'what_you_expected_instead',
	regression: 'last_image_that_worked',
	logs: 'logs',
	netcfg: 'network_and_wireless_config',
};

const OPENWRT = 'https://github.com/openwrt/openwrt/issues';

// Each rule closes the issue: the answer says the reporter is not on this tree,
// or that the bug survives without it. Order matters only for the message.
const ROUTES = [
	{
		field: F.image, match: /qosmio/i,
		to: 'the qosmio/openwrt-ipq tracker', url: 'https://github.com/qosmio/openwrt-ipq/issues',
		why: 'that is a different tree with a different driver stack, not this one',
	},
	{
		field: F.image, match: /official openwrt image/i,
		to: 'OpenWrt', url: OPENWRT,
		why: 'an official image carries none of this tree\'s code, so nothing here can be its cause',
	},
	{
		field: F.image, match: /another nss build/i,
		to: 'that build\'s own tracker', url: null,
		why: 'only images built from this tree are supported here',
	},
	{
		field: F.gate, match: /official openwrt image is broken the same way/i,
		to: 'OpenWrt', url: OPENWRT,
		why: 'a bug that reproduces on an official image is an OpenWrt bug - this tree only adds the offload path on top of it',
	},
];

const JUNK = /^(n\/?a|none|nothing|idk|i don'?t know|unknown|\?+|-+|\.+|test|asdf|see (the )?(above|title|logs?|attachment))\.?$/i;

const norm = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '');

// Issue-form bodies are "### <label>\n\n<value>" sections; a render: text field
// arrives fenced. A pasted log line starting with "### " would split a section
// early - it has not happened, and the cost is one spurious missing-field note.
function parseForm(body) {
	const out = {};
	for (const part of String(body || '').split(/^###[ \t]+/m).slice(1)) {
		const nl = part.indexOf('\n');
		const label = (nl < 0 ? part : part.slice(0, nl)).trim();
		let value = (nl < 0 ? '' : part.slice(nl + 1)).trim();
		value = value.replace(/^```[a-z]*\n?/i, '').replace(/```$/, '').trim();
		if (value === '_No response_') value = '';
		out[norm(label)] = value;
	}
	return out;
}

function triage(body) {
	const f = parseForm(body);
	if (!(F.image in f)) return null; // not the bug form

	for (const r of ROUTES) {
		if (!r.match.test(f[r.field] || '')) continue;
		const where = r.url ? `[${r.to}](${r.url})` : r.to;
		return {
			verdict: 'route',
			add: ['upstream'],
			remove: ['needs-triage', 'needs-info', 'no-response'],
			close: true,
			comment: `**This belongs to ${where}, not here.**\n\n` +
				`You answered *${f[r.field]}*, and ${r.why}.\n\n` +
				`Closing for that reason - nothing is being judged about the bug itself, ` +
				`only where it can be fixed. If the answer was a mis-click, correct it in ` +
				`the form above and reopen: the check re-runs on every edit.`,
		};
	}

	const missing = [];
	const ask = (what) => missing.push(what);
	const junk = (k) => !f[k] || JUNK.test(f[k]);

	if (/not tested yet/i.test(f[F.gate] || ''))
		ask('**Whose bug this is.** Run `uci set nss.general.enabled=\'0\'; uci commit nss; reboot` ' +
			'and retry. Still broken? Flash the official OpenWrt image for the device and retry once ' +
			'more. Then pick the answer that matches - that one line decides whether this is fixable here at all.');

	const state = f[F.state] || '';
	const cmds = [
		['`ubus call system board`', /"kernel"/],
		['`uci show nss`', /nss\.general/],
		['`nss-status -d`', /NSS offload status:/],
	].filter(([, m]) => !m.test(state)).map(([c]) => c);
	if (cmds.length)
		ask(`**Build and runtime state** is missing the output of ${cmds.join(', ')}. ` +
			'Paste it whole - the version, the offload state and the port counters are read from it.');

	if (/self-built/i.test(f[F.image] || '') && !f[F.diffconfig])
		ask('**Diffconfig.** A self-built image is only reproducible with the config that built it: ' +
			'paste `./scripts/diffconfig.sh` from the build tree.');

	for (const [k, label] of [[F.what, 'What happens'], [F.expected, 'What you expected instead']])
		if (junk(k)) ask(`**${label}** needs a real answer.`);

	if (junk(F.steps) || (f[F.steps] || '').length < 40)
		ask('**Steps to reproduce** must be runnable on hardware that is not yours: numbered, ' +
			'from a fresh boot, with the exact commands and how long it takes.');

	if (!f[F.regression]) ask('**Last image that worked** - a tag, a commit, "unknown" or "never worked".');

	if (junk(F.logs) || (f[F.logs] || '').length < 200)
		ask('**Logs** - `logread` and `dmesg` from the boot that shows the bug, not a picked line.');

	const crashed = /panic|crash|reboot|reset itself|watchdog|oops|hang/i.test(`${f[F.what]} ${f[F.logs]}`);
	if (crashed && !/ramoops|pstore/i.test(body))
		ask('**The crash log.** A box that panicked wrote one: attach the whole newest ' +
			'`/root/pstore/*/console-ramoops-*` as a file. Without it the cause is guesswork, and ' +
			'a truncated tail is usually cut exactly where the trap is.');

	if (!/config\s+(interface|wifi-iface|device|zone)/.test(f[F.netcfg] || ''))
		ask('**Network and wireless config** - the `uci export` output, secrets removed. ' +
			'Bridge, VLAN and SQM shape are what most of these bugs turn on.');

	const add = /(^|[^\d.])11\.4|mesh|802\.11s/i.test(body) ? ['as-is'] : [];

	if (!missing.length)
		return { verdict: 'ok', add, remove: ['needs-info', 'no-response'], close: false, comment: null };

	return {
		verdict: 'incomplete', add: [...add, 'needs-info'], remove: [], close: false,
		comment: 'Thanks for the report. It is missing what it takes to work it on hardware ' +
			'that is probably not yours:\n\n' + missing.map((m) => `- ${m}`).join('\n') +
			'\n\n**Add it by editing the issue itself**, not in a comment - this check re-runs on ' +
			'every edit and clears the label when the report is complete. An issue left incomplete ' +
			'closes itself after 7 days, with a reminder here two days before that; reopen it ' +
			'whenever you have the missing pieces.',
	};
}

module.exports = { parseForm, triage, F };
