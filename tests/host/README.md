# Host (local) unit tests

Fast, hardware-free tests for the LIN protocol core. They compile the **real**
`esphome/components/linbus/LinProtocolHandler.cpp` on your PC with `g++`, against small
stub ESP-IDF headers in [`stubs/`](stubs) (fake `esp_log` / `esp_timer` / `freertos` /
`driver/uart`) plus a fake `LinSerial` that records what the handler would transmit.

No ESP32, no ESP-IDF toolchain, no flashing.

## Run

```bash
tests/host/run.sh
```

Expected tail:

```
35 passed, 0 failed
```

Exit code is non-zero if any check fails, so it drops straight into CI.

## What is covered

| Area | Test | Related finding |
|------|------|-----------------|
| PID parity accept/reject | `test_pid_parity` | — |
| Classic / enhanced / diagnostic checksum | `test_checksum` | — |
| End-to-end receive + inter-byte finalize | `test_receive_enhanced` | H1 |
| Classic-frame reception via fallback | `test_c2_classic_fallback` | **C2** |
| Genuine (both-mode) checksum error counted | `test_genuine_checksum_error` | C2 |
| Hardware-break resync + frame finalize | `test_c3_break_resync` | **C3** |
| Sync tolerates stray break byte | `test_c3_sync_tolerates_zero` | C3 |
| Slave transmits configured response on its ID | `test_slave_responds` | — |
| Master schedule due/not-due + millis wrap | `test_master_schedule_and_wrap` | **C4** |

## What it does NOT cover

The host tests exercise pure protocol logic. They deliberately do **not** cover the
ESP-IDF glue (`LINCommunication`, `LinSerial`, the UART event task, driver install,
FreeRTOS task priority/teardown) or real bus timing — those need the hardware / network
tests described in [`../../NETWORK_TESTS.md`](../../NETWORK_TESTS.md).

## Adding a test

Add a `test_*()` function in `test_lin_protocol.cpp`, call it from `main()`, and use
`CHECK(...)` / `CHECK_EQ(a, b)`. Advance the mock clock with `g_mock_time_us`; inspect
transmitted bytes via `g_tx` and break count via `g_break_count`.
