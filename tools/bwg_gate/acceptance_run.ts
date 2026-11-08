#!/usr/bin/env bun
// Runs budgeted windows through the Gate's acceptance page in Chrome, one page lifetime each. Every Start here
// charges the device's ledgers for good, and a started campaign refuses plain leases until all three windows ran.
import { readFileSync } from "node:fs";
import { Page } from "./page_driver.ts";

const SUPERVISOR = "http://127.0.0.1:48765";
const COOL_DOWN_MS = 30_000;
const FAULT_STOP_MS = 30_000;
// A full window, its shutdown bound, and the page finishing the restore.
const STOP_WINDOW_MS = 260_000;
const CADENCE_CAPTURE_MS = 62_000;
const HTTP_SAMPLE_MS = 5_000;
// A session repeats its boot line this often.
const BOOT_LINE_WAIT_S = 20;
const RECEIPT_ROUND_MS = 10_000;

const wanted = process.argv.slice(2);
const started = performance.now();
const report = (event: string, detail: unknown = {}) => console.log(JSON.stringify({ t: Math.round(performance.now() - started), event, ...(detail as object) }));
const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));

const page = await Page.attach(SUPERVISOR);
await page.send("Page.enable");

const state = () => page.evaluate("window.workerAcceptance.state()");
async function step(operation: string, ...input: unknown[]): Promise<any> {
  const before = performance.now();
  const passed = input.map((value) => JSON.stringify(value)).join(",");
  const result = await page.evaluate(`(async () => { try { return { ok: await window.workerAcceptance.${operation}(${passed}) ?? null }; } catch (error) { return { failed: error.message }; } })()`);
  if (result.failed) throw new Error(`${operation}: ${result.failed}`);
  report(operation, { ms: Math.round(performance.now() - before) });
  return result.ok;
}

async function reloadPage() {
  await page.send("Page.reload", { ignoreCache: true });
  await page.send("Page.bringToFront");
  for (let attempt = 0; attempt < 40; attempt++) {
    await sleep(250);
    const status = await page.evaluate("window.workerAcceptance?.state().status").catch(() => undefined);
    if (status === "configured") return;
  }
  throw new Error("page_not_configured");
}

async function connect() {
  await page.click("#connect");
  for (let attempt = 1; attempt <= 30; attempt++) {
    await sleep(1_000);
    const current = await state();
    if (current.connected) { report("connected", { attempt }); return; }
    if (current.status === "failed") {
      report("connect_retry", { admission: current.admissionFailureStage, serial: current.serialFailureCategory, failure: current.failure });
      await sleep(9_000);
      await page.click("#connect");
    }
  }
  throw new Error("connect_timeout");
}

const supervisor = () => fetch(`${SUPERVISOR}/supervisor-state`).then((response) => response.json());
const local = (route: string) => fetch(`${SUPERVISOR}${route}`, { method: "POST", body: "{}" }).then((response) => response.json());

/** A fan proof, a reviewed budget, then the one signed window, inside one possession's 60 seconds. */
async function beginWindow() {
  await connect();
  await startOnOpenConnection();
}

async function startOnOpenConnection() {
  report("cooling", await step("submitCoolingReview"));
  report("budget_review", { review: await step("submitBudgetReview") });
  await step("prepareStartAuthorization");
  await step("loadSignedWindow");
  await step("startWindow");
}

async function awaitNonces(limitMs: number) {
  const end = performance.now() + limitMs;
  while (performance.now() < end) {
    const current = await state();
    if (current.qualification?.nonce_work_correlations > 0 && current.qualification.work_gate_remaining_ms > 3_500) return;
    if (!current.running) throw new Error(`window_ended_early_${current.failure ?? current.status}`);
    await sleep(500);
  }
  throw new Error("no_nonces_in_time");
}

async function awaitBaseline(limitMs: number) {
  const end = performance.now() + limitMs;
  while (performance.now() < end) {
    await sleep(1_000);
    const current = await state();
    if (current.status === "baseline_confirmed") return current;
    if (current.status === "failed" || current.failure) throw new Error(`window_failed_${current.failure}_${current.serialFailureCategory ?? "none"}`);
  }
  throw new Error("baseline_timeout");
}

/** The tab really goes to the background: the page's SDK ends the session, and only the device stops the work. */
/** Leaving the page ends the session; hiding it no longer does (turnstile ADR-0110). Returns as from the back/forward cache, so the page keeps its state. */
async function leavePage() {
  await page.evaluate(`dispatchEvent(new PageTransitionEvent("pagehide", { persisted: true }))`);
  await sleep(FAULT_STOP_MS);
  await page.evaluate(`dispatchEvent(new PageTransitionEvent("pageshow", { persisted: true }))`);
}

