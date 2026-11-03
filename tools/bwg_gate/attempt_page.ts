#!/usr/bin/env bun
// Runs one Noise diagnostic, V2 channel or V2 share attempt through Chrome's real WebSerial, against the Rust repository's fixture.
// noise_judge.mjs and v2_judge.mjs read what it keeps. It uses the origin the test profile's serial grant belongs to,
// so the acceptance supervisor has to be stopped first.
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { Page } from "./page_driver.ts";

const ORIGIN_PORT = 48765;
const POLL_MS = 500;
const POLLS = 260;
const USER = "fixture-worker";

const [kind, gate, trustPath, commit, elf, fixtureBinary, privateRoot] = process.argv.slice(2);
if (kind !== "noise" && kind !== "channel" && kind !== "share") throw new Error("kind_must_be_noise_channel_or_share");
const started = performance.now();
const report = (event: string, detail: unknown = {}) => console.log(JSON.stringify({ t: Math.round(performance.now() - started), event, ...(detail as object) }));
const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));

const entry = `${privateRoot}/attempt-entry.ts`;
writeFileSync(entry, `import * as gate from "${gate}/web/webserial-worker-controller.ts"; (window as any).gate = gate;\n`, { mode: 0o600 });
const built = await Bun.build({ entrypoints: [entry], target: "browser" });
if (!built.success) throw new Error("gate_bundle_failed");
const bundle = await built.outputs[0]!.text();
const PAGE = `<!doctype html><meta charset="utf-8"><title>Attempt</title><button id="connect">Connect</button>
<script type="module" src="/gate.js"></script><script type="module" src="/attempt_browser.mjs"></script>`;
const js = (body: BodyInit) => new Response(body, { headers: { "content-type": "text/javascript" } });
const server = Bun.serve({
  port: ORIGIN_PORT,
  hostname: "127.0.0.1",
  fetch(request) {
    const { pathname } = new URL(request.url);
    if (pathname === "/gate.js") return js(bundle);
    if (pathname === "/attempt_browser.mjs") return js(Bun.file(resolve(import.meta.dir, "attempt_browser.mjs")));
    if (pathname === "/context") return Response.json({ commit, elf, trust: JSON.parse(readFileSync(trustPath!, "utf8")) });
    return new Response(PAGE, { headers: { "content-type": "text/html" } });
  },
});

const page = await Page.attach(`http://127.0.0.1:${ORIGIN_PORT}`);
await page.send("Page.enable");
await page.send("Page.reload", { ignoreCache: true });
await page.send("Page.bringToFront");
await sleep(1_000);

async function call(operation: string, ...input: unknown[]): Promise<any> {
  const passed = input.map((value) => JSON.stringify(value)).join(",");
  const result = await page.evaluate(`(async () => { try { return { ok: await window.attempt.${operation}(${passed}) ?? null }; } catch (error) { return { failed: String(error.message) }; } })()`);
  if (result.failed) throw new Error(`${operation}: ${result.failed}`);
  return result.ok;
}

async function connect() {
  for (let attempt = 1; attempt <= 18; attempt++) {
    await page.click("#connect");
    for (let wait = 0; wait < 10; wait++) {
      await sleep(1_000);
      const current = await call("state");
      if (current.connected) return report("connected", { attempt });
      if (current.failure) {
        report("connect_retry", { failure: current.failure });
        break;
      }
    }
  }
  throw new Error("connect_timeout");
}

mkdirSync(`${privateRoot}/v2-fixture`, { recursive: true, mode: 0o700 });
const root = `${privateRoot}/v2-fixture/${Date.now()}-page-${kind}`;
const peer = readFileSync(`${privateRoot}/ip.txt`, "utf8").trim();
const attemptId = Buffer.from(crypto.getRandomValues(new Uint8Array(16))).toString("base64url");
const fixture = Bun.spawn([process.execPath, resolve(import.meta.dir, "v2_fixture.ts"), fixtureBinary!, root, kind, attemptId, peer, USER], {
  stdin: "ignore",
  stdout: "pipe",
  stderr: Bun.file(`${root}.stderr`),
});
process.on("exit", () => fixture.kill());

async function readyOfChannel() {
  const reader = (fixture.stdout as ReadableStream<Uint8Array>).getReader();
  let text = "";
  while (!text.includes("\n")) {
    const { done, value } = await reader.read();
    if (done) throw new Error("fixture_not_ready");
    text += new TextDecoder().decode(value);
  }
  return JSON.parse(text.split("\n")[0]!);
}

async function readyOfNoise() {
  for (let i = 0; i < 50; i++) {
    if (existsSync(`${root}/ready.json`)) return JSON.parse(readFileSync(`${root}/ready.json`, "utf8"));
    await sleep(100);
  }
  throw new Error("fixture_not_ready");
}

const ready = kind === "noise" ? await readyOfNoise() : await readyOfChannel();
const keep = (status: unknown) => appendFileSync(`${root}.records.jsonl`, `${JSON.stringify(status)}\n`, { mode: 0o600 });
const keepSafety = (status: unknown, maybeObservedAtUs: number | null) =>
  appendFileSync(`${root}.safety.jsonl`, `${JSON.stringify({ atHostMs: Date.now(), observedAtUs: maybeObservedAtUs, status })}\n`, { mode: 0o600 });

