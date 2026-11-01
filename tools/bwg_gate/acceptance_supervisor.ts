#!/usr/bin/env bun
// Serves the Gate's own acceptance page and signs its budgeted leases with a test authority, on loopback only.
// Charges on the device are permanent, so a window is signed and delivered once, and the campaign id lives on
// disk. Judging uses the Rust fork's window judge, so C's records meet the checks its own hardware runs met.
// It logs route names and never a credential, so its output is safe to keep with evidence.
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

type Options = { gate: string; authority: string; trust: string; pool: string; commit: string; elf: string; gateCommit: string; state: string; rustScripts: string; board: string; port: string; serialPort?: string; bridgePython?: string };
// `flag` names the page mode a qualification runs under, on top of the window it signs.
type Mode = ({ kind: "campaign"; window: number } | { kind: "attempt"; purpose: string } | { kind: "soak" }) & { flag?: string };
type Signed = { grant: unknown; renewals: unknown[]; summary: object };

const WINDOW_MS = [180_000, 30_000, 30_000];
const BUNDLE = "dist/worker-serial-acceptance/worker-serial-acceptance.js";
const HTML = "conformance/bwg-worker-serial-0.2/acceptance.html";
const PROTOCOL = "bwg-worker-controller/0.4";

function parseOptions(args: string[]): Options {
  const options: Record<string, string> = { port: "48765", board: "205" };
  for (let i = 0; i < args.length; i += 2) options[args[i]!.replace(/^--/, "")] = args[i + 1]!;
  for (const name of ["gate", "authority", "trust", "pool", "commit", "elf", "gateCommit", "state", "rustScripts"]) {
    if (!options[name]) throw new Error(`missing_option_${name}`);
  }
  return options as Options;
}

const options = parseOptions(process.argv.slice(2));
const gate = resolve(options.gate);
const state = resolve(options.state);
mkdirSync(`${state}/reports`, { recursive: true });
const { judgeWindow, validateState, SOAK_CONTEXT_SCHEMA } = await import(`${options.rustScripts}/fixed-usb-qualification/judge.mjs`);
const { unsafeSample } = await import(`${options.rustScripts}/fixed-usb-soak/judge.mjs`);
const SOAK_MAXIMUM_ACTIVE_MS = 600_000;

const randomId = () => Buffer.from(crypto.getRandomValues(new Uint8Array(16))).toString("base64url");

function campaignId(): string {
  const path = `${state}/campaign.json`;
  if (existsSync(path)) return JSON.parse(readFileSync(path, "utf8")).id;
  const id = randomId();
  writeFileSync(path, JSON.stringify({ id }), { flag: "wx" });
  return id;
}

let mode: Mode = { kind: "campaign", window: 0 };
let scope = { challengeId: "", retentionExpiryUnixSeconds: 0 };
let maybeReview: { report: any; binding: string } | undefined;
let maybeSigned: Signed | undefined;
let records: { sequence: number; receivedAtUnixMs: number; state: any }[] = [];
let maybeFault: any;
let sequence = 0;
let label = "none";
let maybeLastReport: unknown;
let maybeBridge: ReturnType<typeof Bun.spawn> | undefined;

const judgeContext = { gate_commit: options.gateCommit, firmware_commit: options.commit, app_elf_sha256: options.elf };

async function sign(operation: "start" | "renew", binding: string, request: unknown): Promise<string> {
  const signer = Bun.spawn(["bun", `${gate}/scripts/worker-development-authority.ts`, `sign-${operation}`, "--directory", options.authority, "--input", "-", "--output", "-"], { stdin: "pipe", stdout: "pipe", stderr: "pipe", cwd: gate });
  signer.stdin.write(JSON.stringify({ operation, activeChallengeId: scope.challengeId, controlSessionBindingSha256: binding, request }));
  await signer.stdin.end();
  const [output, errors, code] = await Promise.all([new Response(signer.stdout).text(), new Response(signer.stderr).text(), signer.exited]);
  // The authority's errors are closed categories, and the grant is never in them.
  if (code !== 0) throw new Error(`signer_failed_${code}_${errors.match(/category=([a-z_]+)/)?.[1] ?? "unknown"}`);
  return JSON.parse(output).authorization;
}

