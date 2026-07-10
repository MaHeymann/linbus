# LIN Bus Component — Review Findings & Upgrade Suggestions

Static review of the v2 component (commit `4bcda05`), focused on robustness and LIN 2.1
spec adherence. Findings are from code inspection against the LIN 2.1 specification and
ESP-IDF driver semantics.

## Status — fixes applied 2026-07-10

A first pass of fixes has been applied and validated **without hardware**:

- **Validation:** all `esphome config` fixtures pass; a full `esphome compile` succeeds for
  the ESP32, ESP32-C3 and ESP32-C6 fixtures; and a new host-compiled unit test
  (`tests/host/`, run `tests/host/run.sh`) exercises the protocol core (checksums, PID
  parity, decode state machine, C2 fallback, C3 break-resync, C4 wrap) — **35/35 pass**.
  A real LIN-bus smoke test is still pending; see `NETWORK_TESTS.md` for the proposed
  hardware/network test plan.

| Finding | Status | Notes |
|---|---|---|
| C1 inter-byte timeout | ✅ fixed | timeout computed before the task starts; tick floor = 2 |
| C2 RX checksum mode | ✅ fixed (pragmatic) | auto-fallback to the other mode + `checksum_fallback` counter. A per-ID config override is still a nice-to-have |
| C3 break detection | ✅ fixed | `UART_BREAK`→resync, `UART_FRAME_ERR`/`PARITY`/overflow→reset+count; SYNC tolerates stray 0x00 |
| C4 scheduler 24.8-day wrap | ✅ fixed | unsigned wrap-safe millis + `sent_once` sentinel |
| H2 trigger after eviction | ✅ fixed | fires on `updateCount != last` |
| H4 sawtooth counters | ✅ fixed | lifetime `*_total` fed to TOTAL_INCREASING sensors |
| H5 bootloop on bad config | ✅ fixed | `LinSerial::init()` returns `esp_err_t` → `mark_failed()` |
| H6 config validation | ✅ fixed | baud `1000..20000`, non-zero interval, `uart_port` enum |
| H8 unsafe teardown | ✅ fixed | event task stopped before deletes; member initializers |
| H9 task priority | ✅ fixed | high-but-bounded priority (was `configMAX_PRIORITIES-1`) |
| M2 dead `send_failures` | ✅ fixed | counts real break/write TX failures |
| M3 blocking `sendBreak` | ✅ fixed | bounded 20 ms wait, returns success |
| M6 shared statics | ✅ fixed | promoted to per-instance members |
| M8 dead code | ✅ mostly | dead fns/fields/defines/phantom class removed |
| H1 length-aware framing | ⬜ deferred | needs `data_length:` config; see below |
| H3 `on_frame` coalescing | ⬜ deferred | behaviour is reasonable; add a counter/docs |
| H7 torn 64-bit read (RV32) | ⬜ deferred | make `last_bus_activity_us_` atomic/32-bit |
| M1, M4, M5, M7, M9 | ⬜ deferred | spec-quality polish |

The original findings follow unchanged for reference.

Since the hardware smoke test is still pending (see `NEXT_SESSION_PLAN.md`), the Critical
items below are exactly the ones likely to surface in that session — fix C1 before wiring
anything up.

## What is already correct (verified against LIN 2.1)

- **PID parity** (`LinProtocolHandler.cpp:345`): P0 = ID0⊕ID1⊕ID2⊕ID4, P1 = ¬(ID1⊕ID3⊕ID4⊕ID5)
  — matches spec §2.3.1.3 exactly.
- **Checksum** (`LinProtocolHandler.cpp:193`): inverted eight-bit sum with carry; enhanced
  includes the PID, classic does not; IDs 0x3C/0x3D forced to classic — matches §2.3.1.5,
  including the diagnostic-frame rule.
- **Break generation** (`LinSerial.cpp:47`): half-baud 0x00 yields ≈18 nominal bit times
  dominant + 2 bit delimiter — compliant (spec requires ≥13 + ≥1), though longer than needed.
- Mode/role validation in Python (schedule only in master, responses rejected in listener),
  checksum-mode preservation in `update_response`, mutex discipline on shared tables, and
  stripping the checksum byte before `on_frame` all hold up.

