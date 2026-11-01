#!/usr/bin/env bun
// Serves the Gate's own restoration page and signs its leases with a test authority, on loopback only.
// It logs route names and never a credential, so its output is safe to keep with evidence.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";

// `serialPort` and `bridgePython` add /serial, for a page running with `navigator.serial` shimmed over it.
type Options = { gate: string; authority: string; trust: string; pool: string; commit: string; elf: string; gateCommit: string; port: string; serialPort?: string; bridgePython?: string };

function parseOptions(args: string[]): Options {
  const options: Record<string, string> = { port: "48765" };
  for (let i = 0; i < args.length; i += 2) options[args[i]!.replace(/^--/, "")] = args[i + 1]!;
  for (const name of ["gate", "authority", "trust", "pool", "commit", "elf", "gateCommit"]) {
    if (!options[name]) throw new Error(`missing_option_${name}`);
  }
  return options as Options;
}

const options = parseOptions(process.argv.slice(2));
const gate = resolve(options.gate);
const scope = { challengeId: `challenge_restoration${Date.now()}`, retentionExpiryUnixSeconds: Math.floor(Date.now() / 1000) + 86_400 };

// Changed through /control/window between runs, since a page only ever loads one lease at a time.
let window = { durationMilliseconds: 60_000, renewAfterMilliseconds: 20_000, renewals: 1 };
let maybeBinding: string | undefined;
// What the page replays unchanged, for the device to refuse: the last Start signed here.
let maybeLastGrant: unknown;
let maybeLastRenewal: unknown;
let replayOperation = "start";
let checkpoint = "idle";
let completionScenario = "completion";
let maybeBridge: ReturnType<typeof Bun.spawn> | undefined;

async function sign(operation: "start" | "renew", request: unknown): Promise<string> {
  if (!maybeBinding) throw new Error("binding_missing");
  const signer = Bun.spawn(["bun", `${gate}/scripts/worker-development-authority.ts`, `sign-${operation}`, "--directory", options.authority, "--input", "-", "--output", "-"], { stdin: "pipe", stdout: "pipe", stderr: "pipe", cwd: gate });
  signer.stdin.write(JSON.stringify({ operation, activeChallengeId: scope.challengeId, controlSessionBindingSha256: maybeBinding, request }));
  await signer.stdin.end();
  const [output, errors, code] = await Promise.all([new Response(signer.stdout).text(), new Response(signer.stderr).text(), signer.exited]);
  if (code !== 0) {
    // The authority's errors are closed categories, and the grant is never in them.
    throw new Error(`signer_failed_${code}_${errors.match(/category=([a-z_]+)/)?.[1] ?? "unknown"}`);
  }
  return JSON.parse(output).authorization;
}

async function scenarioArtifacts() {
  const pool = JSON.parse(readFileSync(options.pool, "utf8"));
  const leaseId = `lease_restoration${Date.now()}`;
  const { durationMilliseconds, renewAfterMilliseconds } = window;
  const protocolVersion = "bwg-worker-controller/0.4";
  const start = {
    protocolVersion, leaseId, challengeId: scope.challengeId, durationMilliseconds, renewAfterMilliseconds,
    stratum: { endpoint: `stratum+tcp://${pool.poolURL}:${pool.poolPort}/`, username: pool.poolUser, password: pool.poolPassword, suggestedDifficulty: 1000 },
  };
  const grant = { ...start, authorization: await sign("start", start) };
  maybeLastGrant = grant;
  const renewals = [];
  for (let i = 0; i < window.renewals; i++) {
    const renewal = { protocolVersion, leaseId, durationMilliseconds, renewAfterMilliseconds };
    renewals.push({ ...renewal, authorization: await sign("renew", renewal) });
    maybeLastRenewal = renewals[renewals.length - 1];
  }
  return { grant, renewals };
}

function json(value: unknown) {
  return new Response(JSON.stringify(value), { headers: { "content-type": "application/json", "cache-control": "no-store" } });
}

Bun.serve({
  hostname: "127.0.0.1",
  port: Number(options.port),
  idleTimeout: 120,
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
    console.log(request.method, pathname);
    if (pathname === "/serial" && options.serialPort && options.bridgePython) {
      return server.upgrade(request) ? undefined : new Response("upgrade refused", { status: 400 });
    }
    // The unplug a disconnect scenario needs, from the page's point of view.
    if (pathname === "/control/drop-serial") {
      maybeBridge?.kill();
      return json({ dropped: maybeBridge !== undefined });
    }
    if (pathname === "/control/replay") {
      replayOperation = String(searchParams.get("operation"));
      return json({ operation: replayOperation });
    }
    if (pathname === "/control/scenario") {
      completionScenario = String(searchParams.get("name"));
      return json({ scenario: completionScenario });
    }
    if (pathname === "/") return new Response(Bun.file(`${gate}/conformance/bwg-worker-serial-0.2/restoration.html`), { headers: { "content-type": "text/html" } });
    if (pathname === "/dist/worker-restoration/worker-restoration-page.js") return new Response(Bun.file(`${gate}/dist/worker-restoration/worker-restoration-page.js`), { headers: { "content-type": "text/javascript" } });
    if (pathname === "/context") {
      return json({
        expectedGateCommit: options.gateCommit, expectedFirmwareSourceCommit: options.commit, expectedAppElfSha256: options.elf,
        trust: JSON.parse(readFileSync(options.trust, "utf8")), restorationQualification: true,
      });
    }
    if (pathname === "/activate") return json(scope);
    if (pathname === "/authorization-context") {
      maybeBinding = (await request.json()).controlSessionBindingSha256;
      return json({});
    }
    if (pathname === "/scenario-artifacts") return json(await scenarioArtifacts());
    if (pathname === "/replay-artifact") {
      if (replayOperation === "renew") return json({ operation: "renew", renewal: maybeLastRenewal });
      return json({ operation: "start", grant: maybeLastGrant });
    }
    if (pathname === "/physical-window") {
      if (request.method === "POST") checkpoint = (await request.json()).event === "begin" ? "begun" : "armed";
      return json({ checkpoint });
    }
    if (pathname === "/completion-context") return json({ nonce: crypto.randomUUID() });
    // This supervisor judges nothing: the driver reads the reviews, and the page's receipt stays unverified.
    if (pathname === "/completion-review") return json({ result: "unverified", scenario: completionScenario, cleanup_confirmed: true });
    if (pathname === "/control/window") {
      window = { durationMilliseconds: Number(searchParams.get("duration")), renewAfterMilliseconds: Number(searchParams.get("renewAfter")), renewals: Number(searchParams.get("renewals")) };
      return json(window);
    }
    return new Response("not found", { status: 404 });
  },
});
console.log("restoration supervisor ready");
