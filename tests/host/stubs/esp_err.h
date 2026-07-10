// Host-test stub for esp_err.h
#pragma once

typedef int esp_err_t;

#ifndef ESP_OK
#define ESP_OK 0
#endif
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102

static inline const char *esp_err_to_name(esp_err_t) { return "ESP_ERR"; }