---

## Critical — will break or corrupt communication on real hardware

### C1. Inter-byte timeout latches 0 → frames chopped mid-response

`LINCommunication::init()` creates the UART event task (`LINCommunication.cpp:75`) **before**
assigning `inter_byte_timeout_us_` (`LINCommunication.cpp:83`). The task runs at
`configMAX_PRIORITIES - 1`, so on single-core C3/C6 it preempts `init()` immediately and
computes `wait_ticks` (`LINCommunication.cpp:102`) from the still-zero timeout →
`pdMS_TO_TICKS(0)` → forced to **1 tick**.

ESPHome builds with `CONFIG_FREERTOS_HZ=1000` (confirmed in the generated sdkconfig), so
1 tick = 1 ms — and a 1-tick `xQueueReceive` expires at the *next tick interrupt*, which can
be arbitrarily soon after the call. At 19200 baud a byte takes 521 µs, so roughly half of all
inter-byte gaps contain a tick boundary. Each hit fires `onInterByteTimeout()`
(`LinProtocolHandler.cpp:237`), which finalizes the partial frame (checksum error) **and
resets the decoder to `READ_STATE_BREAK`**, discarding the rest of the response. Expect the
receive path to be mostly broken on hardware.

**Fix:**
1. Assign `inter_byte_timeout_us_` *before* creating the task (or pass it as part of the task
   argument).
2. Enforce a floor: `wait_ticks = max(pdMS_TO_TICKS(timeout_ms) + 1, 2)` — a timeout of N
   ticks guarantees only N-1 full tick periods.
3. Longer term: stop using queue-wait silence as the framing signal entirely (see H1/M1).

### C2. RX checksum mode is inferred from the node's own TX table — classic frames from other nodes cannot be received

`onData()` (`LinProtocolHandler.cpp:83-91`) decides enhanced-vs-classic by looking up the
received ID in `responses_` — the table of frames *this node transmits*. Consequences:

- A **listener** can never receive classic-checksum frames: `responses` is (correctly)
  rejected in listener mode, so `enhanced_checksum` is always `true` and every LIN 1.x frame
  is dropped as a checksum error. Sniffing a LIN 1.x bus reports 100 % errors.
- A **master/slave** receiving a classic frame published by another node has no safe
  workaround: adding a `responses` entry for that ID to select classic would make this node
  *transmit* on that ID → guaranteed bus collision (the exact failure the README warns about).

Per spec, checksum type is a static property of each frame (defined in the LDF), independent
of who publishes it.

**Fix:** add a per-received-ID checksum config — e.g. `checksum:` on `on_frame`/`schedule`
entries, or a top-level `rx_checksum_overrides: [{lin_id, checksum}]` — kept in a small
lookup separate from `responses_`. A pragmatic complement: on checksum mismatch, try the
other mode and count `checksum_fallback` separately, so a sniffer still sees traffic.

### C3. Break detection is "any 0x00 byte"; the UART's real break/framing events are discarded

`receiveDecode()` `READ_STATE_BREAK` (`LinProtocolHandler.cpp:258`) accepts any 0x00 byte as
a break. The event task explicitly ignores `UART_BREAK` (`LINCommunication.cpp:126-129`,
comment: "not implemented") and `UART_FRAME_ERR` falls into `default:`. Failure modes:

- After any desync (FIFO overflow flush, missed byte, C1 firing), payload bytes are parsed
  in header states. A payload sequence `0x00, 0x55, <valid-parity byte>` forms a **ghost
  header**, and `onID()` then *transmits a response mid-frame* → bus collision from a node
  that is supposed to be well-behaved.
- The design silently depends on the IDF driver placing the break's framing-error 0x00 into
  the RX ring buffer; that is driver behavior, not a contract.

**Fix:** handle `UART_BREAK` as the authoritative resync: force the decoder to
`READ_STATE_SYNC` (finalizing any pending frame first) and treat 0x00 in `READ_STATE_BREAK`
only as a *fallback*. Handle `UART_FRAME_ERR` by resetting the decoder and counting it. This
single change removes the ghost-header collision risk and makes end-of-frame deterministic
(a break always terminates the previous frame slot) — which also de-risks C1/H1.

