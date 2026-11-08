#!/usr/bin/env bun
// Drives a real device with the Gate's own controller code, from a terminal instead of a browser.
// It prints labels, states and timings only. Pool values and keys stay out of its output.
import { createHash } from "node:crypto";
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

type Options = { gate: string; authority: string; trust: string; pool: string; port: string; bridgePython: string; commit: string; elf: string; scenario: string; deviceIp?: string };

function parseOptions(args: string[]): Options {
  const options: Record<string, string> = {};
  for (let i = 0; i < args.length; i += 2) options[args[i]!.replace(/^--/, "")] = args[i + 1]!;
  for (const name of ["gate", "authority", "trust", "pool", "port", "bridgePython", "commit", "elf", "scenario"]) {
    if (!options[name]) throw new Error(`missing_option_${name}`);
  }
  return options as Options;
}

const options = parseOptions(process.argv.slice(2));
const gate = resolve(options.gate);
const { createWebSerialWorkerController, workerSerialQualificationHook } = await import(`${gate}/web/webserial-worker-controller.ts`);
const { workerSerialTestRuntime } = await import(`${gate}/web/webserial-worker-port.ts`);
const { createMemoryWorkerContinuityAccess } = await import(`${gate}/web/worker-continuity-store.ts`);

const started = performance.now();
const elapsed = () => Math.round(performance.now() - started);
const report = (event: string, detail: unknown = {}) => console.log(JSON.stringify({ t: elapsed(), event, ...(detail as object) }));
const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));
/** Only the scenarios that reach the board over HTTP need `--deviceIp`. */
const deviceUrl = (path: string) => { if (!options.deviceIp) throw new Error("missing_option_deviceIp"); return `http://${options.deviceIp}${path}`; };

function bridgedPort() {
  let maybeBridge: ReturnType<typeof Bun.spawn> | undefined;
  const port: any = {
    readable: null,
    writable: null,
    connected: true,
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    async open() {
      const bridge = Bun.spawn([options.bridgePython, resolve(import.meta.dir, "serial_bridge.py"), options.port], { stdin: "pipe", stdout: "pipe", stderr: "ignore" });
      maybeBridge = bridge;
      const output = (bridge.stdout as ReadableStream<Uint8Array>).getReader();
      // Cancelling a process's own stdout doesn't settle, which the Gate's 1 s close bound would fail.
      port.readable = new ReadableStream<Uint8Array>({
        async pull(controller) {
          const { done, value } = await output.read();
          if (done) controller.close();
          else controller.enqueue(value);
        },
        cancel() { bridge.kill(); },
      });
      const sink = bridge.stdin as any;
      port.writable = new WritableStream<Uint8Array>({
        // Not awaiting the flush: a pipe's flush can outlast the 1 s the Gate gives an abort, and a real
        // serial writer has no such wait.
        write(chunk) { sink.write(chunk); sink.flush(); },
      });
    },
    async close() {
      const closing = performance.now();
      maybeBridge?.kill();
      await maybeBridge?.exited;
      report("bridge_closed", { ms: Math.round(performance.now() - closing) });
      port.readable = null;
      port.writable = null;
    },
  };
  return port;
}

const hook: any = {
  suppressHeartbeats: false,
  memoryOnlyContinuity: true,
  allowClockDiscontinuityStimulus: false,
  observeStatus: () => {},
  // The digest stays on the wire. A short tag of it is enough to see that it moved, and says nothing of the settings.
  observePreservation: (value: any) => report("preservation", { schema: value.schema, unchanged: value.pool_configuration_unchanged_since_boot, mineOnBoot: value.mine_on_boot, settingsTag: createHash("sha256").update(value.settings_sha256).digest("hex").slice(0, 8) }),
  maybeObserveAdmissionFailure: (stage: string) => report("admission_failure", { stage }),
  maybeObserveSerialFailure: (category: string) => report("serial_failure", { category }),
  maybeObserveDiagnostic: (value: unknown) => report("diagnostic", { value }),
  async prepareScope() { return scope; },
};
const scope = { challengeId: "challenge_host_harness", retentionExpiryUnixSeconds: Math.floor(Date.now() / 1000) + 3600 };