/** Which allowance to sign, how its lease renews, and its renewals, from what the device's review said. */
function plan(report: any) {
  if (mode.kind === "campaign") {
    const windowMs = WINDOW_MS[mode.window]!;
    return {
      allowance: { acceptanceCampaign: { id: campaignId(), window: mode.window, maximumActiveMilliseconds: windowMs } },
      renewAfter: 20_000, renewals: Math.ceil(windowMs / 20_000), renewalAfter: 20_000,
    };
  }
  if (mode.kind === "soak") {
    return {
      allowance: { soakAllowance: { schema: "worker-soak-allowance-v2", id: randomId(), ordinal: report.next_ordinal, maximumActiveMilliseconds: 600_000 } },
      renewAfter: 20_000, renewals: 36, renewalAfter: 20_000,
    };
  }
  const normal = mode.purpose === "normal";
  return {
    allowance: { qualificationAttempt: { schema: "worker-qualification-attempt-v1", id: randomId(), ordinal: report.next_ordinal, purpose: mode.purpose, maximumActiveMilliseconds: normal ? 180_000 : 30_000 } },
    renewAfter: 5_000, renewals: normal ? 9 : 2, renewalAfter: normal ? 20_000 : 5_000,
  };
}

function requireIdle(report: any) {
  if (report.pending !== false) throw new Error("review_pending");
  if (mode.kind === "campaign") {
    // A device with no campaign yet reports no match and empty masks, and only its first window may begin one.
    const fresh = report.campaign_match === false && report.reserved_mask === 0 && mode.window === 0;
    if (report.campaign_match !== true && !fresh) throw new Error("campaign_mismatch");
    if ((report.reserved_mask >> mode.window) & 1) throw new Error("window_already_reserved");
  } else if (!Number.isSafeInteger(report.next_ordinal) || report.next_ordinal < 1) {
    throw new Error("ordinal_invalid");
  }
}

async function signWindow(binding: string): Promise<Signed> {
  if (!maybeReview || maybeReview.binding !== binding) throw new Error("binding_not_reviewed");
  const { report } = maybeReview;
  maybeReview = undefined;
  requireIdle(report);
  const { allowance, renewAfter, renewals, renewalAfter } = plan(report);
  const pool = JSON.parse(readFileSync(options.pool, "utf8"));
  const leaseId = `lease_${randomId()}`;
  const start = {
    protocolVersion: PROTOCOL, leaseId, challengeId: scope.challengeId, durationMilliseconds: 60_000, renewAfterMilliseconds: renewAfter,
    stratum: { endpoint: `stratum+tcp://${pool.poolURL}:${pool.poolPort}/`, username: pool.poolUser, password: pool.poolPassword, suggestedDifficulty: 1000 },
    ...allowance,
  };
  const grant = { ...start, authorization: await sign("start", binding, start) };
  const signedRenewals = [];
  for (let i = 0; i < renewals; i++) {
    const renewal = { protocolVersion: PROTOCOL, leaseId, durationMilliseconds: 60_000, renewAfterMilliseconds: renewalAfter };
    signedRenewals.push({ ...renewal, authorization: await sign("renew", binding, renewal) });
  }
  return { grant, renewals: signedRenewals, summary: { mode, label, ...("next_ordinal" in report ? { ordinal: report.next_ordinal } : {}), renewals } };
}

function saveReport(name: string, report: unknown) {
  maybeLastReport = report;
  writeFileSync(`${state}/reports/${name}-${Date.now()}.json`, JSON.stringify(report, null, 1));
}

/** C leaves a Hello unanswered while it hands local mining over, as Rust does, and that first connect isn't the window's failure. */
const refusedConnect = (record: { state: any }) => !record.state.running && record.state.failure === "connect_failed" && record.state.admissionFailureStage === "hello";