### C4. Master scheduler freezes after ~24.8 days of uptime

`master_send()` truncates `esp_timer_get_time()/1000` into `int32_t`
(`LinProtocolHandler.cpp:155`); `lin_schedule_item_t::last_send_time_ms` is also `int32_t`.
At ~2^31 ms (≈24.8 days) `current_time_ms` wraps negative, `elapsed_ms` goes hugely negative,
and no schedule entry is ever due again — the bus goes silent on an always-on ESPHome node.
The `last_send_time_ms == 0` "never sent" sentinel also aliases a legitimate timestamp.

**Fix:** use `uint32_t` millis with unsigned wraparound subtraction
(`(uint32_t)(now - last) >= interval`, the ESPHome `millis()` idiom), or keep `int64_t`
microseconds end-to-end. Replace the `0` sentinel with a `bool sent_once` or initialize to
`now - interval`.

---

## High — robustness and data-integrity defects

### H1. Frame boundaries exist only as "silence"; no per-ID frame length

Reception has no notion of expected response length. End-of-frame is a tick-quantized queue
timeout (C1) or buffer overflow. Even fixed, this means: `on_frame` data is delayed ≥1–2 ms
after the last byte; back-to-back traffic (a second master, fast schedules, or bursty slaves)
merges frames; and a slave pausing >1–2 ms mid-response (spec budget allows up to 1.4× the
nominal frame slot, §2.2.3) gets its frame split. LIN frame lengths are static per ID (LDF),
so the master/listener *can* know them.

**Fix:** optional `data_length:` on `schedule`/`on_frame` entries; when known, finalize
deterministically after `data_length + 1` bytes. Combined with break-resync (C3), silence
detection becomes a last resort instead of the primary mechanism.

### H2. `on_frame` triggers die permanently after discovered-table eviction

`onData()` LRU-overwrites a slot and resets `updateCount = 1` (`LinProtocolHandler.cpp:143`),
but `linbus.cpp:132` fires triggers only when `updateCount > last_count`, and
`last_update_counts_` keeps the old high-water mark. With more than `LIN_MAX_DISCOVERED_PIDS`
(32) active IDs on the bus, any evicted-and-rediscovered ID's trigger goes silent until the
count re-passes the stale value — potentially forever.

**Fix:** fire when `updateCount != last_count`, or store `lastUpdateTime` as the change token
instead of a resettable counter. Consider bumping the table size or making eviction log at WARN.

### H3. `on_frame` silently coalesces frames (and the README says otherwise)

The trigger path polls once per ESPHome `loop()` (`linbus.cpp:122-143`) and only sees the
*latest* frame per ID. At a 10–50 ms schedule against a ~16 ms (and jittery) loop, intermediate
frames are dropped with no counter. The README's "high-frequency frames" section claims
`on_frame` runs thousands of times a minute — it cannot; it is loop-rate-bound per ID.

**Fix:** either document the coalescing honestly (it is a reasonable design for ESPHome), or
push frames through a small ring buffer drained in `loop()` so every frame fires once. Add a
`frames_dropped`/`frames_coalesced` counter either way.

### H4. Counter sensors publish a 10-second sawtooth as `TOTAL_INCREASING`

`updateStatistics()` zeroes all counters every 10 s (`LinProtocolHandler.cpp:406-413`), while
`linbus.cpp:94-103` publishes the *raw in-window* counters on its own unsynchronized 10 s
timer, and `sensor/__init__.py` declares them `STATE_CLASS_TOTAL_INCREASING`. Home Assistant
interprets every decrease as a meter reset → long-term statistics are garbage.

**Fix:** keep lifetime totals (never reset) for the `TOTAL_INCREASING` sensors and separate
window counters for the rate/percent values, or reclassify everything as `MEASUREMENT` rates.

### H5. Config errors abort into a bootloop instead of `mark_failed()`

