// Host-test stub for freertos/semphr.h — single-threaded, so the "mutex" is a no-op.
#pragma once
#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  static int dummy;
  return (SemaphoreHandle_t) &dummy;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t) {}
