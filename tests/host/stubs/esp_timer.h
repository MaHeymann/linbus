// Host-test stub for esp_timer.h — the test provides a settable mock clock.
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