function newController() {
  const trust = JSON.parse(readFileSync(options.trust, "utf8"));
  const port = bridgedPort();
  const runtime = {
    serial: { requestPort: async () => port, getPorts: async () => [port] },
    foreground: () => true, pageOpen: () => true,
    userActivation: () => true,
    now: () => performance.now(),
    acquireLock: async () => () => {},
    subscribePageExit: () => () => {},
    every: (ms: number, listener: () => void) => { const timer = setInterval(listener, ms); return () => clearInterval(timer); },
  };
  return createWebSerialWorkerController({
    deviceFilter: { usbVendorId: 0x303a, usbProductId: 0x1001 },
    trustedUpdateKeys: trust.updateAuthority.keys,
    continuityScope: scope,
    expectedFirmwareSourceCommit: options.commit,
    expectedAppElfSha256: options.elf,
    [workerSerialTestRuntime]: { runtime, continuity: createMemoryWorkerContinuityAccess(scope) },
    [workerSerialQualificationHook]: hook,
  } as any);
}

async function connect(deadlineMs = 180_000) {
  const until = performance.now() + deadlineMs;
  for (let attempt = 1; ; attempt++) {
    const controller = newController();
    try {
      const result = await controller.requestPermission();
      report("connected", { attempt, recovered: result.recovered });
      return controller;
    } catch (error) {
      report("connect_failed", { attempt, category: (error as Error).message });
      await controller.close().catch(() => {});
      if (performance.now() > until) throw error;
      await sleep(10_000);
    }
  }
}

async function sign(operation: "start" | "renew", request: unknown, activeChallengeId: string, binding: string): Promise<string> {
  const signer = Bun.spawn(["bun", `${gate}/scripts/worker-development-authority.ts`, `sign-${operation}`, "--directory", options.authority, "--input", "-", "--output", "-"], { stdin: "pipe", stdout: "pipe", stderr: "pipe", cwd: gate });
  signer.stdin.write(JSON.stringify({ operation, activeChallengeId, controlSessionBindingSha256: binding, request }));
  await signer.stdin.end();
  const [output, errors, code] = await Promise.all([new Response(signer.stdout).text(), new Response(signer.stderr).text(), signer.exited]);
  if (code !== 0) {
    // The authority's errors are closed categories, and the grant is never in them.
    const category = errors.match(/category=([a-z_]+)/)?.[1] ?? "unknown";
    throw new Error(`signer_failed_${code}_${category}`);
  }
  return JSON.parse(output).authorization;
}

function publicStatus(status: any) {
  const { lease, ...rest } = status;
  return { ...rest, ...(lease ? { renewInMs: lease.renewAtMonotonicMilliseconds - status.monotonicMilliseconds, expiresInMs: lease.expiresAtMonotonicMilliseconds - status.monotonicMilliseconds } : {}) };
}

function leaseRequest(id: string, durationMilliseconds: number, renewAfterMilliseconds: number) {
  const pool = JSON.parse(readFileSync(options.pool, "utf8"));
  return {
    protocolVersion: "bwg-worker-controller/0.4",
    leaseId: `lease_${id}`,
    challengeId: scope.challengeId,
    durationMilliseconds,
    renewAfterMilliseconds,
    stratum: { endpoint: `stratum+tcp://${pool.poolURL}:${pool.poolPort}/`, username: pool.poolUser, password: pool.poolPassword, suggestedDifficulty: 1000 },
  };
}

async function startLease(controller: any, id: string, duration: number, renewAfter: number) {
  const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
  const request = leaseRequest(id, duration, renewAfter);
  const authorization = await sign("start", request, request.challengeId, binding);
  const before = performance.now();
  const status = await controller.startLease({ ...request, authorization });
  report("lease_started", { replyMs: Math.round(performance.now() - before), status: publicStatus(status) });
  return { request, binding };
}

const reportTimedStop = async (controller: any, stop: () => Promise<unknown>) => { const before = performance.now(); const status = await stop(); report("stopped", { stopMs: Math.round(performance.now() - before), status: publicStatus(status) }); await controller.close(); };
async function renew(controller: any, lease: { request: any; binding: string }) {
  const { leaseId, protocolVersion, durationMilliseconds, renewAfterMilliseconds } = lease.request;
  const request = { protocolVersion, leaseId, durationMilliseconds, renewAfterMilliseconds };
  const authorization = await sign("renew", request, lease.request.challengeId, lease.binding);
  const status = await controller.renewLease({ ...request, authorization });
  report("lease_renewed", { status: publicStatus(status) });
}

