// Host-test stub for freertos/FreeRTOS.h — just enough to compile LinProtocolHandler on a PC.
#pragma once
#include <cstdint>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;

#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif
#ifndef pdPASS
#define pdPASS 1
#endif
#ifndef portMAX_DELAY
#define portMAX_DELAY ((TickType_t) 0xffffffffUL)
#endif
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(x) ((TickType_t) (x))
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
