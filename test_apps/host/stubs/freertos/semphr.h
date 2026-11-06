#pragma once
#include "FreeRTOS.h"
typedef struct host_semaphore *SemaphoreHandle_t;
typedef struct { SemaphoreHandle_t handle; } StaticSemaphore_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