async function watch(controller: any, ms: number, until: (status: any) => boolean = () => false) {
  const end = performance.now() + ms;
  let status: any;
  while (performance.now() < end) {
    await sleep(1000);
    status = await controller.status();
    if (until(status)) break;
  }
  report("status", { status: publicStatus(status) });
  return status;
}

const board = process.env.BWG_BOARD ?? "205";
// A status sent during a stop has no reply until the stop ends, and the Gate gives up after 30 s. The 601's cooling
// proof takes 30 s however cool the board is, which carries its stop past that, so these wait before asking.
const waitOutStop = (windowMs = 0) => board === "601" ? sleep(windowMs + 50_000) : Promise.resolve();
const v2Root = () => `${process.env.HOME}/.bitaxe-fork-private/${board}/v2-fixture`;
const V2_USER = "fixture-worker";
const randomAttemptId = () => Buffer.from(crypto.getRandomValues(new Uint8Array(16))).toString("base64url");

/** Starts the fixture for one attempt. Its readiness holds private addresses, which go nowhere but the grant. */
async function startFixture(fixtureScope: "channel" | "share", attemptId: string) {
  mkdirSync(v2Root(), { recursive: true, mode: 0o700 });
  const root = `${v2Root()}/${Date.now()}-${fixtureScope}`;
  const peer = readFileSync(`${process.env.HOME}/.bitaxe-fork-private/${board}/ip.txt`, "utf8").trim();
  const child = Bun.spawn([process.execPath, resolve(import.meta.dir, "v2_fixture.ts"), process.env.BWG_V2_FIXTURE!, root, fixtureScope, attemptId, peer, V2_USER],
    { stdin: "ignore", stdout: "pipe", stderr: Bun.file(`${root}.stderr`) });
  process.on("exit", () => child.kill());
  const reader = (child.stdout as ReadableStream<Uint8Array>).getReader();
  let text = "";
  while (!text.includes("\n")) {
    const { done, value } = await reader.read();
    if (done) throw new Error("fixture_not_ready");
    text += new TextDecoder().decode(value);
  }
  const ready = JSON.parse(text.split("\n")[0]!);
  const stratum = {
    endpoint: `stratum+tcp://${ready.listenIpv4}:${ready.listenPort}/`,
    profile: "bwg-worker-stratum-v2-standard/0.1",
    authorityPublicKey: ready.authorityPublicKey,
    userIdentity: V2_USER,
  };
  return { root, child, stratum };
}

/** One attempt's records as the device reported them, private for their addresses, and a summary free of them. */
function keepRecord(root: string, status: any) {
  appendFileSync(`${root}.records.jsonl`, `${JSON.stringify(status)}\n`, { mode: 0o600 });
  const record = status.record;
  return {
    state: status.state,
    outcome: record?.outcome ?? null,
    failure: record?.firstFailure ?? null,
    secondary: record?.secondaryFailures?.length ?? 0,
    stages: record?.events?.map((event: any) => event.kind).join(",") ?? "",
    shares: record?.shareFacts?.length ?? 0,
    acked: record?.shareFacts?.filter((fact: any) => fact.ackAtDeviceUs !== null).length ?? 0,
    fence: record?.resources?.fenceRetained ?? null,
  };
}

/** Keeps the process, and so the board's log capture, alive for a while when a step fails. */
async function holdingLogs<T>(step: () => Promise<T>): Promise<T> {
  try {
    return await step();
  } catch (error) {
    report("step_failed", { category: (error as Error).message });
    await sleep(15_000);
    throw error;
  }
}

