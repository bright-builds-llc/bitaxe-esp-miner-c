# ESP-Miner Development Guide for Agents

Welcome to the ESP-Miner repository. This document provides a high-level overview of the project structure, build instructions, and CI setup to help you navigate and contribute effectively.

## Project Overview
ESP-Miner is the firmware powering the Bitaxe open-source Bitcoin ASIC miners. It is built on the ESP-IDF framework (v5.x+) for the ESP32-S3 and includes a modern web interface (Axe-OS) built with Angular.

## Repository Structure

```text
.
├── .github/workflows/          # CI/CD pipelines (GitHub Actions)
├── bootloader_components/      # Custom components for the 2nd stage bootloader
├── components/                 # Shared application components (asic, stratum, etc.)
├── main/                       # Main application source code
│   └── http_server/            # C-based HTTP server logic
│       └── axe-os/             # Axe-OS Angular Frontend
├── configs/                    # Hardware-specific NVS configuration files
├── partitions.csv              # Flash partition layout
└── sdkconfig                   # Project-wide ESP-IDF configuration
```

## Build Instructions

### 1. Firmware (Main Project)
The project uses the standard ESP-IDF build system. Ensure you are using ESP-IDF v6.0.2.

**Prerequisites:**
- ESP-IDF v6.0.2 environment sourced.
- Node.js (v22+) and npm for the frontend bundle.

**Commands:**
```bash
# Source the environment
. ~/esp/v6.0.2/esp-idf/export.sh

# Build the project (automatically builds Axe-OS and generates binaries)
idf.py build

# Flash to device
idf.py build flash
```
*Note: The Axe-OS frontend is automatically built, gzipped, and embedded directly into the firmware (`esp-miner.bin`) as static C arrays as part of the main `idf.py build` process. The default build no longer generates a separate `www.bin` file.*

### 2. Axe-OS (Frontend)
The frontend is a standalone Angular application located in `main/http_server/axe-os`.

**Commands:**
```bash
cd main/http_server/axe-os
npm install

# Build only the frontend
npm run build

# Run local development server
npm run start
```

## Testing

### Internal C Components (Unity & QEMU)
Backend firmware unit tests use the Unity test framework and run under QEMU emulation (`esp32s3` machine target) in CI.

**Execution:**
```bash
bash tools/run_qemu_tests.sh
```
*Note: Test cases tagged with `[not-on-qemu]` (such as hardware-specific ASIC nonce tests) are automatically skipped in QEMU emulation.*

### Axe-OS Frontend
Angular unit tests use Karma and Jasmine. We use a specific CI command for CI environments which ensures consistent reporting and uses a headless browser.

**Execution:**
```bash
cd main/http_server/axe-os
npm run test:ci
```

**Key Points:**
- `npm run test:ci` automatically runs `npm run generate:api` before executing `ng test`.
- It uses a custom `ChromeHeadlessCI` launcher (defined in `karma.conf.js`) with `--no-sandbox` to ensure stability in containerized CI environments.
- Unit tests are highly isolated. **Reminder:** Components with many dependencies (like `HomeComponent`) must have all services, pipes, and sub-components explicitly declared or provided in the `TestBed`.

## CI/CD Setup

We use GitHub Actions for automated testing and releases.

### Key Workflows:
- **`unittest.yml`**: Runs both backend and frontend unit tests in parallel.
- **`build.yml`**: Verifies that the project compiles for all primary configurations.
- **`release.yml`**: Handles automated releases and binary packaging.

## AI Agent Tips
- **API Generation**: If you modify `openapi.yaml`, you **must** run `npm run generate:api` in the `axe-os` directory to update the TypeScript services. This is also automatically handled by `npm run build` and `npm run test:ci`.
- **Node Environment**: If `node` or `npm` are not in your global path, check for local installations in `~/.nvm`. You can source them using `export PATH=~/.nvm/versions/node/v[version]/bin:$PATH`.
- **Modern Angular Testing**: Use functional providers like `provideRouter([])` and `provideHttpClient()` instead of deprecated class-based modules like `RouterTestingModule`.
- **PSRAM**: Bitaxe heavily relies on PSRAM. Always check `esp_psram_is_initialized()` before allocating large buffers in the backend.
- **Mock Data Parity**: When updating `openapi.yaml` and regenerating the API, you **must** update the mock data in `main/http_server/axe-os/src/app/services/system.service.ts`. The TypeScript compiler will fail if properties are missing from the `of()` calls used for development.
- **API Type Safety**: The `SystemInfo` API uses a numeric 0/1 pattern for many boolean-like status fields (e.g., `overclockEnabled`, `overheat_mode`). In the backend, use `cJSON_AddNumberToObject(root, "key", val ? 1 : 0)` to maintain parity with the `integer` types in OpenAPI and support strict equality checks (`=== 1`) in the Angular frontend.
- **Reduce Verbosity**: Use one paragraph to describe changes. No headings, no "What this changes", no multi-line comments.
- **Reuse Existing Code**: Avoid refactoring and making large changes on your own. If a large change or refactor is required, refuse to make any further changes and alert your operator.
- **Minimize Scope**: If it's a contained feature, go for a PR and isolate the changes to that feature, minimizing the scope of the PR. If you have a restructuring of refactoring in mind, open an issue first to align it with ongoing changes.
<!-- BEGIN Bright Builds fork section: everything above is upstream's, keep it on merges. -->
## Bright Builds Fork

