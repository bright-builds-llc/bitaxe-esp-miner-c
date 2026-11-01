#!/usr/bin/env node
// Judges one Noise diagnostic with the Rust repository's pure protocol join: `node noise_judge.mjs <root>`.
// The Rust repository's whole-qualification judge is bound to its own install and flash steps, so this covers
// what the device and the fixture said of the exchange, and nothing of how the image got onto the board.
import { existsSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { resolve } from "node:path";

const rust = process.env.BWG_RUST_REPO ?? resolve(homedir(), "Repos/bitaxe-esp-miner");
const { parseNoiseStatusV2, validateNoiseProgressV2 } = await import(`${rust}/scripts/str005-noise-serial/device-v2.mjs`);
const { inspectProtocolV2 } = await import(`${rust}/scripts/str005-noise-serial/protocol-v2.mjs`);

const root = process.argv[2];
const json = (path) => JSON.parse(readFileSync(path, "utf8"));
const statuses = readFileSync(`${root}.records.jsonl`, "utf8").trim().split("\n").map((line) => JSON.parse(line));
statuses.forEach((status) => parseNoiseStatusV2(status));
statuses.slice(1).forEach((status, index) => validateNoiseProgressV2(statuses[index], status));
const job = statuses.at(-1).job;
const verdict = {
  statuses: statuses.length,
  state: statuses.at(-1).state,
  outcome: job.terminal?.outcome ?? null,
  firstFailure: job.firstFailure && `${job.firstFailure.stage}/${job.firstFailure.category}/${job.firstFailure.detail}`,
  resources: { socket: job.resources.socketState, worker: job.resources.workerState, disposed: job.resources.volatileInputsDisposed, deadlineMet: job.resources.deadlineMet },
};
if (job.terminal?.outcome === "accepted") {
  if (!existsSync(`${root}/terminal.json`)) throw new Error("noise_fixture_terminal_missing");
  const { timings } = inspectProtocolV2(json(`${root}.start.json`), statuses, json(`${root}/ready.json`), json(`${root}/terminal.json`));
  verdict.acceptedProtocol = true;
  verdict.timingsMs = timings;
}
console.log(JSON.stringify(verdict));