async function v2Channel() {
  const attemptId = randomAttemptId();
  hook.stratumV2Pair = { firmwareSourceCommit: options.commit, appElfSha256: options.elf, scope: "channel" };
  const fixture = await startFixture("channel", attemptId);
  const controller = await connect();
  const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
  const idle = await controller.stratumV2Status("channel", null, binding);
  report("idle", keepRecord(fixture.root, idle));
  const admitted = await holdingLogs(() => controller.stratumV2ChannelStart({
    schema: "worker-stratum-v2-channel-start-v1",
    attemptId,
    expectedBootOrdinal: idle.observation.bootOrdinal,
    networkObservedAtUs: idle.observation.observedAtUs,
    stratum: fixture.stratum,
  }, binding));
  report("admitted", keepRecord(fixture.root, admitted));
  let last = "";
  for (let i = 0; i < 130; i++) {
    const status = await controller.stratumV2Status("channel", attemptId, binding);
    const summary = keepRecord(fixture.root, status);
    if (JSON.stringify(summary) !== last) report("attempt", summary);
    last = JSON.stringify(summary);
    if (status.state === "terminal") break;
    await sleep(500);
  }
  report("fixture_exit", { code: await fixture.child.exited });
  await controller.close();
  report("root", { root: fixture.root });
}

async function v2Share() {
  const attemptId = randomAttemptId();
  hook.stratumV2Pair = { firmwareSourceCommit: options.commit, appElfSha256: options.elf, scope: "share" };
  const fixture = await startFixture("share", attemptId);
  const controller = await connect();
  const review = await controller.qualificationAttemptReview();
  report("review", { nextOrdinal: review.next_ordinal, pending: review.pending });
  const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
  report("idle", keepRecord(fixture.root, await controller.stratumV2Status("share", null, binding)));
  const request = {
    protocolVersion: "bwg-worker-controller/0.4",
    leaseId: `lease_${attemptId.replaceAll("-", "a").replaceAll("_", "b")}`,
    challengeId: scope.challengeId,
    durationMilliseconds: 60_000,
    renewAfterMilliseconds: 20_000,
    stratum: fixture.stratum,
    qualificationAttempt: { schema: "worker-qualification-attempt-v1", id: attemptId, ordinal: review.next_ordinal, purpose: "normal", maximumActiveMilliseconds: 180_000 },
  };
  writeFileSync(`${fixture.root}.qualification.json`, JSON.stringify(request.qualificationAttempt), { mode: 0o600 });
  await controller.startLease({ ...request, authorization: await sign("start", request, request.challengeId, binding) });
  report("lease_started");
  const renewal = { protocolVersion: request.protocolVersion, leaseId: request.leaseId, durationMilliseconds: 60_000, renewAfterMilliseconds: 20_000 };
  let nextRenewal = performance.now() + 20_000;
  let last = "";
  for (let i = 0; i < 400; i++) {
    const status = await controller.stratumV2Status("share", attemptId, binding).catch((error: Error) => {
      // Missing heartbeats end the session, which is the fault itself. The record outlives it.
      if (hook.suppressHeartbeats) return null;
      throw error;
    });
    if (status === null) break;
    const summary = keepRecord(fixture.root, status);
    if (JSON.stringify(summary) !== last) report("attempt", summary);
    last = JSON.stringify(summary);
    if (status.state === "terminal") break;
    if (hook.suppressHeartbeats || summary.acked > 0) {
      const safety = await controller.status().catch((error: Error) => {
        if (hook.suppressHeartbeats) return null;
        throw error;
      });
      if (safety === null) break;
      keepSafety(fixture.root, safety, status.observation.observedAtUs);
    }
    if (!hook.suppressHeartbeats && summary.acked > 0) {
      report("heartbeats_suppressed");
      hook.suppressHeartbeats = true;
    }
    if (!hook.suppressHeartbeats && performance.now() >= nextRenewal) {
      await controller.renewLease({ ...renewal, authorization: await sign("renew", renewal, request.challengeId, binding) });
      nextRenewal = performance.now() + 20_000;
      report("renewed");
    }
    await sleep(500);
  }
  hook.suppressHeartbeats = false;
  await followShare(fixture.root, attemptId, () => report("session_ended"));
  report("fixture_exit", { code: await fixture.child.exited });
  report("root", { root: fixture.root });
}

/** Reads a share attempt to its end over a session of its own, as the stop can outlast the one that started it. */
async function followShare(root: string, attemptId: string, onStart = () => {}) {
  onStart();
  await sleep(1_000);
  let last = "";
  for (let session = 0; session < 40; session++) {
    try {
      const controller = await connect();
      for (;;) {
        const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
        const status = await controller.stratumV2Status("share", attemptId, binding);
        const summary = keepRecord(root, status);
        if (JSON.stringify(summary) !== last) report("attempt", summary);
        last = JSON.stringify(summary);
        keepSafety(root, await controller.status(), status.observation.observedAtUs);
        if (status.state === "terminal") {
          await controller.close();
          return;
        }
        await sleep(1_000);
      }
    } catch (error) {
      report("follow_retry", { category: (error as Error).message });
      await sleep(3_000);
    }
  }
  throw new Error("share_never_finished");
}

