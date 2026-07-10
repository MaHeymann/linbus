// Host-compiled unit tests for the LIN protocol core (LinProtocolHandler).
//
// These run on a normal PC with g++ — no ESP32, no ESP-IDF, no hardware. They compile the
// *real* esphome/components/linbus/LinProtocolHandler.cpp against small stub headers in
// tests/host/stubs/ (fake esp_log / esp_timer / freertos / driver-uart), plus a fake
// LinSerial defined below that records what the handler would transmit.
//
// Covered: PID parity, classic/enhanced/diagnostic checksums, the receive state machine,
// the C2 checksum fallback, C3 break-resync, C4 wrap-safe master scheduling, and the slave
// response TX path. See UPGRADES.md for the corresponding findings.
//
// Build & run:  tests/host/run.sh   (or see that script for the exact g++ line)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "LinProtocolHandler.h"
#include "LinSerial.h"

// ---------------------------------------------------------------------------
// Mock clock (backs the esp_timer.h stub). Tests set g_mock_time_us directly.
// ---------------------------------------------------------------------------
static int64_t g_mock_time_us = 0;
extern "C" int64_t esp_timer_get_time(void) { return g_mock_time_us; }

// ---------------------------------------------------------------------------
// Fake LinSerial: records bytes the handler transmits and counts breaks.
// Only the methods LinProtocolHandler actually calls are defined.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> g_tx;
static int g_break_count = 0;

LinSerial::LinSerial(uart_port_t uart_num, int tx_pin, int rx_pin, int baud_rate)
    : uart_num_(uart_num), tx_pin_(tx_pin), rx_pin_(rx_pin), baud_rate_(baud_rate) {}
LinSerial::~LinSerial() {}
bool LinSerial::writeBytes(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i)
    g_tx.push_back(data[i]);
  return true;
}
bool LinSerial::sendBreak() {
  g_break_count++;
  g_tx.push_back(0x00);
  return true;
}

