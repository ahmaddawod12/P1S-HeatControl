#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

struct host_semaphore { pthread_mutex_t lock; pthread_cond_t changed; unsigned count; };
struct host_queue { pthread_mutex_t lock; pthread_cond_t changed; unsigned capacity, size, count; unsigned char *data; };
static struct timespec deadline(TickType_t ticks)
{
    struct timespec t; timespec_get(&t, TIME_UTC);
    t.tv_sec += ticks / 1000;
    t.tv_nsec += (long) (ticks % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { ++t.tv_sec; t.tv_nsec -= 1000000000L; }
    return t;
}
static int wait(pthread_cond_t *cond, pthread_mutex_t *lock, TickType_t ticks, const struct timespec *until)
{ return ticks == portMAX_DELAY ? pthread_cond_wait(cond, lock) : pthread_cond_timedwait(cond, lock, until); }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    if (s) { pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->changed, NULL); }
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ SemaphoreHandle_t s = xSemaphoreCreateBinary(); if (s) s->count = 1; return s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    const struct timespec until = deadline(ticks);
    pthread_mutex_lock(&s->lock);
    while (!s->count) if (!ticks || wait(&s->changed, &s->lock, ticks, &until)) {
        pthread_mutex_unlock(&s->lock); return pdFALSE;
    }
    s->count = 0; pthread_mutex_unlock(&s->lock); return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{ pthread_mutex_lock(&s->lock); s->count = 1; pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock); return pdTRUE; }
QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t size)
{
    QueueHandle_t q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->data = calloc(capacity, size);
    if (!q->data) { free(q); return NULL; }
    pthread_mutex_init(&q->lock, NULL); pthread_cond_init(&q->changed, NULL);
    q->capacity = capacity; q->size = size; return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    const struct timespec until = deadline(ticks); pthread_mutex_lock(&q->lock);
    while (q->count == q->capacity) if (!ticks || wait(&q->changed, &q->lock, ticks, &until)) {
        pthread_mutex_unlock(&q->lock); return pdFALSE;
    }
    memcpy(q->data + q->count++ * q->size, item, q->size);
    pthread_cond_broadcast(&q->changed); pthread_mutex_unlock(&q->lock); return pdTRUE;
}
static BaseType_t receive(QueueHandle_t q, void *item, TickType_t ticks, bool pop)
{
    const struct timespec until = deadline(ticks); pthread_mutex_lock(&q->lock);
    while (!q->count) if (!ticks || wait(&q->changed, &q->lock, ticks, &until)) {
        pthread_mutex_unlock(&q->lock); return pdFALSE;
    }
    memcpy(item, q->data, q->size);
    if (pop) { --q->count; memmove(q->data, q->data + q->size, q->count * q->size); }
    pthread_cond_broadcast(&q->changed); pthread_mutex_unlock(&q->lock); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks) { return receive(q, item, ticks, true); }
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks) { return receive(q, item, ticks, false); }
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{ pthread_mutex_lock(&q->lock); memcpy(q->data, item, q->size); q->count = 1; pthread_cond_broadcast(&q->changed); pthread_mutex_unlock(&q->lock); return pdTRUE; }
BaseType_t xQueueReset(QueueHandle_t q)
{ pthread_mutex_lock(&q->lock); q->count = 0; pthread_cond_broadcast(&q->changed); pthread_mutex_unlock(&q->lock); return pdTRUE; }
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q)
{ pthread_mutex_lock(&q->lock); const unsigned n = q->capacity - q->count; pthread_mutex_unlock(&q->lock); return n; }
typedef struct { void (*fn)(void *); void *arg; } task_t;
static void *task_entry(void *p) { task_t task = *(task_t *) p; free(p); task.fn(task.arg); return NULL; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    (void) name; (void) stack; (void) priority;
    pthread_t thread; task_t *task = malloc(sizeof(*task));
    if (!task) return pdFALSE;
    *task = (task_t) {fn, arg};
    if (out) *out = task;
    if (pthread_create(&thread, NULL, task_entry, task)) { free(task); if (out) *out = NULL; return pdFALSE; }
    pthread_detach(thread); return pdPASS;
}
#ifndef HOST_FREERTOS_NO_DELAY
void vTaskDelay(TickType_t ticks) { Sleep(ticks); }
#endif
void vTaskDelete(TaskHandle_t task) { (void) task; pthread_exit(NULL); }
#ifndef HOST_FREERTOS_NO_TIMER
int64_t esp_timer_get_time(void) { return (int64_t) GetTickCount64() * 4000; }
#endif
