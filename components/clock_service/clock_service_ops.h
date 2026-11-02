#pragma once
#include "clock_service.h"
typedef struct {
  uint64_t (*monotonic_ms)(void);
  bool (*valid)(void);
  esp_err_t (*start)(const char *host);
  void (*wait)(uint32_t ms);
  void (*stop)(void);
  void (*sleep)(uint32_t ms);
} clock_service_ops_t;
esp_err_t clock_service_wait_with_ops(const clock_service_ops_t *, const char *,
                                      uint32_t, bool (*)(void *), void *);