function keepSafety(root: string, status: unknown, maybeObservedAtUs: number | null) {
  appendFileSync(`${root}.safety.jsonl`, `${JSON.stringify({ atHostMs: Date.now(), observedAtUs: maybeObservedAtUs, status })}\n`, { mode: 0o600 });
}

/** One attempt's statuses as the device reported them, private for their addresses, and a summary free of them. */
function keepNoiseStatus(root: string, status: any) {
  appendFileSync(`${root}.records.jsonl`, `${JSON.stringify(status)}\n`, { mode: 0o600 });
  const job = status.job;
  return {
    state: status.state,
    stages: job?.stages.map((row: any) => row.stage).join(",") ?? null,
    firstFailure: job?.firstFailure ? `${job.firstFailure.stage}/${job.firstFailure.category}/${job.firstFailure.detail}` : null,
    outcome: job?.terminal?.outcome ?? null,
  };
}

async function waitForFile(path: string) {
  for (let i = 0; i < 50; i++) {
    if (existsSync(path)) return JSON.parse(readFileSync(path, "utf8"));
    await sleep(100);
  }
  throw new Error("fixture_not_ready");
}

/** One diagnostic against the fixture. A different key is a wrong authority; `cancel` ends it as soon as it is admitted. */
async function noiseAttempt(variant: "plain" | "wrong_authority" | "cancel") {
  const attemptId = randomAttemptId();
  hook.noiseDiagnosticPair = { firmwareSourceCommit: options.commit, appElfSha256: options.elf };
  mkdirSync(v2Root(), { recursive: true, mode: 0o700 });
  const root = `${v2Root()}/${Date.now()}-noise-${variant}`;
  const peer = readFileSync(`${process.env.HOME}/.bitaxe-fork-private/${board}/ip.txt`, "utf8").trim();
  const child = Bun.spawn([process.execPath, resolve(import.meta.dir, "v2_fixture.ts"), process.env.BWG_V2_FIXTURE!, root, "noise", attemptId, peer],
    { stdin: "ignore", stdout: "ignore", stderr: Bun.file(`${root}.stderr`) });
  process.on("exit", () => child.kill());
  const ready = await waitForFile(`${root}/ready.json`);
  const controller = await connect();
  const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
  const idle = await controller.noiseDiagnosticStatus(null, binding);
  report("idle", { state: idle.state });
  const start = {
    schema: "worker-noise-diagnostic-start-v2",
    attemptId,
    expectedBootOrdinal: idle.observation.bootOrdinal,
    networkObservedAtUs: idle.observation.observedAtUs,
    fixtureIpv4: ready.listenIpv4,
    fixturePort: ready.listenPort,
    authorityPublicKey: variant === "wrong_authority" ? "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA" : ready.authorityPublicKey,
  };
  writeFileSync(`${root}.start.json`, JSON.stringify(start), { mode: 0o600 });
  const admitted = await holdingLogs(() => controller.noiseDiagnosticStart(start, binding));
  report("admitted", keepNoiseStatus(root, admitted));
  if (variant === "cancel") report("cancelled", keepNoiseStatus(root, await controller.noiseDiagnosticCancel(attemptId, binding)));
  let last = "";
  for (let i = 0; i < 260; i++) {
    const status = await controller.noiseDiagnosticStatus(attemptId, binding);
    const summary = keepNoiseStatus(root, status);
    if (JSON.stringify(summary) !== last) report("attempt", summary);
    last = JSON.stringify(summary);
    if (status.state === "terminal") break;
    await sleep(500);
  }
  await controller.close();
  // A refused attempt leaves the fixture listening until its own deadline, so only the accepted one waits for it.
  if (variant === "plain") report("fixture_exit", { code: await child.exited });
  report("root", { root });
}

