// Host-test stub for driver/uart.h — only the symbols LinProtocolHandler / LinSerial.h touch.
#pragma once
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

typedef int uart_port_t;

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// LinProtocolHandler::isBusIdle() calls this; report "TX done" so the idle check passes.
static inline esp_err_t uart_wait_tx_done(uart_port_t, TickType_t) { return ESP_OK; }