/** The Rust fork's verdict on this window's records, or the closed category it refused them with. */
function judge(extra: object) {
  try {
    const refused = records.filter(refusedConnect).length;
    const windowRecords = records.filter((record) => !refusedConnect(record));
    for (const record of windowRecords) validateState(record.state, judgeContext);
    const index = mode.kind === "campaign" ? mode.window : mode.kind === "attempt" ? ({ foreground_loss: 1, heartbeat_loss: 2 } as Record<string, number>)[mode.purpose] ?? 0 : 0;
    const fault = maybeFault ? { ...maybeFault, window: index } : undefined;
    const iterative = mode.kind === "attempt" ? { iterativePurpose: mode.purpose } : {};
    const result = judgeWindow(index, windowRecords, fault, iterative);
    return { verdict: "passed", result, refused_connect_records: refused, ...extra };
  } catch (error) {
    return { verdict: "refused", category: (error as { code?: string }).code ?? (error as Error).message, ...extra };
  }
}

/** The Rust judge's terminal checks on a soak, without the observer and settings evidence its own harness carries. */
function judgeSoak(ledgerAfter: any) {
  const failures: string[] = [];
  const check = (failed: boolean, category: string) => { if (failed) failures.push(category); };
  try {
    const judged = records.filter((record) => !refusedConnect(record));
    for (const record of judged) validateState(record.state, { ...judgeContext, schema: SOAK_CONTEXT_SCHEMA });
    const samples = judged.map((record) => record.state.qualification).filter((q) => q?.soak);
    const q = samples.at(-1);
    if (!q) throw new Error("soak_never_observed");
    const last = records.at(-1)!.state;
    check(!last.deviceRestorationConfirmed || !last.deviceLeaseInactive, "restoration_unconfirmed");
    check(q.revocation_reason !== "lease_or_budget_expired", "soak_not_ended_by_budget");
    check(!q.safe_stop_complete || q.safe_stop_stage !== "fan_paused", "safe_stop_unconfirmed");
    check(!q.temperature_fresh || q.chip_temp_celsius > 45 || !q.fan_fresh || !(q.fan_rpm > 0), "terminal_cooling_missing");
    check(q.active_limit_ms !== SOAK_MAXIMUM_ACTIVE_MS || q.soak.reserved_ms !== SOAK_MAXIMUM_ACTIVE_MS || q.soak.maximum_active_ms !== SOAK_MAXIMUM_ACTIVE_MS, "soak_budget_mismatch");
    check(q.active_ms !== SOAK_MAXIMUM_ACTIVE_MS, "soak_active_time");
    check(q.accepted < 1, "no_accepted_share");
    check(q.work_dispatched < 1 || q.nonce_work_correlations < 1, "no_correlated_work");
    check(q.mine_on_boot !== false, "mine_on_boot_not_off");
    check(samples.some((sample) => sample.revocation_reason !== "none" && sample.revocation_reason !== "lease_or_budget_expired"), "soak_revoked");
    const unsafe = judged.filter((record) => record.state.running && record.state.qualification && unsafeSample(record.state.qualification, options.board));
    check(unsafe.length > 0, "unsafe_sample");
    check(judged.some((record) => record.state.failure !== undefined), "browser_failure");
    check(ledgerAfter?.schema !== "worker-soak-ledger-v2" || ledgerAfter.pending !== false, "soak_ledger_pending");
    return { verdict: failures.length === 0 ? "passed" : "refused", category: failures.join(",") || undefined,
      result: { active_ms: q.active_ms, accepted: q.accepted, rejected: q.rejected, nonce_work_correlations: q.nonce_work_correlations, renewals: last.renewalsConfirmed, samples: samples.length, unsafe_samples: unsafe.length } };
  } catch (error) {
    return { verdict: "refused", category: (error as { code?: string }).code ?? (error as Error).message };
  }
}

