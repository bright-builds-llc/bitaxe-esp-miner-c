#!/usr/bin/env bun
// Runs the restoration scenarios through the Gate's own page in Chrome, one page lifetime each.
// With --shim the page's `navigator.serial` is a stand-in that reaches the board over the supervisor's
// /serial, so everything but Chrome's own serial stack is the real page. Without it, the page uses Chrome's
// serial port, which needs the profile's grant first (see page_driver.ts).
import { Page } from "./page_driver.ts";

const SUPERVISOR = "http://127.0.0.1:48765";
const SHIM = `(() => {
  let socket;
  const port = {
    readable: null, writable: null, connected: true,
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    addEventListener() {}, removeEventListener() {},
    async open() {
      socket = new WebSocket("ws://127.0.0.1:48765/serial");
      socket.binaryType = "arraybuffer";
      await new Promise((opened, failed) => { socket.onopen = opened; socket.onerror = () => failed(new DOMException("open", "NetworkError")); });
      const current = socket;
      port.readable = new ReadableStream({
        start(controller) {
          current.onmessage = (event) => controller.enqueue(new Uint8Array(event.data));
          current.onclose = () => { try { controller.close(); } catch {} };
        },
        cancel() { current.close(); },
      });
      port.writable = new WritableStream({ write(chunk) { current.send(chunk); } });
    },
    async close() { socket?.close(); port.readable = null; port.writable = null; },
  };
  const serial = { getPorts: async () => [port], requestPort: async () => port, addEventListener() {}, removeEventListener() {} };
  Object.defineProperty(Navigator.prototype, "serial", { get: () => serial, configurable: true });
})();`;

const args = process.argv.slice(2);
const shim = args.includes("--shim");
const deviceIp = args[args.indexOf("--device-ip") + 1]!;
const dropCommand = args.includes("--drop-command") ? args[args.indexOf("--drop-command") + 1]! : undefined;
const wanted = args.filter((value, index) => !value.startsWith("--") && args[index - 1] !== "--device-ip" && args[index - 1] !== "--drop-command");

const started = performance.now();
const report = (event: string, detail: unknown = {}) => console.log(JSON.stringify({ t: Math.round(performance.now() - started), event, ...(detail as object) }));
const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));

const page = await Page.attach(SUPERVISOR);
await page.send("Page.enable");
// The Gate's qualification supervisor flushes its own recovery records here. This run keeps none.
await page.send("Page.addScriptToEvaluateOnNewDocument", { source: "globalThis.recoverySupervisor = { flush: async () => {} };" });
if (shim) await page.send("Page.addScriptToEvaluateOnNewDocument", { source: SHIM });

const call = (operation: string) => page.evaluate(`(async () => { try { return { ok: await window.workerRestoration.${operation}() }; } catch (error) { return { failed: error.message }; } })()`);
const state = () => page.evaluate("window.workerRestoration.state()");

async function step(operation: string): Promise<any> {
  const result = await call(operation);
  if (result.failed) throw new Error(`${operation}: ${result.failed}`);
  report(operation);
  return result.ok;
}

async function reloadPage() {
  await page.send("Page.reload", { ignoreCache: true });
  await page.send("Page.bringToFront");
  for (let attempt = 0; attempt < 40; attempt++) {
    await sleep(250);
    const status = await page.evaluate("window.workerRestoration?.state().status").catch(() => undefined);
    if (status === "configured") return;
  }
  throw new Error("page_not_configured");
}

// A refused Hello's handover takes until the third attempt from local mining, as with the Gate's own retries.
async function connect() {
  await page.click("#connect");
  for (let attempt = 1; attempt <= 20; attempt++) {
    await sleep(1_000);
    const current = await state();
    if (current.connected) { report("connected", { attempt, device: current.device }); return; }
    if (current.status !== "failed") continue;
    await sleep(9_000);
    const retried = await call("reconnect");
    if (retried.ok) { report("connected", { attempt, retried: true }); return; }
  }
  throw new Error("connect_timeout");
}

async function beginLease(duration: number, renewAfter: number, renewals: number) {
  await fetch(`${SUPERVISOR}/control/window?duration=${duration}&renewAfter=${renewAfter}&renewals=${renewals}`);
  await step("prepareStart");
  await step("loadScenarioLease");
  const before = performance.now();
  await step("startScenarioLease");
  report("lease_running", { replyMs: Math.round(performance.now() - before) });
}

// A status sent during a stop has no reply until the stop ends, and the Gate gives up after 30 s. The 601's cooling
// proof takes 30 s however cool the board is, which carries its stop past that, so these wait before asking.
const waitOutStop = (windowMs = 0) => process.env.BWG_BOARD === "601" ? sleep(windowMs + 50_000) : Promise.resolve();

