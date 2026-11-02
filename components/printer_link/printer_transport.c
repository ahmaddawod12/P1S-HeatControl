#include "printer_transport.h"
#include "clock_service.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#if !defined(CONFIG_MQTT_EVENT_QUEUE_SIZE) || CONFIG_MQTT_EVENT_QUEUE_SIZE < 2
#error                                                                         \
    "Printer dispatcher requires MQTT custom configuration with at least two event slots"
#endif
#if defined(CONFIG_MQTT_DISABLE_API_LOCKS) && CONFIG_MQTT_DISABLE_API_LOCKS
#error "Printer dispatcher requires MQTT API locks"
#endif
#include "printer_deadline_transport.h"
#include "printer_report.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Trust bundle from OpenBambuAPI/examples/ca_cert.pem (retrieved 2026-11-01).
 * Includes Bambu's old and 2025 RSA/ECC CA generations. Never bypass peer
 * identity. */
#ifdef PRINTER_CA_HOST_TEST
extern const char printer_bambu_ca[];
#else
extern const char printer_bambu_ca[] asm("_binary_bambu_ca_pem_start");
#endif
typedef struct {
  esp_mqtt_client_handle_t client;
  printer_frame_t *frame;
  printer_connection_config_t config;
  atomic_uint epoch;
  atomic_int subscription, last_ack;
  atomic_bool subscription_notified;
  char report_topic[64], client_id[48];
  esp_event_handler_instance_t wifi_event, ip_event;
  esp_transport_handle_t wire;
  atomic_int send_state;
  atomic_uint revision;
  uint32_t send_revision;
  int send_id;
  uint64_t send_deadline;
  char send_topic[64], send_json[128];
  size_t send_len;
  printer_dispatch_result_t send_result;
  bool send_is_resume;
} native_t;
static native_t s;
static bool (*runtime_guard)(void *);
static void *runtime_guard_ctx;
void printer_link_set_dispatch_guard(bool (*guard)(void *), void *ctx) {
  runtime_guard = guard;
  runtime_guard_ctx = ctx;
}
static uint64_t now_ms(void *ctx) {
  (void)ctx;
  return (uint64_t)(esp_timer_get_time() / 1000);
}
static bool cancelled(void *ctx) {
  return printer_link_cancelled(((native_t *)ctx)->epoch);
}
enum { SEND_IDLE, SEND_QUEUED, SEND_EXECUTING, SEND_DONE, SEND_EXPIRED };
static bool send_allowed(void *ctx) {
  native_t *n = ctx;
  return !cancelled(n) && atomic_load(&n->send_state) == SEND_EXECUTING &&
         n->send_revision == atomic_load(&n->revision) &&
         (!n->send_is_resume ||
          (runtime_guard && runtime_guard(runtime_guard_ctx))) &&
         now_ms(NULL) < n->send_deadline;
}
static void wire_destroyed(void *ctx) { ((native_t *)ctx)->wire = NULL; }
static void maybe_subscribed(native_t *n) {
  int expected_id = atomic_load(&n->subscription);
  if (expected_id < 0 || expected_id != atomic_load(&n->last_ack))
    return;
  bool expected = false;
  if (atomic_compare_exchange_strong(&n->subscription_notified, &expected,
                                     true))
    printer_link_event_subscribed(n->epoch);
}
static void network_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data) {
  (void)data;
  native_t *n = arg;
  if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
      base == IP_EVENT)
    printer_link_invalidate(n->epoch);
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)base;
  native_t *n = arg;
  esp_mqtt_event_handle_t event = data;
  if (!event || event->client != n->client || cancelled(n))
    return;
  if (id == MQTT_USER_EVENT) {
    int expected = SEND_QUEUED;
    if (event->msg_id != n->send_id ||
        !atomic_compare_exchange_strong(&n->send_state, &expected,
                                        SEND_EXECUTING))
      return;
    n->send_result = PRINTER_DISPATCH_REJECTED;
    if (send_allowed(n)) {
      printer_deadline_transport_set(n->wire, n->send_deadline);
      int result = esp_mqtt_client_publish(
          n->client, n->send_topic, n->send_json, (int)n->send_len, 0, 0);
      printer_deadline_transport_set(n->wire, 0);
      n->send_result =
          result >= 0 ? PRINTER_DISPATCH_SENT : PRINTER_DISPATCH_UNKNOWN;
    }
    atomic_store(&n->send_state, SEND_DONE);
  } else if (id == MQTT_EVENT_CONNECTED)
    printer_link_event_connected(n->epoch);
  else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR)
    printer_link_invalidate(n->epoch);
  else if (id == MQTT_EVENT_SUBSCRIBED) {
    atomic_store(&n->last_ack, event->msg_id);
    maybe_subscribed(n);
  } else if (id == MQTT_EVENT_DATA) {
    if (event->current_data_offset == 0 &&
        (event->topic_len != (int)strlen(n->report_topic) || !event->topic ||
         memcmp(event->topic, n->report_topic, (size_t)event->topic_len)))
      return;
    if (event->data_len < 0 || event->current_data_offset < 0 ||
        event->total_data_len < 0) {
      printer_link_invalidate(n->epoch);
      return;
    }
    printer_fragment_t part = {.topic = event->topic,
                               .topic_len = (size_t)event->topic_len,
                               .data = event->data,
                               .len = (size_t)event->data_len,
                               .offset = (size_t)event->current_data_offset,
                               .total = (size_t)event->total_data_len,
                               .epoch = n->epoch,
                               .retained = event->retain};
    const char *json = NULL;
    size_t len = 0;
    printer_frame_result_t result =
        printer_frame_feed(n->frame, &part, &json, &len);
    if (result == PRINTER_FRAME_COMPLETE) {
      uint32_t revision = atomic_fetch_add(&n->revision, 1) + 1;
      printer_link_event_report(n->epoch, json, len, revision, now_ms(NULL));
    } else if (result == PRINTER_FRAME_REJECTED && !event->retain)
      printer_link_invalidate(n->epoch);
  }
}
static esp_err_t register_network(native_t *n) {
  if (n->wifi_event && n->ip_event)
    return ESP_OK;
  esp_err_t err = esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event, n,
      &n->wifi_event);
  if (err != ESP_OK)
    return err;
  err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                            network_event, n, &n->ip_event);
  if (err != ESP_OK) {
    esp_event_handler_instance_unregister(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, n->wifi_event);
    n->wifi_event = NULL;
  }
  return err;
}
static void abort_link(void *ctx) {
  native_t *n = ctx;
  if (n->client) {
    esp_mqtt_client_stop(n->client);
    esp_mqtt_client_destroy(n->client);
    n->client = NULL;
    /* SDK owns/destroys the custom transport together with the client. */
    n->wire = NULL;
  }
  free(n->frame);
  n->frame = NULL;
  memset(n->config.access_code, 0, sizeof(n->config.access_code));
}
static esp_err_t connect_link(const printer_connection_config_t *config,
                              void *ctx) {
  native_t *n = ctx;
  n->epoch = printer_link_generation();
  atomic_store(&n->subscription, -1);
  atomic_store(&n->last_ack, -1);
  atomic_store(&n->subscription_notified, false);
  n->config = *config;
  if (register_network(n) != ESP_OK)
    return ESP_ERR_INVALID_STATE;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip = {0};
  if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.ip.addr)
    return ESP_ERR_INVALID_STATE;
  esp_err_t err = clock_service_wait(config->ntp_host, 30000, cancelled, n);
  if (err != ESP_OK || cancelled(n))
    return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
  n->frame = calloc(1, sizeof(*n->frame));
  if (!n->frame)
    return ESP_ERR_NO_MEM;
  esp_transport_handle_t tls = esp_transport_ssl_init();
  if (!tls) {
    abort_link(n);
    return ESP_ERR_NO_MEM;
  }
  esp_transport_ssl_set_cert_data(tls, printer_bambu_ca,
                                  (int)strlen(printer_bambu_ca));
  esp_transport_ssl_set_common_name(tls, n->config.serial);
  n->wire = printer_deadline_transport_create(tls, now_ms, send_allowed, n);
  if (!n->wire) {
    abort_link(n);
    return ESP_ERR_NO_MEM;
  }
  printer_deadline_transport_cleanup(n->wire, wire_destroyed, n);
  atomic_store(&n->send_state, SEND_IDLE);
  snprintf(n->report_topic, sizeof(n->report_topic), "device/%s/report",
           config->serial);
  snprintf(n->client_id, sizeof(n->client_id), "heat-%s-%08lx", config->serial,
           (unsigned long)n->epoch);
  const esp_mqtt_client_config_t mqtt = {
      .broker.address = {.hostname = n->config.ipv4,
                         .port = 8883,
                         .transport = MQTT_TRANSPORT_OVER_SSL},
      .broker.verification = {.certificate = printer_bambu_ca,
                              .common_name = n->config.serial,
                              .skip_cert_common_name_check = false},
      .credentials = {.username = "bblp",
                      .client_id = n->client_id,
                      .authentication.password = n->config.access_code},
      .session = {.disable_clean_session = false, .keepalive = 30},
      .network = {.disable_auto_reconnect = true,
                  .timeout_ms = 10000,
                  .transport = n->wire},
      .buffer = {.size = 2048, .out_size = 1024},
      .task = {.stack_size = 6144, .priority = 3}};
  n->client = esp_mqtt_client_init(&mqtt);
  if (!n->client) {
    /* init can fail before or after the SDK takes ownership of the wire. */
    if (n->wire)
      esp_transport_destroy(n->wire);
    abort_link(n);
    return ESP_ERR_NO_MEM;
  }
  err = esp_mqtt_client_register_event(n->client, ESP_EVENT_ANY_ID, mqtt_event,
                                       n);
  if (err == ESP_OK)
    err = esp_mqtt_client_start(n->client);
  if (err != ESP_OK)
    abort_link(n);
  return err;
}
static esp_err_t subscribe_link(const char *topic, void *ctx) {
  native_t *n = ctx;
  if (!n->client || cancelled(n))
    return ESP_ERR_INVALID_STATE;
  int id = esp_mqtt_client_subscribe(n->client, topic, 0);
  atomic_store(&n->subscription, id);
  /* SUBACK may run on the MQTT core before subscribe() returns to this core. */
  if (id >= 0)
    maybe_subscribed(n);
  return id < 0 ? ESP_FAIL : ESP_OK;
}
static printer_dispatch_result_t
publish_link(const char *topic, const char *json, size_t len, int qos,
             bool retain, uint64_t deadline, uint32_t validated_revision,
             void *ctx) {
  native_t *n = ctx;
  if (!n->client || cancelled(n) || qos || retain || now_ms(NULL) >= deadline)
    return PRINTER_DISPATCH_REJECTED;
  if (len >= sizeof(n->send_json) || strlen(topic) >= sizeof(n->send_topic))
    return PRINTER_DISPATCH_REJECTED;
  if (atomic_load(&n->send_state) == SEND_EXECUTING ||
      atomic_load(&n->send_state) == SEND_QUEUED)
    return PRINTER_DISPATCH_REJECTED;
  strcpy(n->send_topic, topic);
  memcpy(n->send_json, json, len);
  n->send_json[len] = 0;
  n->send_len = len;
  n->send_deadline = deadline;
  n->send_is_resume = len >= 9 && !memcmp(json, "{\"print\":", 9);
  /* New callbacks cannot grant permission to contents still waiting in the
   * link queue. Resume is bound to the observation the link actually checked.
   */
  n->send_revision =
      n->send_is_resume ? validated_revision : atomic_load(&n->revision);
  ++n->send_id;
  atomic_store(&n->send_state, SEND_QUEUED);
  esp_mqtt_event_t event = {.client = n->client, .msg_id = n->send_id};
  /* User events run inside MQTT's task/API lock: no unbounded API mutex wait.
   * An expired event is inert; QoS 0 never enters the retransmission outbox. */
  if (esp_mqtt_dispatch_custom_event(n->client, &event) != ESP_OK) {
    atomic_store(&n->send_state, SEND_EXPIRED);
    return PRINTER_DISPATCH_REJECTED;
  }
  while (now_ms(NULL) < deadline && !cancelled(n)) {
    if (atomic_load(&n->send_state) == SEND_DONE)
      return n->send_result;
    uint64_t left = deadline - now_ms(NULL);
    vTaskDelay(pdMS_TO_TICKS(left < 10 ? left : 10));
  }
  int previous = atomic_exchange(&n->send_state, SEND_EXPIRED);
  return previous == SEND_QUEUED ? PRINTER_DISPATCH_REJECTED
                                 : PRINTER_DISPATCH_UNKNOWN;
}
const printer_link_ops_t *printer_transport_ops(void) {
  static const printer_link_ops_t ops = {
      connect_link, subscribe_link, publish_link, abort_link, now_ms, &s};
  return &ops;
}
