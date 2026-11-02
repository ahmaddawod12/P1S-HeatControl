#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t,TickType_t);
BaseType_t xTaskNotifyGive(TaskHandle_t);
