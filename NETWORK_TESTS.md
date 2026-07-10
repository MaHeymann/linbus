# Proposed Network / Integration Tests for the LIN Bus Component

The [host tests](tests/host/README.md) cover the protocol core in isolation. They cannot
observe real UART timing, the FreeRTOS event task, driver install, or two nodes actually
talking on a wire — exactly the areas the critical findings (C1 RX timing, C3 break framing,
C2 classic reception, C4 long-run scheduling) live in. This document proposes a layered set
of **network / on-hardware** tests to close that gap, ordered cheapest-first.

The name "network tests" is used in two senses here, both covered:

1. **Bus network** — nodes exchanging real LIN frames on a shared bus.
2. **IP network** — driving/observing those nodes over ESPHome's Wi-Fi API + OTA logger, so
   the tests can run unattended in CI against a bench rig.

All of these assume C1 and C3 are fixed first (they are, in this change) — otherwise RX is
expected to be broken and every test below would fail for the wrong reason.

---

## Test rig topologies

```
T1  Single-node loopback         T2  Two-node bus                 T3  Real device sniff
   ┌──────────┐                     ┌────────┐   ┌────────┐          ┌────────┐
   │  ESP32   │                     │ master │   │ slave  │          │listener│
   │ TX ─┬─ RX│   (+ transceiver    │  node  │   │  node  │          │  node  │
   └─────┘    │    or direct wire)  └───┬────┘   └───┬────┘          └───┬────┘
   loopback via transceiver             └─── LIN ────┘  + 1kΩ pull-up    │
   or TX↔RX jumper                          bus @12V, term.              └── tap a real
                                                                             car/appliance LIN
```

- **T1 Loopback** — one board; a LIN transceiver in loopback (or, for a pure-logic smoke
  test, TX wired to RX through a level-safe resistor). Validates the full TX→wire→RX→decode
  path on a single MCU, no second node needed.
- **T2 Two-node bus** — two boards (any mix of ESP32 / C3 / C6) on one LIN bus with proper
  12 V supply, master pull-up (~1 kΩ) and termination. The reference integration rig.
- **T3 Sniff** — one listener node tapped onto a **real** LIN device (car door module,
  HVAC panel, etc.) to validate against traffic we don't control (mixed classic/enhanced,
  bursty slaves).

---

## Layer A — On-device self-check (no second node, no PC harness)

The component already ships a built-in bus exerciser (`LINCommunication::enableSelfTest`,
master-only). Promote it to a first-class, network-observable test.

- **A1 — Loopback self-test (T1).** Master node with self-test enabled drives ~9 scheduled
  IDs and, because TX loops back into RX, should *receive its own frames*. Pass criteria
  (already logged by `selfTestEvaluate`): every ID reaches `seen_updates >= 2`, and measured
  min/max intervals stay within ±50 % of the scheduled interval. This is the single most
  valuable test — it exercises break generation, header TX, the RX event task, decode, and
  scheduling end-to-end on one board.
  - **Regression value:** would have caught C1 (chopped RX → `seen_updates` stuck at 0) and
    C4 (after the wrap, `master_requests` stops climbing).
- **A2 — Expose self-test as config.** Add an opt-in `self_test: true` (or a
  `linbus.run_self_test` action) so a CI fixture can enable it without a code edit, and gate
  the per-header `ESP_LOGD` behind it (H9 hot-path note).

---

## Layer B — Network-driven assertions over the ESPHome API

Drive and observe the bench rig from a PC over Wi-Fi using
[`aioesphomeapi`](https://github.com/esphome/aioesphomeapi) + `pytest`. The diagnostic
sensors are now trustworthy for this (H4: counters are monotonic lifetime totals; H2/H3:
`on_frame` fires correctly), so tests can assert on sensor state instead of scraping logs.

Suggested `pytest` cases (rig = T2 unless noted):

- **B1 — RX integrity.** Master schedules ID X @ 20 ms; slave publishes a known payload for
  X. Subscribe to the master's `on_frame` for X (or a template sensor fed by it). Assert the
  decoded payload equals the slave's payload, and that `checksum_errors_total` stays 0 over
  60 s. *Directly guards C1/C3.*
- **B2 — Classic checksum reception (C2).** Slave publishes a **classic**-checksum frame the
  master never transmits. Assert the master decodes it and `checksum_fallback` increments
  while `checksum_errors_total` stays flat. Repeat with a listener node on T3. *Guards C2.*
- **B3 — No bus collisions / ghost headers (C3).** Run B1 for 5 min with a second, faster
  slave injecting back-to-back frames. Assert `frame_errors`/`checksum_errors_total` remain
  bounded and the master never transmits on an ID it doesn't own (watch the slave's
  `id_requests_answered_total` for unexpected jumps). *Guards C3 ghost-header collisions.*
- **B4 — `update_response` round-trip.** From the PC, call the `linbus.update_response`
  action (via an API service or a button entity) to change a slave's payload; assert the
  master's next `on_frame` reflects the new bytes, and that the configured checksum mode is
  preserved (enhanced stays enhanced, classic stays classic).
- **B5 — Config-failure path (H5).** Flash a fixture with `uart_port: UART_NUM_0` while the
  logger owns UART0. Assert the node boots, marks the component **failed** (visible in
  `dump_config` / API), and does **not** bootloop. *Guards H5.*
- **B6 — Sensor semantics (H4).** Over 3 stat windows assert every `*_total` sensor is
  monotonic non-decreasing (no sawtooth), and that `bus_load` / `error_rate` behave as
  instantaneous MEASUREMENT values.

---

## Layer C — Timing & soak (bench instrumentation)

- **C1t — Scope/logic-analyzer capture (T1/T2).** Capture the header on a logic analyzer;
  assert break ≥ 13 Tbit, sync = 0x55, and inter-byte gaps within the LIN 2.1 frame-slot
  budget (≤ 1.4× nominal, §2.2.3). Complements the M1 `slot_overruns` idea.
- **C2t — Schedule jitter.** With A1 running, log the per-ID min/max interval for 10 min and
  assert jitter stays within the ESPHome-loop bound documented for M4. Flags if scheduling
  needs to move to an `esp_timer` task.
- **C3t — Long-run soak.** 48–72 h run of B1 with the counter sensors graphed in Home
  Assistant; assert no silent stall and no counter resets. A short-circuited version:
  temporarily seed the millis base near 2³¹ ms to force the C4 wrap within minutes and assert
  scheduling continues.

---

## Automating it in CI

- **Firmware build gate (already possible today):** `esphome compile` for each fixture in
  `tests/components/linbus/` on every push. Add negative fixtures (`uart_port: UART_NUM_0`,
  `baud_rate: 0`, `interval: 0ms`) asserted to **fail** `esphome config` — mirrors the
  existing `*_invalid.yaml` pattern and locks in the H6 validators.
- **Hardware-in-the-loop (HIL) runner:** a self-hosted runner wired as T2 with a USB-serial
  reset line. CI flashes both nodes over USB, then runs the Layer B `pytest` suite over the
  LAN API. Gate merges on B1/B2/B5.
- **Artifacts:** on failure, pull the last N seconds of each node's log over the API and the
  final `dump_config`, so a red CI run is diagnosable without physical access.

---

## Suggested first three to build

| Order | Test | Why first | Rig |
|-------|------|-----------|-----|
| 1 | A1 loopback self-test | One board; catches C1/C3/C4 regressions immediately | T1 |
| 2 | B1 RX integrity + B2 classic (C2) | The core promises of the driver, unattended | T2 |
| 3 | B5 config-failure (H5) | Cheap, proves no-bootloop guarantee | any |