/** Reads the finished lease after a fault, over a fresh connection, for the records the judge needs. */
async function reviewAfterFault() {
  await step("close").catch(() => undefined);
  await sleep(2_000);
  await connect();
  await step("refresh");
}

const saveEvidence = (name: string, value: unknown) =>
  fetch(`${SUPERVISOR}/evidence-report`, { method: "POST", body: JSON.stringify({ name, value }) }).then((response) => response.json());

/** The board's address stays in its private file and out of everything this prints. */
function boardAddress(): string {
  const path = process.env.BWG_BOARD_IP_FILE;
  if (!path) throw new Error("board_ip_file_unset");
  return readFileSync(path, "utf8").trim();
}

function randomNonce(): string {
  return Buffer.from(crypto.getRandomValues(new Uint8Array(16))).toString("base64url");
}

/** The boot this page saw announced, for the restart request that has to name it. */
async function announcedBootOrdinal(): Promise<number> {
  for (let waited = 0; waited < BOOT_LINE_WAIT_S; waited++) {
    const text = await page.evaluate(`document.querySelector("#diagnostics")?.textContent ?? "[]"`);
    const line = JSON.parse(text).find((value: any) => value.category === "boot");
    if (line) return Number(line.boot_ordinal);
    await sleep(1_000);
  }
  throw new Error("boot_ordinal_not_announced");
}

async function runRestart() {
  await connect();
  const expectedBootOrdinal = await announcedBootOrdinal();
  const request = { requestNonce: randomNonce(), expectedBootOrdinal };
  try {
    const completed = await step("qualificationRestart", request);
    report("restart", { summary: completed.summary, ack: completed.ack });
  } finally {
    const evidence = await step("exportRestartEvidence");
    await saveEvidence("restart", evidence);
    report("restart_evidence", {
      summary: evidence?.summary,
      lifecycle: evidence?.lifecycle,
      observations: (evidence?.observations ?? []).map((value: any) => [value.atMs, value.diagnostic.category, value.diagnostic.stage ?? value.diagnostic.reset_reason ?? value.diagnostic.state ?? "", value.diagnostic.boot_ordinal ?? value.diagnostic.uptime_ms ?? ""]).slice(0, 24),
    });
  }
}

// The panic ends the page's session, and what it left is judged from the next boot and read off the board's flash.
async function runCoreDumpSelfTest() {
  await connect();
  const expectedBootOrdinal = await announcedBootOrdinal();
  const request = { requestNonce: randomNonce(), expectedBootOrdinal };
  try {
    const completed = await step("coreDumpSelfTest", request);
    report("core_dump_self_test", { summary: completed.summary, ack: completed.ack });
  } finally {
    const evidence = await step("exportCoreDumpSelfTestEvidence");
    await saveEvidence("core-dump-self-test", evidence);
    report("core_dump_self_test_evidence", {
      summary: evidence?.summary,
      lifecycle: evidence?.lifecycle,
      observations: (evidence?.observations ?? []).map((value: any) => [value.atMs, value.diagnostic.category, value.diagnostic.stage ?? value.diagnostic.reset_reason ?? value.diagnostic.state ?? "", value.diagnostic.boot_ordinal ?? value.diagnostic.uptime_ms ?? ""]).slice(0, 24),
    });
  }
  await reportReceipts();
}

/** The receipt lines take a session's first seconds to come round, and the Gate's parsers name what they accept. */
async function reportReceipts() {
  await sleep(RECEIPT_ROUND_MS);
  const text = await page.evaluate(`document.querySelector("#diagnostics")?.textContent ?? "[]"`);
  const receipts = JSON.parse(text).filter((value: any) => /receipt/.test(value.category));
  report("receipts", { receipts: [...new Map(receipts.map((value: any) => [JSON.stringify(value), value])).values()] });
}

async function runTrace() {
  await connect();
  const device = await step("deviceSerialTraceReview");
  const browser = await step("exportBrowserSerialTrace");
  await saveEvidence("serial-trace", { device, browser });
  const stages = (window: any) => [...new Set((window?.events ?? []).map((event: any) => event.stage))];
  report("trace", { available: device.snapshotAvailable, dropped: device.droppedEvents, current: device.current.events.length, currentStages: stages(device.current), previous: device.previous?.events.length ?? 0, previousStages: stages(device.previous), overwrittenPrevious: device.previous?.overwrittenEvents ?? 0 });
  await step("close");
}

async function runEndpoint() {
  await connect();
  const endpoint = await step("observeStationEndpoint");
  const status = await fetch(`http://${endpoint.ipv4}:${endpoint.httpPort}/api/system/info`);
  report("endpoint", { schema: endpoint.schema, matchesBoard: endpoint.ipv4 === boardAddress(), bootOrdinal: endpoint.bootOrdinal, generation: endpoint.generation, httpStatus: status.status });
  await step("close");
}

