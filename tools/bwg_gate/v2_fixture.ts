#!/usr/bin/env bun
// Runs the Rust repository's Stratum V2 fixture, on the pipes its V2 modes require:
// `bun v2_fixture.ts <binary> <root> <channel|share|noise> <attemptId> <peer> [user]`.
// Its readiness line goes to stdout, and it holds private addresses and a public key, so keep it private.
import { type ChildProcess, spawn } from "node:child_process";
import { createSocket } from "node:dgram";
import { createWriteStream } from "node:fs";
import type { Readable, Writable } from "node:stream";

/** The local address the route to `peer` leaves from, found without sending anything. */
async function routeIpv4To(peer: string): Promise<string> {
  const probe = createSocket("udp4");
  await new Promise<void>((done, fail) => {
    probe.once("error", fail);
    probe.connect(9, peer, done);
  });
  const { address } = probe.address();
  probe.close();
  return address;
}

function exitCodeOf(child: ChildProcess): Promise<number> {
  return new Promise((done) => child.once("exit", (code, signal) => done(code ?? (signal ? 128 : 1))));
}

function stopWithParent(child: ChildProcess) {
  process.once("SIGTERM", () => {
    child.kill("SIGKILL");
    process.exit(143);
  });
}

/** The Noise mode's bounds are fixed by the fixture, and it writes its readiness to its root. */
async function runNoise(binary: string, root: string, attempt: string, peer: string): Promise<number> {
  const listen = await routeIpv4To(peer);
  const child = spawn(binary, [
    "--mode", "noise-serial", "--listen-address", `${listen}:0`, "--expected-peer-address", peer,
    "--accept-timeout-seconds", "120", "--read-timeout-seconds", "10", "--lifetime-seconds", "150",
    "--session-timeout-seconds", "180", "--private-root", root, "--attempt-id", attempt,
  ], { stdio: ["ignore", "ignore", "inherit"] });
  stopWithParent(child);
  return exitCodeOf(child);
}

/** The first line on `stream`, or undefined if it ends first. The stream closes after it, as the fixture writes only one. */
async function maybeFirstLine(stream: Readable): Promise<string | undefined> {
  let text = "";
  for await (const chunk of stream) {
    text += chunk.toString();
    const end = text.indexOf("\n");
    if (end >= 0) {
      stream.destroy();
      return text.slice(0, end + 1);
    }
  }
  return text === "" ? undefined : text;
}

async function runChannel(binary: string, root: string, scope: string, attempt: string, peer: string, user: string): Promise<number> {
  const listen = await routeIpv4To(peer);
  const child = spawn(binary, ["--mode", "v2-serial", "--scope", scope, "--private-root", root, "--attempt-id", attempt], {
    stdio: ["pipe", "pipe", "inherit", "pipe"],
  });
  stopWithParent(child);
  const exited = exitCodeOf(child);
  const input = {
    schema: "str005-v2-fixture-input-v1",
    scope,
    attemptId: attempt,
    listenIpv4: listen,
    expectedPeerIpv4: peer,
    userIdentity: user,
  };
  (child.stdin as Writable).end(`${JSON.stringify(input)}\n`);
  const maybeReady = await maybeFirstLine(child.stdout as Readable);
  if (maybeReady === undefined) {
    return (await exited) || 1;
  }
  process.stdout.write(maybeReady.endsWith("\n") ? maybeReady : `${maybeReady}\n`);
  // The connection facts land next to the root: the fixture refuses a root it didn't create.
  const connection = createWriteStream(`${root}.connection.json`, { mode: 0o600 });
  await new Promise<void>((done, fail) => {
    (child.stdio[3] as Readable).pipe(connection).once("finish", done).once("error", fail);
  });
  return exited;
}

const [binary, root, scope, attempt, peer, user] = process.argv.slice(2);
if (!binary || !root || !scope || !attempt || !peer) {
  throw new Error("usage: v2_fixture.ts <binary> <root> <channel|share|noise> <attemptId> <peer> [user]");
}
if (scope !== "noise" && !user) {
  throw new Error("v2_fixture.ts: a channel or share attempt needs a user");
}
process.exit(scope === "noise" ? await runNoise(binary, root, attempt, peer) : await runChannel(binary, root, scope, attempt, peer, user!));
