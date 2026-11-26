#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

struct host_semaphore { pthread_mutex_t lock; pthread_cond_t changed; unsigned count; };
struct host_queue { pthread_mutex_t lock; pthread_cond_t changed; unsigned capacity, size, count; unsigned char *data; };
static atomic_bool pause_control_queue, control_queue_paused;
static _Atomic(QueueHandle_t) paused_control_queue;
static atomic_bool early_reply_timeout;
void host_control_queue_pause(bool paused) { atomic_store(&pause_control_queue, paused); }
bool host_control_queue_is_paused(void) { return atomic_load(&control_queue_paused); }
bool host_control_queue_has_command(void)
{
    QueueHandle_t queue = atomic_load(&paused_control_queue);
    return queue && uxQueueSpacesAvailable(queue) < queue->capacity;
}
void host_reply_queue_timeout_early(void) { atomic_store(&early_reply_timeout, true); }
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
void vSemaphoreDelete(SemaphoreHandle_t s)
{
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    if (!storage->handle) storage->handle = xSemaphoreCreateMutex();
    return storage->handle;
}
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
void vQueueDelete(QueueHandle_t q)
{
    pthread_cond_destroy(&q->changed);
    pthread_mutex_destroy(&q->lock);
    free(q->data);
    free(q);
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
    if (pop && q->capacity == 1 && atomic_exchange(&early_reply_timeout, false)) return pdFALSE;
    if (pop && q->capacity == 8 && atomic_load(&pause_control_queue)) {
        atomic_store(&paused_control_queue, q);
        atomic_store(&control_queue_paused, true);
        while (atomic_load(&pause_control_queue)) Sleep(1);
        atomic_store(&control_queue_paused, false);
        atomic_store(&paused_control_queue, NULL);
    }
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
typedef struct { void (*fn)(void *); void *arg; SemaphoreHandle_t notification; } task_t;
static _Thread_local task_t *current_task;
static void task_cleanup(void *p)
{
    task_t *task = p;
    vSemaphoreDelete(task->notification);
    free(task);
}
static void *task_entry(void *p)
{
    current_task = p;
    pthread_cleanup_push(task_cleanup, p);
    current_task->fn(current_task->arg);
    pthread_cleanup_pop(1);
    return NULL;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    (void) name; (void) stack; (void) priority;
    pthread_t thread; task_t *task = malloc(sizeof(*task));
    if (!task) return pdFALSE;
    *task = (task_t) {fn, arg, xSemaphoreCreateBinary()};
    if (!task->notification) { free(task); return pdFALSE; }
    if (out) *out = task;
    if (pthread_create(&thread, NULL, task_entry, task)) { task_cleanup(task); if (out) *out = NULL; return pdFALSE; }
    pthread_detach(thread); return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t handle)
{
    const task_t *task = handle;
    SemaphoreHandle_t notification = task->notification;
    pthread_mutex_lock(&notification->lock);
    ++notification->count;
    pthread_cond_signal(&notification->changed);
    pthread_mutex_unlock(&notification->lock);
    return pdTRUE;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    SemaphoreHandle_t notification = current_task->notification;
    const struct timespec until = deadline(ticks);
    pthread_mutex_lock(&notification->lock);
    while (!notification->count) if (!ticks || wait(&notification->changed, &notification->lock, ticks, &until)) {
        pthread_mutex_unlock(&notification->lock);
        return 0;
    }
    const uint32_t count = notification->count;
    notification->count = clear ? 0 : count - 1;
    pthread_mutex_unlock(&notification->lock);
    return count;
}
#ifndef HOST_FREERTOS_NO_DELAY
void vTaskDelay(TickType_t ticks) { Sleep(ticks); }
#endif
void vTaskDelete(TaskHandle_t task) { (void) task; pthread_exit(NULL); }
#ifndef HOST_FREERTOS_NO_TIMER
int64_t esp_timer_get_time(void) { return (int64_t) GetTickCount64() * 4000; }
#endif