const scenarios: Record<string, () => Promise<void>> = {
  noise: () => noiseAttempt("plain"),
  noise_wrong_authority: () => noiseAttempt("wrong_authority"),
  noise_cancel: () => noiseAttempt("cancel"),
  v2_channel: v2Channel,
  v2_share: v2Share,
  async v2_share_read() {
    const root = process.env.BWG_V2_ROOT!;
    const attemptId = JSON.parse(readFileSync(`${root}.qualification.json`, "utf8")).id;
    hook.stratumV2Pair = { firmwareSourceCommit: options.commit, appElfSha256: options.elf, scope: "share" };
    await followShare(root, attemptId);
    report("root", { root });
  },
  async connect() {
    const controller = await connect();
    report("baseline", { status: publicStatus(await controller.status()) });
    await controller.close();
  },
  // Hello, a lease, one renewal and an operator stop, on one connection.
  async lease_stop() {
    const controller = await connect();
    report("baseline", { status: publicStatus(await controller.status()) });
    const lease = await startLease(controller, `stop${Date.now()}`, 60_000, 20_000);
    await watch(controller, 20_000);
    await renew(controller, lease);
    await watch(controller, 25_000);
    await reportTimedStop(controller, () => controller.pause());
  },
  // The restoration the Gate's page asks for once a Gate Pass is issued, during mining.
  async restore_satisfied() {
    const controller = await connect();
    await startLease(controller, `restore${Date.now()}`, 60_000, 20_000).then(() => watch(controller, 20_000));
    await reportTimedStop(controller, () => controller.restore("challenge_satisfied"));
  },
  // A long lease kept alive by renewals, to show accepted shares.
  async mining() {
    const controller = await connect();
    const lease = await startLease(controller, `mining${Date.now()}`, 60_000, 20_000);
    for (let renewal = 0; renewal < 15; renewal++) {
      await watch(controller, 20_000);
      await renew(controller, lease);
    }
    await watch(controller, 5_000);
    await reportTimedStop(controller, () => controller.pause());
  },
  // The device restarts under a running lease; the next session must report it.
  async reboot() {
    const controller = await connect();
    await startLease(controller, `reboot${Date.now()}`, 60_000, 20_000);
    await watch(controller, 5_000);
    report("restart_requested", { http: (await fetch(deviceUrl("/api/system/restart"), { method: "POST" })).status });
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
    await sleep(25_000);
    const next = await connect();
    report("after_reboot", { status: publicStatus(await next.status()) });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
  // An operator's pause during a lease: the lease ends at once and a resume can't reopen it.
  async operator_pause() {
    const controller = await connect();
    await startLease(controller, `pause${Date.now()}`, 60_000, 20_000);
    await watch(controller, 5_000);
    report("pause_requested", { http: (await fetch(deviceUrl("/api/system/pause"), { method: "POST" })).status });
    await sleep(3_000);
    await controller.status().then((status: any) => report("status_after_pause", { status: publicStatus(status) })).catch((error: Error) => report("status_failed", { category: error.message }));
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
    await sleep(20_000);
    const next = await connect();
    report("after_pause", { status: publicStatus(await next.status()) });
    // A lease outranks the pause, and the pause returns once the link goes quiet.
    const started = await startLease(next, `paused${Date.now()}`, 60_000, 20_000).then(() => "started", (error: Error) => error.message);
    report("start_while_paused", { result: started });
    if (started === "started") await watch(next, 5_000);
    const stopped = await next.pause();
    report("lease_while_paused_stopped", { status: publicStatus(stopped) });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
    await sleep(20_000);
    const info: any = await (await fetch(deviceUrl("/api/system/info"))).json();
    report("pause_returned", { miningPaused: info.miningPaused });
    report("resume_requested", { http: (await fetch(deviceUrl("/api/system/resume"), { method: "POST" })).status });
  },
  // The same signed Start twice: the second must fail on the device's high water, and it revokes the session.
  async replay() {
    const controller = await connect();
    const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
    const request = leaseRequest(`replay${Date.now()}`, 30_000, 10_000);
    const grant = { ...request, authorization: await sign("start", request, request.challengeId, binding) };
    await controller.startLease(grant);
    await sleep(2_000);
    const stopped = await controller.pause();
    report("first_start_stopped", { status: publicStatus(stopped) });
    await controller.prepareWorkerLeaseAuthorizationContext("start");
    const replayed = await controller.startLease(grant).then(() => "started", (error: Error) => error.message);
    report("replayed_start", { result: replayed });
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
    const next = await connect();
    report("after_replay", { status: publicStatus(await next.status()) });
    report("rejection_review", { review: await next.authorizationRejectionReview() });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
  // The fault window with no renewal: the lease has to end by itself.
  async expiry() {
    const controller = await connect();
    await startLease(controller, `expiry${Date.now()}`, 30_000, 10_000);
    const before = performance.now();
    await waitOutStop(30_000);
    const status = await watch(controller, 170_000, (value) => value.state === "baseline");
    report("expired", { afterStartMs: Math.round(performance.now() - before), status: publicStatus(status) });
    await controller.close();
  },
  // Heartbeats stop while the chip is still ramping; the stop must begin within its limit, not after the start.
  async start_abort() {
    const controller = await connect();
    const { controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start");
    const request = leaseRequest(`abort${Date.now()}`, 60_000, 20_000);
    const authorization = await sign("start", request, request.challengeId, binding);
    const starting = controller.startLease({ ...request, authorization }).then(() => "started", (error: Error) => error.message);
    await sleep(1_000);
    report("heartbeats_suppressed");
    hook.suppressHeartbeats = true;
    report("start_result", { result: await starting });
    await sleep(5_000);
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
    hook.suppressHeartbeats = false;
    const next = await connect();
    report("after_fault", { status: publicStatus(await next.status()) });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
  // Something outside forces an unsafe reading during a lease. The device stops by itself, and says why in a later session.
  async unsafe_reading() {
    const controller = await connect();
    await startLease(controller, `unsafe${Date.now()}`, 60_000, 20_000);
    for (let i = 0; i < 70; i++) {
      const status = await controller.status().then(publicStatus, () => null);
      if (status?.state !== "mining") break;
      await sleep(1_000);
    }
    await controller.close().catch(() => {});
    await sleep(25_000);
    const next = await connect();
    await sleep(6_000);
    report("after_fault", { status: publicStatus(await next.status()) });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
  // A setting outside the board's catalog during a lease: the device must stop by itself.
  async limit_fault() {
    const controller = await connect();
    await startLease(controller, `limit${Date.now()}`, 60_000, 20_000);
    await watch(controller, 5_000);
    const settings = await (await fetch(deviceUrl("/api/system/info"))).json();
    const patch = (frequency: number) => fetch(deviceUrl("/api/system"), { method: "PATCH", headers: { "content-type": "application/json" }, body: JSON.stringify({ frequency }) });
    report("limit_broken", { http: (await patch(300)).status });
    await sleep(2_000);
    const stopped = await controller.status().then(publicStatus, (error: Error) => ({ failed: error.message }));
    report("after_limit", { status: stopped });
    report("limit_restored", { http: (await patch(settings.frequency)).status });
    await sleep(25_000);
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
    const next = await connect();
    report("after_fault", { status: publicStatus(await next.status()) });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
  // The boot, clock and rejection reviews, around a lease the clock stimulus ends.
  async reviews() {
    hook.allowClockDiscontinuityStimulus = true;
    const controller = await connect();
    report("boot_review", { review: await controller.bootReview() });
    await startLease(controller, `stimulus${Date.now()}`, 60_000, 20_000);
    report("stimulus_armed", { ack: await controller.clockDiscontinuityStimulus() });
    await waitOutStop();
    const status = await watch(controller, 60_000, (value) => value.state === "baseline");
    report("after_stimulus", { status: publicStatus(status) });
    report("stimulus_review", { review: await controller.clockDiscontinuityStimulusReview() });
    report("rejection_review", { review: await controller.authorizationRejectionReview() });
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
  // Heartbeats stop on a live connection; the device must revoke by itself.
  async heartbeat_loss() {
    const controller = await connect();
    await startLease(controller, `heartbeat${Date.now()}`, 60_000, 20_000);
    await watch(controller, 10_000);
    report("heartbeats_suppressed");
    hook.suppressHeartbeats = true;
    await sleep(5_000);
    await controller.close().catch((error: Error) => report("close_failed", { category: error.message }));
    hook.suppressHeartbeats = false;
    const next = await connect();
    report("after_fault", { status: publicStatus(await next.status()) });
    await next.close().catch((error: Error) => report("close_failed", { category: error.message }));
  },
};

const run = scenarios[options.scenario];
if (!run) throw new Error("unknown_scenario");
await run();
report("done");
process.exit(0);
