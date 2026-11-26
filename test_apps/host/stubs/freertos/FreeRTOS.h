#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#ifdef HOST_FREERTOS_PTHREAD_CRITICAL
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define taskEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
#endif
/* Run virtual firmware milliseconds four times faster than wall time. */
#define pdMS_TO_TICKS(ms) ((TickType_t) (((ms) + 3U) / 4U))
