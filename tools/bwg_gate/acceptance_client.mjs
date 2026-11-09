// Loaded beside the Gate's acceptance page. It reports the page's own state to the supervisor, in order,
// and never touches the serial port.
let queue = Promise.resolve();
let recordFailed = false;

async function post(route, body) {
  const response = await fetch(route, { method: "POST", headers: { "Content-Type": "application/json" }, cache: "no-store", body: JSON.stringify(body), keepalive: true });
  if (!response.ok) throw new Error("supervisor_request_rejected");
  return response.json();
}

function enqueue(operation) {
  queue = queue.then(operation).catch(() => { recordFailed = true; });
}

const current = () => window.workerAcceptance?.state();

new MutationObserver(() => {
  const state = current();
  if (!state?.expectedFirmwareSourceCommit) return;
  enqueue(async () => {
    await post("/record", { state });
    if (state.running && state.heartbeatSuppressed) {
      await post("/fault", { kind: "heartbeats_suppressed", running: true, visibility: document.visibilityState, heartbeatSuppressed: true, generation: state.qualification?.generation });
    }
  });
}).observe(document.querySelector("#state"), { childList: true, subtree: true, characterData: true });

// Since the Gate's ADR-0110 a session ends when the page is left, not hidden; the fault keeps its wire kind,
// as the purpose keeps `foreground_loss`. Capture runs before the SDK's own exit listener, so the
// snapshot still shows the running lease.
window.addEventListener("pagehide", () => {
  const state = current();
  if (!state?.running) return;
  enqueue(() => post("/fault", { kind: "visibility_hidden", running: true, visibility: document.visibilityState, heartbeatSuppressed: state.heartbeatSuppressed, generation: state.qualification?.generation }));
}, { capture: true });

window.recoverySupervisor = {
  async flush() {
    await Promise.resolve();
    await queue;
    if (recordFailed) throw new Error("supervisor_record_rejected");
    return { records_flushed: true };
  },
};