`LinSerial`'s constructor uses `ESP_ERROR_CHECK` (`LinSerial.cpp:26-33`), which calls
`abort()` on failure. Example trigger: `uart_port: UART_NUM_0` while the ESPHome logger owns
UART0 → `uart_driver_install` fails → panic → bootloop, with no YAML validation to prevent it
(`uart_port` is a free-form `cv.string` with a runtime fallback-warning,
`__init__.py:77`, `linbus.cpp:27-29`).

**Fix:** return `esp_err_t` from an `init()` on `LinSerial` and propagate to
`LinBusComponent::setup()` → `mark_failed()`. In Python, use
`cv.one_of("UART_NUM_0", "UART_NUM_1", "UART_NUM_2")` and reject UART0 when the logger uses it.

### H6. `baud_rate` and `interval` accept values that break the driver

- `baud_rate: 0` passes `cv.int_` (`__init__.py:76`) → division by zero in
  `LinProtocolHandler` (`LinProtocolHandler.cpp:26`) and `init()` (`LINCommunication.cpp:83`)
  → crash. LIN 2.1 also caps the bus at 20 kbit/s.
- `interval: 0ms` passes `positive_time_period_milliseconds` (which allows zero) but
  `updateInterval == 0` is the *free-slot sentinel* (`LINCommunication.cpp:168`,
  `LinProtocolHandler.cpp:157`) → the entry is silently never scheduled, and
  `master_add_id_schedule(id, 0)` acts as an accidental remove.

**Fix:** `cv.int_range(min=1000, max=20000)` for baud (or `cv.one_of(9600, 10400, 19200,
20000)`), `positive_not_null_time_period` for intervals, and stop using `0` as a sentinel
(add an `in_use` flag).

### H7. Torn 64-bit timestamp reads on RISC-V can start a header mid-frame

`last_bus_activity_us_` (`int64_t`) is written in `receiveDecode()` without the mutex
(`LinProtocolHandler.cpp:254`) and read in `isBusIdle()` (`LinProtocolHandler.cpp:333`) from
a different task. On RV32 (C3/C6) a 64-bit store is two instructions — a torn read can make
the bus look idle while a byte is mid-flight, letting `master_send()` transmit a break into
another node's response. Same pattern applies to `lastUpdateTime` reads outside the lock.

**Fix:** store as `uint32_t` microseconds (wrap-safe subtraction covers LIN timescales), use
`std::atomic<int64_t>`, or read it under `data_mutex_` (it is already held in `master_send`;
the *writer* is the unprotected side).

### H8. Teardown is unsafe (latent, but cheap to fix)

- `lin_protocol_handler_` is never initialized in the constructor
  (`LINCommunication.cpp:22-32`); destroying an un-`init()`-ed instance deletes a wild pointer.
- The destructor deletes `lin_serial_`/`lin_protocol_handler_` while the event task
  (`uart_event_task_handle_`) is still running against them → use-after-free; the task is
  never stopped.

ESPHome rarely destroys components, but this bites anyone reusing the driver classes.
**Fix:** member initializers (`{nullptr}`), and in the destructor: signal + `vTaskDelete`
the event task first, then delete the handler, then the serial.

### H9. UART event task runs at `configMAX_PRIORITIES - 1`

`LINCommunication.cpp:75` pins the RX task at the highest FreeRTOS priority — above
`esp_timer`, the Wi-Fi/BT stacks, and everything ESPHome depends on. A busy or misbehaving
bus (or the ESP_LOGD per received header in `receiveDecode`) can starve the system.
LIN at ≤20 kbit/s needs low latency, not maximum priority. **Fix:** priority ~12–18 is ample;
also drop the per-header `ESP_LOGD` from the hot path (`LinProtocolHandler.cpp:283`) or gate
it behind a verbose flag.

---

## Medium — spec-quality gaps and misleading surfaces

- **M1. Frame-slot timing budget not enforced or observed.** No measurement of response
  space / frame slot vs the spec's 1.4× nominal budget (§2.2.3). With H1's `data_length`
  known, a per-frame `slot_overruns` counter would make marginal slaves visible — valuable
  given the diagnostic-sensor investment already made.