async function sign(operation: "start" | "renew", request: unknown, activeChallengeId: string, binding: string): Promise<string> {
  const signer = Bun.spawn(["bun", `${gate}/scripts/worker-development-authority.ts`, `sign-${operation}`, "--directory", dirname(trustPath!), "--input", "-", "--output", "-"], { stdin: "pipe", stdout: "pipe", stderr: "pipe", cwd: gate });
  signer.stdin.write(JSON.stringify({ operation, activeChallengeId, controlSessionBindingSha256: binding, request }));
  await signer.stdin.end();
  const [output, errors, code] = await Promise.all([new Response(signer.stdout).text(), new Response(signer.stderr).text(), signer.exited]);
  // The authority's errors are closed categories, and the grant is never in them.
  if (code !== 0) throw new Error(`signer_failed_${code}_${errors.match(/category=([a-z_]+)/)?.[1] ?? "unknown"}`);
  return JSON.parse(output).authorization;
}

/** A lease through the page until the pool acknowledges a share, then heartbeats stop, and the record is read to its end. */
async function shareAttempt() {
  await call("useShareScope");
  await connect();
  const review = await call("review");
  const binding = await call("binding");
  keep(await call("shareStatus", null));
  const leaseId = `lease_${attemptId.replaceAll("-", "a").replaceAll("_", "b")}`;
  const request = {
    protocolVersion: "bwg-worker-controller/0.4",
    leaseId,
    challengeId: await call("challengeId"),
    durationMilliseconds: 60_000,
    renewAfterMilliseconds: 20_000,
    stratum: { endpoint: `stratum+tcp://${ready.listenIpv4}:${ready.listenPort}/`, profile: "bwg-worker-stratum-v2-standard/0.1", authorityPublicKey: ready.authorityPublicKey, userIdentity: USER },
    qualificationAttempt: { schema: "worker-qualification-attempt-v1", id: attemptId, ordinal: review.next_ordinal, purpose: "normal", maximumActiveMilliseconds: 180_000 },
  };
  writeFileSync(`${root}.qualification.json`, JSON.stringify(request.qualificationAttempt), { mode: 0o600 });
  await call("startLease", { ...request, authorization: await sign("start", request, request.challengeId, binding) });
  report("lease_started", {});
  const renewal = { protocolVersion: request.protocolVersion, leaseId, durationMilliseconds: 60_000, renewAfterMilliseconds: 20_000 };
  let nextRenewal = performance.now() + 20_000;
  for (let i = 0; i < 400; i++) {
    const status = await call("shareStatus", attemptId);
    keep(status);
    if (status.state === "terminal") return status;
    if (status.record?.shareFacts?.some((fact: any) => fact.ackAtDeviceUs !== null)) {
      keepSafety(await call("status"), status.observation.observedAtUs);
      break;
    }
    if (performance.now() >= nextRenewal) {
      await call("renewLease", { ...renewal, authorization: await sign("renew", renewal, request.challengeId, binding) });
      nextRenewal = performance.now() + 20_000;
    }
    await sleep(POLL_MS);
  }
  await call("suppressHeartbeats", true);
  report("heartbeats_suppressed", {});
  // Missing heartbeats end the session, which is the fault itself. The record outlives it.
  for (let i = 0; i < 40; i++) {
    const status = await call("shareStatus", attemptId).catch(() => null);
    if (status === null) break;
    keep(status);
    keepSafety(await call("status"), status.observation.observedAtUs);
    await sleep(POLL_MS);
  }
  await call("suppressHeartbeats", false);
  return followShare();
}

/** The stop can outlast the session that started it, so each reading is over a session of its own. */
async function followShare() {
  for (let session = 0; session < 40; session++) {
    try {
      await connect();
      for (;;) {
        await call("refreshBinding");
        const status = await call("shareStatus", attemptId);
        keep(status);
        keepSafety(await call("status"), status.observation.observedAtUs);
        if (status.state === "terminal") return status;
        await sleep(1_000);
      }
    } catch (error) {
      report("follow_retry", { category: (error as Error).message.slice(0, 120) });
      await sleep(3_000);
    }
  }
  throw new Error("share_never_finished");
}

if (kind === "share") {
  const terminal = await shareAttempt();
  report("terminal", { state: terminal.state, outcome: terminal.record?.outcome });
  report("fixture_exit", { code: await fixture.exited });
  await call("close");
  server.stop(true);
  report("root", { root });
  process.exit(terminal.record?.outcome === "accepted" ? 0 : 1);
}

await connect();
const statusCall = kind === "noise" ? "noiseStatus" : "channelStatus";
const idle = await call(statusCall, null);
const common = { attemptId, expectedBootOrdinal: idle.observation.bootOrdinal, networkObservedAtUs: idle.observation.observedAtUs };
const start =
  kind === "noise"
    ? { schema: "worker-noise-diagnostic-start-v2", ...common, fixtureIpv4: ready.listenIpv4, fixturePort: ready.listenPort, authorityPublicKey: ready.authorityPublicKey }
    : {
        schema: "worker-stratum-v2-channel-start-v1",
        ...common,
        stratum: {
          endpoint: `stratum+tcp://${ready.listenIpv4}:${ready.listenPort}/`,
          profile: "bwg-worker-stratum-v2-standard/0.1",
          authorityPublicKey: ready.authorityPublicKey,
          userIdentity: USER,
        },
      };
writeFileSync(`${root}.start.json`, JSON.stringify(start), { mode: 0o600 });
if (kind === "channel") keep(idle);
keep(await call(kind === "noise" ? "noiseStart" : "channelStart", start));
let last: any;
for (let i = 0; i < POLLS; i++) {
  last = await call(statusCall, attemptId);
  keep(last);
  if (last.state === "terminal") break;
  await sleep(POLL_MS);
}
const outcome = kind === "noise" ? last.job?.terminal?.outcome : last.record?.outcome;
report("terminal", { state: last.state, outcome });
report("fixture_exit", { code: await fixture.exited });
await call("close");
server.stop(true);
report("root", { root });
process.exit(outcome === "accepted" ? 0 : 1);
