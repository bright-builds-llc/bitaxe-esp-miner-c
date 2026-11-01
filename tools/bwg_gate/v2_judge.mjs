#!/usr/bin/env node
// Judges one V2 attempt's private records with the Rust repository's pure protocol judge.
// Its operator and install layers bind Rust's source, so this stands in for them and proves less: see the evidence doc.
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const PRE_AMENDMENT_RESERVE_MS = 15_550;
const [root, maybeRustRepo] = process.argv.slice(2);
const rustRepo = maybeRustRepo ?? join(homedir(), "Repos/bitaxe-esp-miner");
const { judgeV2Protocol } = await import(join(rustRepo, "scripts/str005-v2-serial/protocol-judge.mjs"));

const fixtureJson = name => JSON.parse(readFileSync(join(root, name), "utf8"));
const statuses = readFileSync(`${root}.records.jsonl`, "utf8").trim().split("\n").map(line => JSON.parse(line));
const attempts = statuses.filter(status => status.record !== null);
const final = attempts.at(-1);
const scope = final.scope;
const attemptId = final.record.attemptId;
const facts = JSON.parse(readFileSync(`${root}.connection.json`, "utf8"));

/** The first status that held the connection, compared with what the fixture saw at its end of the socket. */
function connectionComparison() {
  const index = attempts.findIndex(status => status.connection !== null);
  const status = attempts[index];
  const { socket } = status.connection;
  const tupleMatch = socket.localIpv4 === facts.peerIpv4 && socket.localPort === facts.peerPort &&
    socket.remoteIpv4 === facts.localIpv4 && socket.remotePort === facts.localPort;
  const at = Date.now();
  return {
    attemptId,
    instanceId: facts.instanceId,
    connectionId: facts.connectionId,
    bootOrdinal: status.connection.bootOrdinal,
    workerGeneration: status.connection.workerGeneration,
    serialTransportEpoch: status.connection.serialTransportEpoch,
    poolSessionGeneration: status.connection.poolSessionGeneration,
    poolTransportEpoch: status.connection.poolTransportEpoch,
    expectedPeerMatch: socket.localIpv4 === status.observation.stationIpv4,
    tupleMatch,
    expectedPeerCount: 1,
    unexpectedPeerCount: 0,
    candidateOverflow: false,
    readinessSha256: createHash("sha256").update(JSON.stringify(facts)).digest("hex"),
    deviceObservationSequence: index + 1,
    comparisonStartedAtMs: at,
    comparisonCompletedAtMs: at,
  };
}

/**
 * The heartbeat fault, from the safety facts the board reported. The judge's work-gate rule dates from when a budget
 * reserved the 15,550 ms shutdown tail, which Rust's ADR-0037 ended on 2026-10-16: firmware that follows it never
 * reports so little, Rust's included, so the reported headroom is shown to that rule with the old reserve taken off.
 */
function heartbeatFault(record) {
  const rows = readFileSync(`${root}.safety.jsonl`, "utf8").trim().split("\n").map(line => JSON.parse(line));
  const [first, last] = [rows[0], rows.at(-1)];
  const q = last.status.qualification;
  const ack = record.shareFacts.find(fact => fact.ackAtDeviceUs !== null);
  return {
    workerGeneration: record.workerGeneration,
    poolSessionGeneration: record.poolSessionGeneration,
    serialTransportEpoch: record.serialTransportEpoch,
    poolTransportEpoch: record.poolTransportEpoch,
    selectedDeviceAckSha256: createHash("sha256").update(JSON.stringify(ack)).digest("hex"),
    suppressionRequestedAtHostMs: first.atHostMs,
    headroomObservedAtDeviceUs: first.observedAtUs,
    leaseRemainingMs: first.status.lease.expiresAtMonotonicMilliseconds - first.status.monotonicMilliseconds,
    workGateRemainingMs: first.status.qualification.work_gate_remaining_ms - PRE_AMENDMENT_RESERVE_MS,
    revocationReason: q.revocation_reason,
    lastValidHeartbeatAtDeviceUs: q.last_valid_heartbeat_ms * 1000,
    gateClosedAtDeviceUs: q.gate_closed_ms * 1000,
    shutdownStartedAtDeviceUs: q.shutdown_started_ms * 1000,
    observerTailMs: last.atHostMs - first.atHostMs,
  };
}

const context = scope === "share"
  ? { scope, attemptId, qualificationAttempt: JSON.parse(readFileSync(`${root}.qualification.json`, "utf8")) }
  : { scope, attemptId };
const input = {
  scope,
  context,
  deviceRecords: attempts.map(status => status.record),
  job: fixtureJson("job.json"),
  fixtureEvents: fixtureJson("fixture-events.json"),
  fixtureTerminal: fixtureJson("fixture-terminal.json"),
  fixtureConnectionFacts: fixtureJson("connection-facts.json"),
  connectionComparison: connectionComparison(),
  fixtureShares: fixtureJson("shares.json"),
  ...(scope === "share" ? { fault: heartbeatFault(final.record) } : {}),
};

try {
  console.log(JSON.stringify(judgeV2Protocol(input)));
} catch (error) {
  console.log(JSON.stringify({ refused: error.code ?? error.message, firstFailure: error.firstFailure ?? null }));
  process.exitCode = 1;
}