This is `bright-builds-llc/bitaxe-esp-miner-c`: upstream's firmware plus the owner's extensions, chiefly Bitcoin Work Gate interoperability, with as small a difference from upstream as possible. Read `docs/project/c-fork-handoff.md` before any work on the Gate, an upstream fix, a fork-only extension or moving a device. It holds the ordered steps, the protocol summary and the hazards.

### Owner Decisions (2026-10-30)
- **Base:** fork work lands on `master`, on top of upstream's `master` at `bb4fd0c`. This replaces the handoff's recommendation of a separate branch from `v2.15.4`. Pull requests to upstream start from `upstream/master`, so they carry none of the fork's commits.
- **Web UI:** keep AxeOS and add what the Gate needs.
- **Device limits:** the catalog range and the target-power stop apply only while a Gate lease is active.
- **BLE:** off in fork builds until the secured design (`task-secured-ble-provisioning`) exists.
- **Device identity:** keep the `bwg_worker` NVS key names and formats, so devices keep their identity.
- **Licence:** GPL-3.0 as a whole, with upstream's notices kept. Code or text moved from the Rust repository keeps its MIT notice. Review source and notice obligations before distributing an image.

### Sync Policy
- Put extensions in new components, such as `components/bwg_worker`, with narrow hooks in upstream files.
- Merge `upstream/master`; history and evidence stay stable that way. Review what each merge brings in and name the checks it invalidates.
- Send a change upstream in preference to carrying it, and follow upstream's conventions in `main/` and `components/`.
- Use upstream's build, adding only the host test harness the fork needs.
- List every upstream file the fork touches below, with the reason, so a merge conflict lands only in this list:
  - `AGENTS.md`: this section and the managed Bright Builds Rules block.
  - `readme.md`: the managed Bright Builds Rules badge block.

### Evidence Rules
- Every claim names its board, commit and evidence, and lists its non-claims. All earlier evidence is about Rust images, so each claim here earns its own.
- Committed evidence is free of IPs, MACs, SSIDs, credentials, pool users, workers and endpoints. Test log redaction against real formats, since vendor Wi-Fi logs leak the SSID.
- Security-sensitive findings go through upstream's private flow (`SECURITY.md`). This repository is public, so commits, issues and pull requests name topics only.
- Hardware work follows "Process rules worth keeping" in the handoff: closed outcomes, a stop on a repeated boundary, and a base-MAC match before any ROM write.

### Hardware Authorization
On 2026-10-30 the owner granted agents working in this repository full hardware autonomy, including direct UART and pin access. The owner also authorized use of the ignored credential and board files in the Rust repository (`~/Repos/bitaxe-esp-miner`). Use those values through scripts, keep them out of output and commits, and keep generated images that contain them outside this repository.
<!-- END Bright Builds fork section -->

<!-- bright-builds-rules-managed:begin -->

# Bright Builds Rules

`AGENTS.md` is the entrypoint for repo-local instructions, not the complete Bright Builds Rules specification.

This managed block is owned upstream by `bright-builds-rules`. If this block needs a fix, open an upstream PR or issue instead of editing the managed text in a downstream repo. Keep downstream-specific instructions outside this managed block.

Before plan, review, implementation, or audit work:

1. Read the repo-local instructions in `AGENTS.md`, including any `## Repo-Local Guidance` section and any instructions outside this managed block.
1. Read `AGENTS.bright-builds.md`.
1. Read `standards-overrides.md` when present.
1. Read the local managed standards pages under `standards/` relevant to the task.
1. If you have not done that yet, stop and load those sources before continuing.

Use this routing map when deciding what to load next:

- For repo-specific commands, prerequisites, generated-file ownership, CI-only suites, or recurring workflow facts, use the local `AGENTS.md`, especially `## Repo-Local Guidance`.
- For the Bright Builds default workflow and high-signal cross-cutting rules used in most tasks, use `AGENTS.bright-builds.md`.
- For deliberate repo-specific exceptions to the Bright Builds defaults, use `standards-overrides.md`.
- To choose the right managed standards page, start with the local Bright Builds entrypoint `standards/index.md`.
- For business-logic structure, domain modeling, and functional-core versus imperative-shell decisions, use the managed standards page `standards/core/architecture.md`.
- For control flow, naming, function/file size, and readability rules, use the managed standards page `standards/core/code-shape.md`.
- For frontend visual defaults, theme defaults, and dark-mode decisions, use the managed standards page `standards/core/frontend-ui.md`.
- For sync, bootstrap, and pre-commit verification rules, use the managed standards page `standards/core/verification.md`.
- For the managed starter checks, run `bun scripts/bright-builds-check.ts all`; use its `--help` output for check-specific commands, exact-file exceptions, and trailing-slash `file-lengths` directory exceptions.
- For unit-test expectations, use the managed standards page `standards/core/testing.md`.
- For Rust or TypeScript/JavaScript-specific rules, use the matching managed standards page under `standards/languages/`.
- For TypeScript/JavaScript frontend framework and UI-library defaults, use `standards/languages/typescript-javascript.md`.
- Keep recurring repo-specific workflow facts, commands, and links in a `## Repo-Local Guidance` section elsewhere in this file.
- Record deliberate repo-specific exceptions and override decisions in `standards-overrides.md`.
- If instructions elsewhere in `AGENTS.md` conflict with `AGENTS.bright-builds.md`, follow the repo-local instructions and treat them as an explicit local exception.

<!-- bright-builds-rules-managed:end -->
