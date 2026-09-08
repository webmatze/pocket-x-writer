#pragma once

// Consumer-supplied logging facility.
//
// The FreeInk SDK's FrontlightManager does `#include <Logging.h>` and calls
// LOG_INF(tag, fmt, ...), but the SDK does not ship that header — it expects the
// consuming firmware to provide it (CrossPoint has its own). Without it,
// FrontlightManager fails to compile. This is that header.
//
// Deliberately routed to Serial, not esp_log: the prebuilt Arduino sdkconfig
// sets CONFIG_LOG_DEFAULT_LEVEL_ERROR, so ESP_LOGI is compiled out entirely, and
// esp_log writes to the IDF UART console rather than the USB CDC stream that
// `pio device monitor` reads.

#include <Arduino.h>

#ifndef POCKETX_LOG_ENABLED
#define POCKETX_LOG_ENABLED 1
#endif

#if POCKETX_LOG_ENABLED
#define LOG_INF(tag, fmt, ...) \
  do { Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#else
#define LOG_INF(tag, fmt, ...) do { } while (0)
#endif