/** Reads the board's own HTTP status while a window runs, as a host watching it would. */
function sampleStatus(endpoint: { ipv4: string; httpPort: number }) {
  const counts = { samples: 0, failures: 0, slowest_ms: 0, hashing: 0 };
  const timer = setInterval(async () => {
    const before = performance.now();
    try {
      const info: any = await (await fetch(`http://${endpoint.ipv4}:${endpoint.httpPort}/api/system/info`, { signal: AbortSignal.timeout(HTTP_SAMPLE_MS) })).json();
      counts.samples++;
      if (info.hashRate > 0) counts.hashing++;
    } catch {
      counts.failures++;
    }
    counts.slowest_ms = Math.max(counts.slowest_ms, Math.round(performance.now() - before));
  }, HTTP_SAMPLE_MS);
  return { counts, stop: () => clearInterval(timer) };
}

async function runSoak() {
  await connect();
  const sampler = sampleStatus(await step("observeStationEndpoint"));
  try {
    await startOnOpenConnection();
    await awaitBaseline(900_000);
  } finally {
    sampler.stop();
    report("status_samples", sampler.counts);
  }
  report("completion", await step("submitSoakCompletion"));
}

/** What the recorder's own rule asks of a phase, apart from the core and priority of the Rust image's loop. */
function cadenceFindings(review: any) {
  return review.phases.map((phase: any) => {
    const inTime = phase.intervalBuckets[0] + phase.intervalBuckets[1];
    const refusals = [
      phase.state !== "complete" && "incomplete",
      phase.intervalCount < 60 && "few_intervals",
      phase.phase === "usb" && phase.maxProbeCount !== 12 && "probes",
      inTime * 100 < phase.intervalCount * 95 && "late_intervals",
      phase.maximumIntervalUs > 1_500_000 && "long_interval",
      phase.maximumExecutionUs > 500_000 && "long_execution",
      phase.subscriberMismatchCount !== 0 && "subscribers",
      phase.projectionFailures + phase.serializationFailures + phase.queueFailures + phase.sendFailures + phase.clockFailures !== 0 && "failures",
      phase.pendingSends !== 0 && "pending",
      phase.overflow && "overflow",
      review.droppedObservations !== 0 && "dropped",
    ].filter(Boolean);
    return {
      phase: phase.phase, state: phase.state, passed: phase.passed, refusals, intervals: phase.intervalCount, buckets: phase.intervalBuckets,
      maximumIntervalUs: phase.maximumIntervalUs, maximumExecutionUs: phase.maximumExecutionUs, probes: phase.maxProbeCount,
      cpuMismatches: phase.cpuMismatchCount, priorityMismatches: phase.priorityMismatchCount, subscriberMismatches: phase.subscriberMismatchCount,
      sends: phase.sendsCompleted, sendFailures: phase.sendFailures, projected: phase.projectionCount, unchanged: phase.unchangedCount,
    };
  });
}

/** One telemetry subscriber, which the recorder's rule asks for, across all three phases. */
function subscribeToTelemetry(): Promise<WebSocket> {
  return new Promise((done, fail) => {
    const socket = new WebSocket(`ws://${boardAddress()}/api/ws/live`);
    socket.onopen = () => done(socket);
    socket.onerror = () => fail(new Error("telemetry_subscribe_failed"));
  });
}

async function runCadence() {
  const subscriber = await subscribeToTelemetry();
  try {
    await connect();
    report("arm", await step("cadenceArm", "idle"));
    await sleep(CADENCE_CAPTURE_MS);
    report("arm", await step("cadenceArm", "usb"));
    const usbStarted = performance.now();
    const usb = await step("cadenceUsbPhase");
    report("usb_phase", { probes: usb.probes.length });
    await sleep(Math.max(0, CADENCE_CAPTURE_MS - (performance.now() - usbStarted)));
    report("arm", await step("cadenceArm", "mining"));
    await startOnOpenConnection();
    // The page itself stops renewing once it has seen the capture out.
    for (let waited = 0; waited < 120 && !(await state()).cadence?.suppressionRequested; waited++) await sleep(1_000);
    if (!(await state()).cadence?.suppressionRequested) throw new Error("cadence_suppression_missing");
    await sleep(FAULT_STOP_MS);
    await reviewAfterFault();
    await finishAttempt();
    await step("close");
    await reloadPage();
    await connect();
    const review = await step("cadenceReview");
    await saveEvidence("cadence", review);
    report("cadence", { schema: review.schema, available: review.snapshotAvailable, dropped: review.droppedObservations, storageBytes: review.storageBytes, phases: cadenceFindings(review) });
  } finally {
    subscriber.close();
  }
}

const finishAttempt = async () => report("completion", await step("submitAttemptCompletion"));
const finishCampaign = async () => { await step("close"); await sleep(500); };

