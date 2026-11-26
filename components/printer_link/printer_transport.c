#include "printer_transport.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#if defined(CONFIG_MBEDTLS_HAVE_TIME_DATE) && defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "clock_service.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#define PRINTER_LAN_TLS_WITHOUT_CLOCK 1
#endif
#include "printer_report.h"
#include <stdatomic.h>
#include <stdio.h>
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
  printer_frame_t frame;
  printer_connection_config_t config;
  atomic_uint epoch;
  atomic_int subscription, last_ack;
  atomic_bool subscription_notified;
  char report_topic[64], client_id[24];
#ifdef PRINTER_LAN_TLS_WITHOUT_CLOCK
  mbedtls_x509_crt lan_ca;
#endif
  esp_event_handler_instance_t wifi_event, ip_event;
} native_t;
static native_t s;
#ifdef PRINTER_LAN_TLS_WITHOUT_CLOCK
static int verify_lan_printer_date(void *ctx, mbedtls_x509_crt *cert,
                                   int depth, uint32_t *flags) {
  (void)ctx;
  (void)cert;
  (void)depth;
  if (!clock_service_has_valid_wall_time()) {
    *flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
  }
  return 0;
}

/* ESP-TLS invokes this hook with peer verification required. Only the
 * configured LAN printer uses this CA and the unsynchronized-clock exception. */
static esp_err_t attach_lan_printer_ca(void *conf) {
  mbedtls_ssl_conf_ca_chain(conf, &s.lan_ca, NULL);
  mbedtls_ssl_conf_verify(conf, verify_lan_printer_date, NULL);
  return ESP_OK;
}
#endif
static uint64_t now_ms(void) {
  return (uint64_t)(esp_timer_get_time() / 1000);
}
static bool cancelled(void *ctx) {
  return printer_link_cancelled(((native_t *)ctx)->epoch);
}
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
      (base == IP_EVENT &&
       (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_STA_LOST_IP)))
    printer_link_invalidate(n->epoch);
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)base;
  native_t *n = arg;
  esp_mqtt_event_handle_t event = data;
  if (!event || event->client != n->client || cancelled(n))
    return;
  if (id == MQTT_EVENT_CONNECTED)
    printer_link_event_connected(n->epoch);
  else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR)
    printer_link_invalidate(n->epoch);
  else if (id == MQTT_EVENT_SUBSCRIBED) {
    if (event->error_handle &&
        event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
      printer_link_invalidate(n->epoch);
      return;
    }
    atomic_store(&n->last_ack, event->msg_id);
    maybe_subscribed(n);
  } else if (id == MQTT_EVENT_DATA) {
    if (event->current_data_offset == 0 &&
        (event->topic_len != (int)strlen(n->report_topic) || !event->topic ||
         memcmp(event->topic, n->report_topic, (size_t)event->topic_len))) {
      printer_frame_clear(&n->frame);
      return;
    }
    if (event->data_len < 0 || event->current_data_offset < 0 ||
        event->total_data_len < 0) {
      printer_frame_clear(&n->frame);
      printer_link_event_report_rejected(n->epoch);
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
        printer_frame_feed(&n->frame, &part, &json, &len);
    if (result == PRINTER_FRAME_COMPLETE) {
      printer_link_event_report(n->epoch, json, len, now_ms());
      printer_frame_clear(&n->frame);
    } else if (result == PRINTER_FRAME_REJECTED && !event->retain) {
      printer_link_event_report_rejected(n->epoch);
    }
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
  }
#ifdef PRINTER_LAN_TLS_WITHOUT_CLOCK
  mbedtls_x509_crt_free(&n->lan_ca);
#endif
  printer_frame_clear(&n->frame);
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
  if (cancelled(n))
    return ESP_ERR_INVALID_STATE;
  uint8_t mac[6];
  const esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
  if (mac_err != ESP_OK)
    return mac_err;
  esp_err_t (*attach_certificate)(void *) = NULL;
#ifdef PRINTER_LAN_TLS_WITHOUT_CLOCK
  if (!clock_service_has_valid_wall_time()) {
    mbedtls_x509_crt_init(&n->lan_ca);
    if (mbedtls_x509_crt_parse(&n->lan_ca, (const unsigned char *)printer_bambu_ca,
                              strlen(printer_bambu_ca) + 1U) != 0) {
      abort_link(n);
      return ESP_ERR_INVALID_STATE;
    }
    attach_certificate = attach_lan_printer_ca;
  }
#endif
  snprintf(n->report_topic, sizeof(n->report_topic), "device/%s/report",
           config->serial);
  snprintf(n->client_id, sizeof(n->client_id), "p1s%02x%02x%02x%02x%02x%02x%08lx",
           (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
           (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5], (unsigned long)n->epoch);
  const esp_mqtt_client_config_t mqtt = {
      .broker.address = {.hostname = n->config.ipv4,
                         .port = 8883,
                         .transport = MQTT_TRANSPORT_OVER_SSL},
      .broker.verification = {.certificate = printer_bambu_ca,
                              .common_name = n->config.serial,
                              .crt_bundle_attach = attach_certificate,
                              .skip_cert_common_name_check = false},
      .credentials = {.username = "bblp",
                      .client_id = n->client_id,
                      .authentication.password = n->config.access_code},
      .session = {.disable_clean_session = false, .keepalive = 30},
      .network = {.disable_auto_reconnect = true,
                  .timeout_ms = 10000},
      .buffer = {.size = 2048, .out_size = 1024},
      .task = {.stack_size = 6144, .priority = 3}};
  n->client = esp_mqtt_client_init(&mqtt);
  if (!n->client) {
    abort_link(n);
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = esp_mqtt_client_register_event(n->client, ESP_EVENT_ANY_ID, mqtt_event,
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
const printer_link_ops_t *printer_transport_ops(void) {
  static const printer_link_ops_t ops = {
      .connect = connect_link, .subscribe = subscribe_link,
      .abort = abort_link, .ctx = &s};
  return &ops;
}
