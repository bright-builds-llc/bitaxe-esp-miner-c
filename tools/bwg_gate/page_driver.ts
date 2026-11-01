#!/usr/bin/env bun
// Drives the Gate's restoration page in a dedicated Chrome profile, through the DevTools protocol.
// Clicks arrive as trusted input, as the page's connect gesture requires. Chrome's port chooser needs
// a person once per profile; the grant then stays in the profile and later connects reuse it.
// Start Chrome first, with --remote-debugging-port=9333 and a --user-data-dir of its own.

const DEBUG_PORT = 9333;

const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));

export class Page {
  #socket: WebSocket;
  #next = 1;
  #pending = new Map<number, (value: any) => void>();
  constructor(socket: WebSocket) {
    this.#socket = socket;
    socket.onmessage = (event) => {
      const message = JSON.parse(String(event.data));
      if (message.id) {
        this.#pending.get(message.id)?.(message);
        this.#pending.delete(message.id);
      }
    };
  }
  static async attach(url: string): Promise<Page> {
    for (let attempt = 0; attempt < 60; attempt++) {
      try {
        const targets: any[] = await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json();
        const target = targets.find((value) => value.type === "page" && value.url.startsWith(url));
        if (target) {
          const socket = new WebSocket(target.webSocketDebuggerUrl);
          await new Promise((done, failed) => { socket.onopen = done; socket.onerror = failed; });
          return new Page(socket);
        }
      } catch { /* Chrome is still starting. */ }
      await sleep(500);
    }
    throw new Error("page_not_found");
  }
  send(method: string, params: object = {}): Promise<any> {
    const id = this.#next++;
    return new Promise((done) => {
      this.#pending.set(id, done);
      this.#socket.send(JSON.stringify({ id, method, params }));
    });
  }
  async evaluate(expression: string): Promise<any> {
    const { result } = await this.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true, userGesture: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.message ?? result.exceptionDetails.text);
    return result.result.value;
  }
  // `selector` must be on screen, since this is real pointer input.
  async click(selector: string): Promise<void> {
    await this.send("Page.bringToFront");
    const box = await this.evaluate(`(() => { const r = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
    for (const type of ["mouseMoved", "mousePressed", "mouseReleased"]) {
      await this.send("Input.dispatchMouseEvent", { type, x: box.x, y: box.y, button: type === "mouseMoved" ? "none" : "left", clickCount: type === "mouseMoved" ? 0 : 1 });
    }
  }
}