const plans: Record<string, { query: string; run: () => Promise<unknown> }> = {
  campaign_normal: { query: "kind=campaign&window=0", async run() { await beginWindow(); await awaitBaseline(STOP_WINDOW_MS); await finishCampaign(); } },
  campaign_foreground: { query: "kind=campaign&window=1", async run() { await beginWindow(); await awaitNonces(20_000); await step("armForegroundLoss"); await leavePage(); await reviewAfterFault(); await finishCampaign(); } },
  campaign_heartbeat: { query: "kind=campaign&window=2", async run() { await beginWindow(); await awaitNonces(20_000); await step("suppressHeartbeats"); await sleep(FAULT_STOP_MS); await reviewAfterFault(); await finishCampaign(); } },
  attempt_diagnostic: { query: "kind=attempt&purpose=diagnostic", async run() { await beginWindow(); await awaitBaseline(STOP_WINDOW_MS); await finishAttempt(); } },
  attempt_normal: { query: "kind=attempt&purpose=normal", async run() { await beginWindow(); await awaitBaseline(STOP_WINDOW_MS); await finishAttempt(); } },
  attempt_foreground: { query: "kind=attempt&purpose=foreground_loss", async run() { await beginWindow(); await awaitNonces(20_000); await step("armForegroundLoss"); await leavePage(); await reviewAfterFault(); await finishAttempt(); } },
  attempt_heartbeat: { query: "kind=attempt&purpose=heartbeat_loss", async run() { await beginWindow(); await awaitNonces(20_000); await step("suppressHeartbeats"); await sleep(FAULT_STOP_MS); await reviewAfterFault(); await finishAttempt(); } },
  soak: { query: "kind=soak", run: runSoak },
  restart: { query: "kind=attempt&purpose=normal&flag=restartQualification", run: runRestart },
  core_dump_self_test: { query: "kind=attempt&purpose=normal&flag=coreDumpSelfTestQualification", run: runCoreDumpSelfTest },
  trace: { query: "", run: runTrace },
  endpoint: { query: "kind=attempt&purpose=normal&flag=stationEndpointHandoff", run: runEndpoint },
  cadence: { query: "kind=attempt&purpose=normal&flag=cadenceQualification", run: runCadence },
  // A signed Start with a bad signature changes nothing, and the review afterwards shows it.
  rejection: { query: "kind=campaign&window=0", async run() {
    await connect();
    report("budget_review", { review: await step("submitBudgetReview") });
    report("rejected", { receipt: await step("rejectStartForRecoveryTest") });
    await sleep(3_000);
    await reloadPage();
    await connect();
    report("budget_review", { review: await step("submitBudgetReview") });
    await step("close");
  } },
  // Reads a window that already ended, for a driver that lost it: the mode and its records stay.
  rejudge_normal: { query: "kind=campaign&window=0&label=campaign_normal&resume=1", async run() {} },
  review_window: { query: "", async run() { await connect(); await step("refresh"); await step("close"); } },
  receipts: { query: "", async run() { await connect(); await reportReceipts(); await step("close"); } },
  cooling: { query: "kind=attempt&purpose=normal", async run() { await connect(); report("cooling", await step("submitCoolingReview")); await step("close"); } },
};

let failures = 0;
for (const name of wanted) {
  const plan = plans[name];
  if (!plan) throw new Error(`unknown_plan_${name}`);
  report("plan", { name });
  if (plan.query) await fetch(`${SUPERVISOR}/control/mode?${plan.query}&label=${name}`);
  await reloadPage();
  try {
    await plan.run();
    if (name.startsWith("campaign_") || name === "review_window" || name === "rejudge_normal") report("judged", await local("/advance"));
  } catch (error) {
    failures++;
    report("plan_failed", { name, failure: (error as Error).message });
  }
  const final = await state();
  const { qualification: q } = final;
  report("final", {
    name, status: final.status, failure: final.failure, serial: final.serialFailureCategory, admission: final.admissionFailureStage,
    renewals: final.renewalsConfirmed, active_ms: q?.active_ms, limit: q?.active_limit_ms, reserved: q?.budget_reserved_ms, complete: q?.budget_complete, reason: q?.revocation_reason, stage: q?.safe_stop_stage,
    accepted: q?.accepted, rejected: q?.rejected, nonces: q?.nonce_work_correlations, work: q?.work_dispatched, attempt: q?.attempt, soak: q?.soak, chip: q?.chip_temp_celsius, stack: q?.owner_resources?.stack_free_bytes,
  });
  report("supervisor", await supervisor());
  await step("close").catch(() => undefined);
  await sleep(COOL_DOWN_MS);
}
report("finished", { failures });
process.exit(failures ? 1 : 0);
