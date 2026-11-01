// Runs the Gate's controller in the page, so a V2 or Noise attempt crosses Chrome's real WebSerial. The driver calls window.attempt.
const { createWebSerialWorkerController, workerSerialQualificationHook } = window.gate;
const context = await (await fetch("/context")).json();
const scope = { challengeId: "challenge_host_harness", retentionExpiryUnixSeconds: Math.floor(Date.now() / 1000) + 3600 };
const pair = { firmwareSourceCommit: context.commit, appElfSha256: context.elf };
const state = { connected: false, failure: null };
const hook = {
  suppressHeartbeats: false,
  memoryOnlyContinuity: true,
  allowClockDiscontinuityStimulus: false,
  observeStatus: () => {},
  observePreservation: () => {},
  maybeObserveAdmissionFailure: (stage) => { state.failure = `admission_${stage}`; },
  maybeObserveSerialFailure: (category) => { state.failure = `serial_${category}`; },
  maybeObserveDiagnostic: () => {},
  async prepareScope() { return scope; },
  noiseDiagnosticPair: pair,
  stratumV2Pair: { ...pair, scope: "channel" },
};
let controller;
let binding;

document.querySelector("#connect").addEventListener("click", async () => {
  state.failure = null;
  state.connected = false;
  await controller?.close().catch(() => {});
  controller = createWebSerialWorkerController({
    deviceFilter: { usbVendorId: 0x303a, usbProductId: 0x1001 },
    trustedUpdateKeys: context.trust.updateAuthority.keys,
    continuityScope: scope,
    expectedFirmwareSourceCommit: context.commit,
    expectedAppElfSha256: context.elf,
    [workerSerialQualificationHook]: hook,
  });
  try {
    await controller.requestPermission();
    ({ controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start"));
    state.connected = true;
  } catch (error) {
    state.failure ??= error.message;
    await controller.close().catch(() => {});
  }
});

window.attempt = {
  state: () => state,
  noiseStatus: (attemptId) => controller.noiseDiagnosticStatus(attemptId, binding),
  noiseStart: (input) => controller.noiseDiagnosticStart(input, binding),
  channelStatus: (attemptId) => controller.stratumV2Status("channel", attemptId, binding),
  channelStart: (input) => controller.stratumV2ChannelStart(input, binding),
  useShareScope: () => { hook.stratumV2Pair = { ...pair, scope: "share" }; },
  challengeId: () => scope.challengeId,
  binding: () => binding,
  refreshBinding: async () => { ({ controlSessionBindingSha256: binding } = await controller.prepareWorkerLeaseAuthorizationContext("start")); },
  review: () => controller.qualificationAttemptReview(),
  startLease: (request) => controller.startLease(request),
  renewLease: (request) => controller.renewLease(request),
  status: () => controller.status(),
  shareStatus: (attemptId) => controller.stratumV2Status("share", attemptId, binding),
  suppressHeartbeats: (suppress) => { hook.suppressHeartbeats = suppress; },
  close: () => controller.close(),
};