/** The page stops renewing a cadence window on purpose, once its capture ends, so only the device's stop is judged. */
function judgeCadenceWindow(ledgerAfter: any) {
  try {
    const last = records.at(-1)!.state;
    const q = last.qualification;
    if (!last.deviceRestorationConfirmed || !last.deviceLeaseInactive) throw new Error("cadence_restoration_missing");
    if (q.revocation_reason !== "heartbeat_timeout" || q.active_ms < 60_000) throw new Error("cadence_window_short");
    if (!q.safe_stop_complete || q.safe_stop_stage !== "fan_paused") throw new Error("cadence_stop_missing");
    if (!(q.nonce_work_correlations > 0)) throw new Error("cadence_mining_missing");
    if (ledgerAfter.pending !== false) throw new Error("cadence_ledger_pending");
    return { verdict: "passed", result: { active_ms: q.active_ms, accepted: q.accepted, rejected: q.rejected, nonce_work_correlations: q.nonce_work_correlations, gate_close_delay_ms: (q.gate_closed_ms - q.last_valid_heartbeat_ms) >>> 0 } };
  } catch (error) {
    return { verdict: "refused", category: (error as Error).message };
  }
}

function json(value: unknown) {
  return new Response(JSON.stringify(value), { headers: { "content-type": "application/json", "cache-control": "no-store" } });
}

const QUALIFICATION_FLAGS = ["cadenceQualification", "coreDumpSelfTestQualification", "restartQualification", "stationEndpointHandoff"];

const routes: Record<string, (input: any) => unknown | Promise<unknown>> = {
  "/cadence/probe": (input) => {
    appendFileSync(`${state}/records-${label}-probes.jsonl`, `${JSON.stringify(input)}\n`);
    return { cadence_probe_saved: true };
  },
  "/evidence-report": (input) => {
    saveReport(String(input.name), input.value);
    return { saved: true };
  },
  "/activate": () => {
    scope = { challengeId: `challenge_${randomId()}`, retentionExpiryUnixSeconds: Math.floor(Date.now() / 1000) + 86_400 };
    return scope;
  },
  // The attempt ledger in every mode: the page refuses a campaign review until the device holds that campaign.
  "/cooling-review-context": () => ({ nonce: randomId(), mode: "iterative" }),
  "/cooling-review": (input) => {
    saveReport("cooling", input);
    return { cooling_review_saved: true, review_file: "cooling.json" };
  },
  "/budget-review-context": () => mode.kind === "campaign" ? { campaignId: campaignId(), nonce: randomId() } : { nonce: randomId(), mode: mode.kind === "soak" ? "soak" : "iterative" },
  "/budget-review": (input) => {
    requireIdle(input.report);
    maybeReview = { report: input.report, binding: input.controlSessionBindingSha256 };
    return { budget_review_saved: true };
  },
  "/authorization-context": async (input) => {
    maybeSigned = await signWindow(input.controlSessionBindingSha256);
    return { ready: true };
  },
  "/completion-context": () => mode.kind === "soak" ? { nonce: randomId() } : { nonce: randomId(), campaignId: campaignId() },
  "/completion-review": (input) => {
    const ledger = input.ledger_after;
    const report = mode.kind === "soak" ? judgeSoak(ledger) : mode.flag === "cadenceQualification" ? judgeCadenceWindow(ledger) : judge({ ledger_after: ledger, original_budget: input.original_budget });
    saveReport(label, report);
    const common = { result: report.verdict === "passed" ? "passed" : "unverified", ordinal: ledger.last_completed_ordinal, cumulative_charged_ms: ledger.total_charged_ms, cleanup_confirmed: true };
    return mode.kind === "attempt" ? { ...common, purpose: mode.purpose } : common;
  },
  "/diagnostic-export": (input) => {
    saveReport("diagnostics", input);
    return { diagnostic_export_saved: true, review_file: `diagnostic-export-${randomId()}.json` };
  },
  "/record": (input) => {
    const record = { sequence: ++sequence, receivedAtUnixMs: Date.now(), state: input.state };
    records.push(record);
    appendFileSync(`${state}/records-${label}.jsonl`, `${JSON.stringify(record)}\n`);
    return { sequence };
  },
  "/fault": (input) => {
    maybeFault ??= { ...input, after_sequence: sequence };
    return { fault_recorded: true };
  },
  "/advance": () => {
    const report = judge({});
    saveReport(label, report);
    return report;
  },
};

