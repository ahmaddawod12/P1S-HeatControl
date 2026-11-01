#pragma once
static inline void host_log(const char *tag, const char *format, ...) { (void) tag; (void) format; }
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
static inline const char *esp_err_to_name(int code) { (void) code; return "host boundary error"; }