async function awaitBaseline(ms: number) {
  const end = performance.now() + ms;
  while (performance.now() < end) {
    await sleep(2_000);
    const status = await step("statusReview");
    if (status?.state === "baseline") return status;
  }
  throw new Error("baseline_timeout");
}

// The shim's bridge ends. Natively, --drop-command takes the board off the USB bus; the chip keeps running, so the
// board sees what a pulled cable would show it, but this is not a pulled cable.
async function dropTransport() {
  if (shim) { await fetch(`${SUPERVISOR}/control/drop-serial`); return; }
  if (!dropCommand) throw new Error("drop_command_required");
  const dropped = Bun.spawn(dropCommand.split(" "), { stdout: "ignore", stderr: "inherit" });
  if (await dropped.exited !== 0) throw new Error("drop_command_failed");
}

const reconnectAfter = async (ms: number) => { await sleep(ms); await connect(); return step("statusReview"); };

const scenarios: Record<string, () => Promise<unknown>> = {
  async completion() {
    await connect();
    report("boot", { review: await step("bootReview") });
    await beginLease(60_000, 20_000, 1);
    await sleep(21_000);
    await step("renewOnce");
    await sleep(5_000);
    await step("restoreChallengeSatisfied");
    report("after_stop", { status: await step("statusReview") });
    return step("submitCompletion");
  },
  async pause() {
    await connect();
    await beginLease(60_000, 20_000, 1);
    await sleep(5_000);
    await step("pause");
    report("after_stop", { status: await step("statusReview") });
    return step("close");
  },
  async cancel() {
    await connect();
    await beginLease(60_000, 20_000, 1);
    await sleep(5_000);
    await step("cancel");
    report("after_stop", { status: await step("statusReview") });
    return step("close");
  },
  async expiry() {
    await connect();
    await beginLease(30_000, 10_000, 0);
    await waitOutStop(30_000);
    report("after_stop", { status: await awaitBaseline(90_000) });
    return step("close");
  },
  async monotonic_uncertainty() {
    await connect();
    await beginLease(60_000, 20_000, 1);
    report("stimulus", { ack: await step("triggerClockDiscontinuity") });
    await waitOutStop();
    report("after_stop", { status: await awaitBaseline(60_000) });
    report("stimulus_review", { review: await step("clockDiscontinuityStimulusReview") });
    report("rejection_review", { review: await step("authorizationRejectionReview") });
    return step("close");
  },
  async disconnect() {
    await connect();
    await beginLease(60_000, 20_000, 1);
    await sleep(5_000);
    await dropTransport();
    report("dropped");
    await sleep(3_000);
    const dropped = await state();
    report("page_after_drop", { connected: dropped.connected, status: dropped.status });
    await call("close");
    report("after_stop", { status: await reconnectAfter(25_000) });
    return step("close");
  },
  async reboot() {
    await connect();
    await beginLease(60_000, 20_000, 1);
    report("checkpoint", await step("beginPhysicalWindow"));
    report("restart_requested", { http: (await fetch(`http://${deviceIp}/api/system/restart`, { method: "POST" })).status });
    report("checkpoint", await step("armPhysicalWindow"));
    await sleep(3_000);
    await call("close");
    report("after_stop", { status: await reconnectAfter(30_000) });
    report("boot", { review: await step("bootReview") });
    return step("close");
  },
  async authorization_negatives() {
    await connect();
    await beginLease(60_000, 20_000, 1);
    await sleep(21_000);
    await step("renewOnce");
    // A signed renewal again: the signature holds, and the device's high water refuses it.
    await fetch(`${SUPERVISOR}/control/replay?operation=renew`);
    report("replay", { outcome: await step("replayArtifact") });
    report("after_stop", { status: await reconnectAfter(30_000) });
    report("rejection_review", { review: await step("authorizationRejectionReview") });
    // The earlier Start under this session's possession: the signed binding no longer matches.
    await fetch(`${SUPERVISOR}/control/replay?operation=start`);
    await step("prepareStart");
    report("replay", { outcome: await step("replayArtifact") });
    report("after_stop", { status: await reconnectAfter(30_000) });
    report("rejection_review", { review: await step("authorizationRejectionReview") });
    return step("close");
  },
};

let failures = 0;
for (const name of wanted.length ? wanted : Object.keys(scenarios)) {
  report("scenario", { name, shim });
  await fetch(`${SUPERVISOR}/control/scenario?name=${name}`);
  await reloadPage();
  try {
    report("scenario_done", { name, result: await scenarios[name]!() });
  } catch (error) {
    failures++;
    report("scenario_failed", { name, failure: (error as Error).message });
  }
  const final = await state();
  report("journal", { name, status: final.status, failure: final.failure, device: final.device, admission: final.admission, journal: final.journal });
  await call("close");
  await sleep(25_000);
}
report("finished", { failures });
process.exit(failures ? 1 : 0);
