# LINBus Hardware Test Plan

This plan is for a separate hardware session. The current code change is compile-validated only; do not treat it as a completed LIN transceiver smoke test.

## Update 2026-07-10 — robustness fixes applied

A round of fixes from `UPGRADES.md` has landed (C1–C4, H2/H4/H5/H6/H8/H9, M2/M3/M6/M8).
See the status table at the top of `UPGRADES.md`. Validation so far, still **no hardware**:

- Host unit test of the protocol core: `tests/host/run.sh` → 35/35 pass.
- `esphome config` passes for all fixtures; the invalid fixtures still fail as expected.
- `esphome compile` succeeds for the ESP32, ESP32-C3 and ESP32-C6 fixtures.
- New negative expectations to lock in the H6 validators: `baud_rate: 0`, `interval: 0ms`
  and an unknown `uart_port` are now rejected by `esphome config`.

The hardware session below is still required. `NETWORK_TESTS.md` proposes the full
on-hardware / network test plan; the loopback self-test (A1) is the recommended first check.

## Current State

- ESPHome compile fixtures now load the local `linbus` component.
- ESP32-C6 compile coverage was added with `esp32-c6-devkitm-1`, ESP-IDF, `GPIO4` TX, `GPIO5` RX, `GPIO6` CS, and `UART_NUM_1`.
- Classic-checksum coverage and invalid mode-combination fixtures were added.
- C6/ESP-IDF compatibility work was implemented and verified without uploading firmware.
- Mode behavior now keeps listener mode receive-only and validates master-only schedules.
- Response checksum mode is stored per response and preserved by `linbus.update_response` for existing IDs.
- Shared response, discovery, and statistics state is protected by a mutex.

## Verification Already Run

- `esphome config tests/components/linbus/test_esp32c3.yaml`
- `esphome compile tests/components/linbus/test_esp32c3.yaml`
- `esphome compile tests/components/linbus/test_esp32c6.yaml`
- `esphome compile tests/components/linbus/test_master.yaml`
- `esphome compile tests/components/linbus/test_slave.yaml`
- `esphome compile tests/components/linbus/test_sensors.yaml`
- `esphome compile tests/components/linbus/test_actions.yaml`
- `esphome compile tests/components/linbus/test_classic_checksum.yaml`
- `esphome config tests/components/linbus/test_listener_with_responses_invalid.yaml` failed as expected.
- `esphome config tests/components/linbus/test_slave_with_schedule_invalid.yaml` failed as expected.
- Generated C++ for `test_classic_checksum.yaml` contains `lin_slave->add_response(52, {0x00, 0x80, 0x01}, false);`.
- C6 chip detection succeeded on `/dev/cu.usbmodem1101`: ESP32-C6 QFN40 revision v0.2, MAC `98:a3:16:ff:fe:96:e2:f0`.

## Hardware Session Tasks

1. Confirm the attached ESP32-C6 with a read-only chip probe.
2. Confirm actual LIN transceiver wiring: power, ground, TX, RX, CS or enable pin, LIN bus line, and termination.
3. Pick hardware-safe pins for the real wiring. The current C6 fixture uses compile-only placeholder pins and ESPHome warns that GPIO4/GPIO5 are strapping pins.
4. Build a minimal C6 master or listener firmware for those confirmed pins.
5. Upload only after wiring is confirmed.
6. Run a smoke test against a known LIN device or bench node and capture logs.
7. Verify master scheduling, slave response transmission, listener receive-only behavior, enhanced checksum, and classic checksum with a device/frame that needs it.

## Guardrails

- Do not upload firmware as part of compile-only verification.
- Keep public YAML names unchanged.
- Treat `responses` in listener mode and `schedule` outside master mode as validation errors.
- Preserve checksum behavior for `linbus.update_response`: existing response IDs keep their configured checksum mode; new action-created response IDs default to enhanced.