Bun.serve({
  hostname: "127.0.0.1",
  port: Number(options.port),
  idleTimeout: 180,
  websocket: {
    idleTimeout: 0,
    open(socket) {
      const bridge = Bun.spawn([options.bridgePython!, resolve(import.meta.dir, "serial_bridge.py"), options.serialPort!], { stdin: "pipe", stdout: "pipe", stderr: "ignore" });
      maybeBridge = bridge;
      void (async () => {
        for await (const chunk of bridge.stdout as ReadableStream<Uint8Array>) socket.sendBinary(chunk);
        socket.close();
      })();
    },
    async message(_socket, data) {
      const sink = maybeBridge?.stdin as any;
      sink.write(data);
      await sink.flush();
    },
    close() {
      maybeBridge?.kill();
      maybeBridge = undefined;
    },
  },
  async fetch(request, server) {
    const { pathname, searchParams } = new URL(request.url);
    if (pathname !== "/record") console.log(request.method, pathname);
    if (pathname === "/serial" && options.serialPort && options.bridgePython) {
      return server.upgrade(request) ? undefined : new Response("upgrade refused", { status: 400 });
    }
    if (pathname === "/" || pathname === `/${HTML}`) {
      const page = readFileSync(`${gate}/${HTML}`, "utf8");
      return new Response(`${page}\n<script type="module" src="/supervisor-client.mjs"></script>`, { headers: { "content-type": "text/html" } });
    }
    if (pathname === `/${BUNDLE}`) return new Response(Bun.file(`${gate}/${BUNDLE}`), { headers: { "content-type": "text/javascript" } });
    if (pathname === "/supervisor-client.mjs") return new Response(Bun.file(resolve(import.meta.dir, "acceptance_client.mjs")), { headers: { "content-type": "text/javascript" } });
    if (pathname === "/context") {
      return json({
        expectedGateCommit: options.gateCommit, expectedFirmwareSourceCommit: options.commit, expectedAppElfSha256: options.elf,
        trust: JSON.parse(readFileSync(options.trust, "utf8")), ...(mode.kind === "soak" ? { soakQualification: true } : {}), ...(mode.flag ? { [mode.flag]: true } : {}),
      });
    }
    if (pathname === "/window-artifacts") {
      if (!maybeSigned) return new Response("no window", { status: 409 });
      const { grant, renewals, summary } = maybeSigned;
      maybeSigned = undefined;
      appendFileSync(`${state}/issued.jsonl`, `${JSON.stringify({ at: Date.now(), ...summary })}\n`);
      return json({ grant, renewals });
    }
    // Between page lifetimes, since a page loads one window at a time.
    if (pathname === "/control/mode") {
      const kind = searchParams.get("kind");
      mode = kind === "campaign" ? { kind, window: Number(searchParams.get("window")) } : kind === "soak" ? { kind } : { kind: "attempt", purpose: String(searchParams.get("purpose")) };
      const flag = searchParams.get("flag");
      if (flag) {
        if (!QUALIFICATION_FLAGS.includes(flag)) return new Response("unknown flag", { status: 400 });
        mode.flag = flag;
      }
      label = String(searchParams.get("label"));
      // `resume` carries on with a window's records on disk, after this supervisor restarted.
      const path = `${state}/records-${label}.jsonl`;
      records = searchParams.has("resume") && existsSync(path) ? readFileSync(path, "utf8").trim().split("\n").map((line) => JSON.parse(line)) : [];
      sequence = records.at(-1)?.sequence ?? sequence;
      maybeFault = undefined;
      maybeLastReport = undefined;
      maybeSigned = undefined;
      maybeReview = undefined;
      return json({ mode, label });
    }
    if (pathname === "/supervisor-state") return json({ mode, label, records: records.length, fault: maybeFault ?? null, last_report: maybeLastReport ?? null });
    const route = routes[pathname];
    if (!route) return new Response("not found", { status: 404 });
    try {
      return json(await route(await request.json()));
    } catch (error) {
      console.log("refused", pathname, (error as Error).message);
      return new Response((error as Error).message, { status: 400 });
    }
  },
});
console.log("acceptance supervisor ready");