// ---------------------------------------------------------------------------
// Tiny assertion framework
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    if (cond) {                                                \
      g_pass++;                                                \
    } else {                                                   \
      g_fail++;                                                \
      printf("  FAIL [%s:%d]: %s\n", __func__, __LINE__, #cond); \
    }                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                      \
  do {                                                                                       \
    long _a = (long) (a);                                                                    \
    long _b = (long) (b);                                                                    \
    if (_a == _b) {                                                                          \
      g_pass++;                                                                              \
    } else {                                                                                 \
      g_fail++;                                                                              \
      printf("  FAIL [%s:%d]: %s == %s (0x%lX vs 0x%lX)\n", __func__, __LINE__, #a, #b, _a, _b); \
    }                                                                                        \
  } while (0)

// Reference PID computation (independent of the code under test) for building frames.
static uint8_t ref_pid(uint8_t id) {
  uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01;
  uint8_t p1 = ~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 0x01;
  return id | (p0 << 6) | (p1 << 7);
}

// A test fixture owning the shared tables and constructing a handler in the given mode.
struct Fixture {
  lin_schedule_item_t schedule[LIN_MAX_ID_RQ_COUNT]{};
  lin_discovered_id_t discovered[LIN_MAX_DISCOVERED_PIDS]{};
  lin_respond_data_t responses[LIN_MAX_RESPONSES]{};
  LinSerial serial;
  LinProtocolHandler h;

  explicit Fixture(lin_mode_t mode)
      : serial(1, 17, 16, 19200),
        h(&serial, 19200, schedule, discovered, responses, mode, xSemaphoreCreateMutex()) {}

  void feed(const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; ++i)
      h.receiveDecode(bytes[i], 0);
  }
};

static void reset_globals() {
  g_tx.clear();
  g_break_count = 0;
  g_mock_time_us = 1'000'000;  // 1 s: keeps the bus "idle" by default
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Checksum math: classic vs enhanced, and the diagnostic-ID (0x3C/0x3D) classic rule.
static void test_checksum() {
  reset_globals();
  Fixture fx(LIN_MODE_LISTENER);
  uint8_t data[] = {0x00, 0x80, 0x01};
  // Classic = ~fold(sum(data)); sum=0x81 -> ~0x81 = 0x7E
  CHECK_EQ(fx.h.calculateChecksum(0x34, data, 3, false), 0x7E);
  // Enhanced includes PID(0x34)=0xB4; sum=0xB4+0x81=0x135 -> fold 0x36 -> ~ = 0xC9
  CHECK_EQ(fx.h.calculateChecksum(0x34, data, 3, true), 0xC9);
  // Diagnostic frames always use classic, even if enhanced is requested.
  CHECK_EQ(fx.h.calculateChecksum(0x3C, data, 3, true), fx.h.calculateChecksum(0x3C, data, 3, false));
  CHECK_EQ(fx.h.calculateChecksum(0x3D, data, 3, true), fx.h.calculateChecksum(0x3D, data, 3, false));
}

// PID parity acceptance/rejection via the receive state machine.
static void test_pid_parity() {
  reset_globals();
  {
    Fixture fx(LIN_MODE_LISTENER);
    fx.h.receiveDecode(0x00, 0);  // BREAK -> SYNC
    fx.h.receiveDecode(0x55, 0);  // SYNC  -> SID
    lin_read_state_t s = fx.h.receiveDecode(ref_pid(0x34), 0);  // valid PID -> DATA
    CHECK_EQ(s, READ_STATE_DATA);
  }
  {
    Fixture fx(LIN_MODE_LISTENER);
    fx.h.receiveDecode(0x00, 0);
    fx.h.receiveDecode(0x55, 0);
    uint32_t before = fx.h.getStatistics().pid_errors;
    lin_read_state_t s = fx.h.receiveDecode(0x34, 0);  // raw 0x34 has wrong parity (real PID 0xB4)
    CHECK_EQ(s, READ_STATE_BREAK);
    CHECK_EQ(fx.h.getStatistics().pid_errors, before + 1);
  }
}

// Full receive of an enhanced-checksum frame, finalized by the inter-byte timeout.
static void test_receive_enhanced() {
  reset_globals();
  Fixture fx(LIN_MODE_LISTENER);
  uint8_t data[] = {0x00, 0x80, 0x01};
  uint8_t cs = fx.h.calculateChecksum(0x34, data, 3, true);  // 0xC9
  uint8_t frame[] = {0x00, 0x55, ref_pid(0x34), 0x00, 0x80, 0x01, cs};
  fx.feed(frame, sizeof(frame));
  fx.h.onInterByteTimeout();  // finalize the pending frame

  uint8_t buf[16] = {0};
  CHECK_EQ(fx.h.getDataForID(0x34, buf), 3);
  CHECK_EQ(buf[0], 0x00);
  CHECK_EQ(buf[1], 0x80);
  CHECK_EQ(buf[2], 0x01);
  CHECK_EQ(fx.h.getStatistics().checksum_errors, 0);
}

// C2: a listener receives a classic-checksum frame it never transmits, via the fallback.
static void test_c2_classic_fallback() {
  reset_globals();
  Fixture fx(LIN_MODE_LISTENER);
  uint8_t data[] = {0x11, 0x22};
  uint8_t classic = fx.h.calculateChecksum(0x10, data, 2, false);  // 0xCC
  uint8_t frame[] = {0x00, 0x55, ref_pid(0x10), 0x11, 0x22, classic};
  fx.feed(frame, sizeof(frame));
  fx.h.onInterByteTimeout();

  CHECK_EQ(fx.h.getStatistics().checksum_fallback, 1);
  CHECK_EQ(fx.h.getStatistics().checksum_errors, 0);
  uint8_t buf[16] = {0};
  CHECK_EQ(fx.h.getDataForID(0x10, buf), 2);
}

// A checksum wrong under *both* modes is still counted as an error and dropped.
static void test_genuine_checksum_error() {
  reset_globals();
  Fixture fx(LIN_MODE_LISTENER);
  uint8_t frame[] = {0x00, 0x55, ref_pid(0x10), 0x01, 0x02, 0xFF};  // 0xFF matches neither mode
  fx.feed(frame, sizeof(frame));
  fx.h.onInterByteTimeout();

  CHECK_EQ(fx.h.getStatistics().checksum_errors, 1);
  CHECK_EQ(fx.h.getStatistics().checksum_fallback, 0);
  uint8_t buf[16] = {0};
  CHECK_EQ(fx.h.getDataForID(0x10, buf), 0);  // nothing stored
}

// C3: a hardware BREAK finalizes the in-flight frame and resyncs to expect the sync byte.
static void test_c3_break_resync() {
  reset_globals();
  Fixture fx(LIN_MODE_LISTENER);
  uint8_t data[] = {0x00, 0x80, 0x01};
  uint8_t cs = fx.h.calculateChecksum(0x34, data, 3, true);
  uint8_t frame[] = {0x00, 0x55, ref_pid(0x34), 0x00, 0x80, 0x01, cs};
  fx.feed(frame, sizeof(frame));  // sits in READ_STATE_DATA, not yet finalized

  fx.h.onUartBreak();  // should finalize AND resync

  uint8_t buf[16] = {0};
  CHECK_EQ(fx.h.getDataForID(0x34, buf), 3);            // captured on break
  CHECK_EQ(fx.h.receiveDecode(0x55, 0), READ_STATE_SID); // proves state == SYNC after break
}

// C3: while resyncing (state SYNC), a stray 0x00 break byte must not derail sync detection.
static void test_c3_sync_tolerates_zero() {
  reset_globals();
  Fixture fx(LIN_MODE_LISTENER);
  fx.h.onUartBreak();                                        // -> SYNC
  CHECK_EQ(fx.h.receiveDecode(0x00, 0), READ_STATE_SYNC);    // stray break byte, stay in SYNC
  CHECK_EQ(fx.h.receiveDecode(0x55, 0), READ_STATE_SID);     // real sync advances
}

// Slave mode: receiving a header for a configured response transmits it immediately (onID).
static void test_slave_responds() {
  reset_globals();
  Fixture fx(LIN_MODE_SLAVE);
  uint8_t rd[] = {0xAA, 0xBB};
  uint8_t cs = fx.h.calculateChecksum(0x34, rd, 2, true);
  fx.responses[0].id = 0x34;
  fx.responses[0].responseData[0] = 0xAA;
  fx.responses[0].responseData[1] = 0xBB;
  fx.responses[0].responseData[2] = cs;
  fx.responses[0].enhancedChecksum = true;
  fx.responses[0].dataLength = 3;

  fx.h.receiveDecode(0x00, 0);
  fx.h.receiveDecode(0x55, 0);
  fx.h.receiveDecode(ref_pid(0x34), 0);  // onID fires here

  CHECK_EQ(g_tx.size(), 3);
  CHECK_EQ(g_tx[0], 0xAA);
  CHECK_EQ(g_tx[1], 0xBB);
  CHECK_EQ(g_tx[2], cs);
  CHECK_EQ(fx.h.getStatistics().id_requests_answered, 1);
}

// C4: master scheduling fires when due, waits when not, and survives the millis() wrap.
static void test_master_schedule_and_wrap() {
  reset_globals();
  Fixture fx(LIN_MODE_MASTER);
  fx.schedule[0].id = 0x34;
  fx.schedule[0].updateInterval = 100;  // ms
  fx.schedule[0].last_send_time_ms = 0;
  fx.schedule[0].sent_once = false;

  // Never sent -> fires immediately.
  g_mock_time_us = 10'000'000;  // 10 s -> now_ms = 10000
  CHECK_EQ(fx.h.master_send(), 0x34);
  CHECK_EQ(g_break_count, 1);
  CHECK(fx.schedule[0].sent_once);

  // 50 ms later: not due.
  g_mock_time_us = 10'050'000;
  CHECK_EQ(fx.h.master_send(), 0x00);

  // 100 ms after last send: due again.
  g_mock_time_us = 10'100'000;
  CHECK_EQ(fx.h.master_send(), 0x34);

  // Wrap: last send just below 2^32 ms, "now" wrapped to a small value. Unsigned elapsed
  // = 116 ms >= 100 ms, so it must still fire (the old int32 logic went silent here).
  g_break_count = 0;
  fx.schedule[0].last_send_time_ms = 0xFFFFFFF0u;  // ~4294967280 ms
  fx.schedule[0].sent_once = true;
  g_mock_time_us = (int64_t) 100 * 1000;  // now_ms = 100 (wrapped past the stored stamp)
  CHECK_EQ(fx.h.master_send(), 0x34);
  CHECK_EQ(g_break_count, 1);

  // Non-master handler never transmits a schedule.
  Fixture listener(LIN_MODE_LISTENER);
  CHECK_EQ(listener.h.master_send(), 0x00);
}

int main() {
  printf("LIN protocol host tests\n");
  test_checksum();
  test_pid_parity();
  test_receive_enhanced();
  test_c2_classic_fallback();
  test_genuine_checksum_error();
  test_c3_break_resync();
  test_c3_sync_tolerates_zero();
  test_slave_responds();
  test_master_schedule_and_wrap();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
