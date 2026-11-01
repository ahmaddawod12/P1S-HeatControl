#include "diagnostics.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static diagnostic_record_t s_records[DIAGNOSTICS_CAPACITY];
static size_t s_next;
static size_t s_count;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;

/* Created once, from static storage, so recording never allocates. */
static SemaphoreHandle_t lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    return s_lock;
}

void diagnostics_record(uint64_t timestamp_ms,
                        heat_state_t from_state,
                        heat_state_t to_state,
                        heat_fault_t fault)
{
    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return;
    }

    xSemaphoreTake(handle, portMAX_DELAY);
    s_records[s_next] = (diagnostic_record_t) {
        .timestamp_ms = timestamp_ms,
        .from_state = from_state,
        .to_state = to_state,
        .fault = fault,
    };
    s_next = (s_next + 1U) % DIAGNOSTICS_CAPACITY;
    if (s_count < DIAGNOSTICS_CAPACITY) {
        ++s_count;
    }
    xSemaphoreGive(handle);
}

void diagnostics_snapshot(diagnostic_record_t *out_records,
                          size_t capacity,
                          size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (out_records == NULL || capacity == 0U || out_count == NULL) {
        return;
    }

    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return;
    }

    xSemaphoreTake(handle, portMAX_DELAY);
    const size_t wanted = s_count < capacity ? s_count : capacity;
    /* Walk back from the newest so a short array keeps recent history. */
    size_t index = (s_next + DIAGNOSTICS_CAPACITY - wanted) % DIAGNOSTICS_CAPACITY;
    for (size_t i = 0U; i < wanted; ++i) {
        out_records[i] = s_records[index];
        index = (index + 1U) % DIAGNOSTICS_CAPACITY;
    }
    *out_count = wanted;
    xSemaphoreGive(handle);
}

void diagnostics_reset(void)
{
    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return;
    }
    xSemaphoreTake(handle, portMAX_DELAY);
    memset(s_records, 0, sizeof(s_records));
    s_next = 0U;
    s_count = 0U;
    xSemaphoreGive(handle);
}
