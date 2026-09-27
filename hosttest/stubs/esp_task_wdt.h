#pragma once
#include <cstdint>
#define ESP_OK 0
#define ESP_ARDUINO_VERSION_MAJOR 3
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) { return ESP_OK; }
inline int esp_task_wdt_init(const esp_task_wdt_config_t*) { return ESP_OK; }
inline int esp_task_wdt_init(uint32_t, bool) { return ESP_OK; }
inline int esp_task_wdt_add(void*) { return ESP_OK; }
inline int esp_task_wdt_reset() { return ESP_OK; }