- **M2. `send_failures` is effectively dead.** It only increments if `serial_ == nullptr`
  (`LinProtocolHandler.cpp:178-180`), which cannot happen after init. The README presents it
  as a meaningful health sensor. Count real failures: `writeBytes` short writes,
  `uart_wait_tx_done` timeouts, bus-not-idle skips.
- **M3. `sendBreak` blocks with `portMAX_DELAY`** (`LinSerial.cpp:55`) inside the ESPHome
  loop; a wedged TX (transceiver held dominant) hangs the main loop → WDT reset. Use a
  bounded timeout and count failures (feeds M2). Also consider the IDF hardware break
  (`uart_write_bytes_with_break`) instead of the half-baud trick — `sendHeader()` in
  `LinSerial.h:24` promises exactly that but was never implemented.
- **M4. `master_send` sends at most one due frame per `loop()`** and schedule fidelity is
  bound to ESPHome's jittery ~16 ms+ loop. Fine for relaxed schedules; document it, or move
  scheduling to an `esp_timer`/dedicated task for spec-like table timing.
- **M5. Statistics window inconsistencies.** `unique_ids_seen`/`unique_id_tracker_` are never
  reset per window despite the struct's "10-second rolling window" comment; `total_bus_bytes`
  counts the ~20-bit break as one byte, skewing `bus_load`. The 256-byte tracker only needs
  8 bytes for 64 IDs.
- **M6. Multi-instance hazards.** Function-local statics `last_stats_log` (`linbus.cpp:87`)
  and `last_eval_us` (`LINCommunication.cpp:428`) are shared across instances; two `linbus:`
  buses would interfere. Make them members.
- **M7. Misleading names/comments.** `get_response_4id()` reads *received* frames
  (discovered table), not responses. `receiveDecode` claims "called by ISR" and is
  `IRAM_ATTR`, but runs in task context and calls ESP_LOG (flash) — drop the attribute or
  the claim. The `LINCommunication.h` doc block still says scheduling is "not yet implemented".
- **M8. Dead code to remove:** `lin_serial_rx_cb` (`LINCommunication.cpp:14`), `onTimeout()`
  and `dumpReceivedPIDsToLog()` declarations on `LinProtocolHandler` (declared, never
  defined — latent linker traps), `ReceivedEntry`, the `timeout_LinRecieive_` dead stores,
  `LIN_DEBUG_LOGGING`, unused macros (`LIN_BREAK_DURATION`, `DIAGNOSTIC_FRAME_MASTER/SLAVE`
  — ironically the checksum code hardcodes 0x3C/0x3D instead of using them), the phantom
  `LinBusSensor` class in `sensor/__init__.py:16`, and the committed `__pycache__` directories
  (untracked, but worth a root `.gitignore` entry).
- **M9. Python validation niceties.** Duplicate `lin_id` within `schedule`/`responses` is
  silently last-wins; `AUTO_LOAD = ["sensor"]` pulls the sensor core into every build even
  without diagnostics (make the sensor platform opt-in only).

---

## Suggested order of attack

| # | Item | Effort | Why first |
|---|------|--------|-----------|
| 1 | C1 ordering + tick floor | tiny | Without it the hardware session will show ~100 % RX loss |
| 2 | C3 UART_BREAK resync | small | Removes collision risk *and* gives deterministic framing |
| 3 | C4 timer wrap + H6 validation | small | Silent long-uptime and config footguns |
| 4 | C2 per-RX-ID checksum config | medium | Unblocks LIN 1.x buses and listener sniffing |
| 5 | H2/H3 trigger correctness | small | Data loss invisible to users |
| 6 | H4 sensor semantics, M2 real send_failures | small | Diagnostics users will act on |
| 7 | H5/H7/H8/H9 hardening | medium | Bootloop, torn reads, teardown, priority |
| 8 | H1/M1 length-aware framing + slot timing | medium | The real spec-quality upgrade |
| 9 | M7/M8 rename + dead-code sweep | small | Keeps the codebase honest for contributors |

A hardware smoke test (per `NEXT_SESSION_PLAN.md`) after step 2 is the right checkpoint: with
C1+C3 fixed, a scope or a second node should show clean header/response cycles, and the
diagnostic sensors become trustworthy evidence rather than noise.
