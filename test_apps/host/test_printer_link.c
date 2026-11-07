/* Printer link + native transport with synthetic ESP-MQTT/netif boundaries.
 * The link only reads gcode_state and only ever publishes pushall. */
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "printer_link.h"
#include "printer_transport.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now;
const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
static esp_event_handler_t mqtt_handler;
static void *mqtt_arg;
static esp_event_handler_t ip_handler;
static void *ip_arg;
static int32_t ip_event_id;
static esp_mqtt_client_config_t recorded;
static unsigned pushalls, aborted, inits;
static bool pending_dispatch;
static struct esp_mqtt_client client;
static esp_mqtt_event_t pending_event;
esp_err_t esp_mqtt_dispatch_custom_event(esp_mqtt_client_handle_t c,
                                         esp_mqtt_event_t *event) {
  assert(c == &client);
  pending_event = *event;
  pending_dispatch = true;
  return ESP_OK;
}
void vTaskDelay(TickType_t ticks) {
  now += (uint64_t)ticks * 4;
  if (pending_dispatch) {
    pending_dispatch = false;
    mqtt_handler(mqtt_arg, "MQTT", MQTT_USER_EVENT, &pending_event);
  }
}
const char printer_bambu_ca[] =
    "-----BEGIN CERTIFICATE-----\ntest CA\n-----END CERTIFICATE-----\n";
int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
esp_mqtt_client_handle_t
esp_mqtt_client_init(const esp_mqtt_client_config_t *config) {
  recorded = *config;
  ++inits;
  return &client;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int32_t id,
                                         esp_event_handler_t h, void *arg) {
  assert(c == &client && id == ESP_EVENT_ANY_ID);
  mqtt_handler = h;
  mqtt_arg = arg;
  return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
  assert(c == &client);
  assert(esp_transport_connect(recorded.network.transport, "192.168.1.20", 8883,
                               10000) == 0);
  return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c) {
  assert(c == &client);
  ++aborted;
  return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) {
  assert(c == &client);
  pending_dispatch = false;
  esp_transport_destroy(recorded.network.transport);
  return ESP_OK;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic,
                              int qos) {
  assert(c == &client && !strcmp(topic, "device/SERIAL123/report") && qos == 0);
  esp_mqtt_event_t event = {.client = c, .msg_id = 17};
  mqtt_handler(mqtt_arg, "MQTT", MQTT_EVENT_SUBSCRIBED, &event);
  return 17;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic,
                            const char *data, int len, int qos, int retain) {
  assert(c == &client && !strcmp(topic, "device/SERIAL123/request"));
  assert(qos == 0 && !retain && len > 0);
  /* The only message this firmware ever sends to the printer. */
  assert(strstr(data, "\"pushing\"") && strstr(data, "\"pushall\""));
  ++pushalls;
  assert(esp_transport_write(recorded.network.transport, data, len, 10000) ==
         len);
  return 0;
}
esp_err_t
esp_event_handler_instance_register(esp_event_base_t b, int32_t id,
                                    esp_event_handler_t h, void *arg,
                                    esp_event_handler_instance_t *inst) {
  if (b == IP_EVENT) {
    ip_handler = h;
    ip_arg = arg;
    ip_event_id = id;
  }
  *inst = (void *)1;
  return ESP_OK;
}
esp_err_t
esp_event_handler_instance_unregister(esp_event_base_t b, int32_t id,
                                      esp_event_handler_instance_t i) {
  (void)b;
  (void)id;
  (void)i;
  return ESP_OK;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
  assert(!strcmp(key, "WIFI_STA_DEF"));
  return (esp_netif_t *)1;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *net, esp_netif_ip_info_t *ip) {
  (void)net;
  ip->ip.addr = 1;
  return ESP_OK;
}
static void event(int id, const char *json) {
  esp_mqtt_event_t e = {.event_id = id, .client = &client, .msg_id = 17};
  if (json) {
    e.topic = "device/SERIAL123/report";
    e.topic_len = (int)strlen(e.topic);
    e.data = (char *)json;
    e.data_len = e.total_data_len = (int)strlen(json);
  }
  mqtt_handler(mqtt_arg, "MQTT", id, &e);
}
static printer_link_status_t status(void) {
  printer_link_status_t s;
  printer_link_get_status(&s);
  return s;
}
static void tick_at(uint64_t t) {
  now = t;
  printer_link_tick(now);
}
static void connect_events(void) {
  event(MQTT_EVENT_CONNECTED, NULL);
  printer_link_tick(now);
  printer_link_tick(now);
  assert(status().connected);
}
static void report_state(const char *state) {
  char json[128];
  snprintf(json, sizeof(json),
           "{\"print\":{\"command\":\"push_status\",\"gcode_state\":\"%s\"}}", state);
  event(MQTT_EVENT_DATA, json);
}
/* Fresh link connected at t=0 with one pushall already sent. */
static void start_connected(void) {
  now = 0;
  pushalls = aborted = inits = 0;
  assert(printer_link_start() == ESP_OK);
  const printer_connection_config_t config = {.enabled = true,
                                             .ipv4 = "192.168.1.20",
                                             .serial = "SERIAL123",
                                             .access_code = "Secret"};
  assert(printer_link_configure(&config) == ESP_OK);
  printer_link_tick(now);
  connect_events();
}
/* Disconnect, then let the 1 s retry delay elapse so the link reconnects at t. */
static void reconnect_at(uint64_t t) {
  event(MQTT_EVENT_DISCONNECTED, NULL);
  tick_at(t - 1000);
  tick_at(t);
  connect_events();
}

static void first_connect_sends_one_pushall(void) {
  start_connected();
  assert(recorded.broker.address.port == 8883);
  assert(recorded.broker.address.transport == MQTT_TRANSPORT_OVER_SSL);
  assert(!strcmp(recorded.broker.address.hostname, "192.168.1.20"));
  assert(!strcmp(recorded.credentials.username, "bblp"));
  assert(!strcmp(recorded.credentials.authentication.password, "Secret"));
  assert(recorded.broker.verification.certificate == printer_bambu_ca);
  assert(!recorded.broker.verification.skip_cert_common_name_check);
  assert(!strcmp(recorded.broker.verification.common_name, "SERIAL123"));
  assert(recorded.network.disable_auto_reconnect);
  assert(pushalls == 1);
  printer_link_status_t s = status();
  assert(s.connected && !s.state_known && s.reconnects == 0);
  report_state("PAUSE");
  printer_link_tick(now);
  s = status();
  assert(s.state_known && s.state == PRINTER_RUN_PAUSED && s.received_ms == now);
}
static void known_state_no_periodic_pushall(void) {
  start_connected();
  report_state("RUNNING");
  for (uint64_t t = 1000; t <= 600000; t += 1000) {
    tick_at(t);
    if (t % 20000 == 0)
      event(MQTT_EVENT_DATA, "{\"print\":{\"command\":\"push_status\",\"mc_percent\":5}}");
  }
  assert(pushalls == 1 && aborted == 0);
}
static void reconnect_waits_for_pushall_interval(void) {
  start_connected();
  report_state("RUNNING");
  printer_link_tick(now);
  reconnect_at(60000);
  assert(!status().state_known && pushalls == 1);
  tick_at(299999);
  assert(pushalls == 1);
  tick_at(300000);
  assert(pushalls == 2);
  tick_at(300500);
  assert(pushalls == 2);
  report_state("RUNNING");
  tick_at(900000);
  assert(pushalls == 2 && status().state == PRINTER_RUN_RUNNING);
}
static void report_without_state_updates_received(void) {
  start_connected();
  report_state("RUNNING");
  tick_at(1000);
  now = 5000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"command\":\"push_status\",\"mc_percent\":7}}");
  printer_link_tick(now);
  const printer_link_status_t s = status();
  assert(s.received_ms == 5000 && s.state_known && s.state == PRINTER_RUN_RUNNING);
}
static void malformed_report_marks_unknown_without_reconnect(void) {
  start_connected();
  report_state("RUNNING");
  tick_at(1000);
  event(MQTT_EVENT_DATA, "{\"print\":{\"command\":\"push_status\",\"gcode_state\":\"RUN");
  tick_at(2000);
  printer_link_status_t s = status();
  assert(!s.state_known && s.connected && aborted == 0 && inits == 1);
  tick_at(299999);
  assert(pushalls == 1);
  tick_at(300000);
  assert(pushalls == 2 && aborted == 0);
  report_state("RUNNING");
  tick_at(300100);
  assert(status().state_known);
}
static void report_burst_never_reconnects(void) {
  start_connected();
  for (unsigned i = 0; i < 200; ++i) {
    if (i == 199)
      report_state("FINISH");
    else if (i % 2)
      report_state("RUNNING");
    else
      event(MQTT_EVENT_DATA, "{\"print\":{\"command\":\"push_status\",\"layer_num\":3}}");
  }
  printer_link_tick(now);
  const printer_link_status_t s = status();
  assert(s.state_known && s.state == PRINTER_RUN_FINISHED);
  assert(aborted == 0 && inits == 1 && s.connected);
}
static void reconnect_counter(void) {
  start_connected();
  reconnect_at(10000);
  reconnect_at(20000);
  assert(status().reconnects == 2 && inits == 3);
}
static void disconnect_clears_state(void) {
  start_connected();
  report_state("RUNNING");
  printer_link_tick(now);
  event(MQTT_EVENT_DISCONNECTED, NULL);
  printer_link_status_t s = status();
  assert(!s.connected);
  printer_link_tick(now);
  s = status();
  assert(!s.connected && !s.state_known && s.received_ms == 0);
}
static void network_event(int32_t id) {
  start_connected();
  report_state("RUNNING");
  printer_link_tick(now);
  assert(ip_handler && (ip_event_id == ESP_EVENT_ANY_ID || ip_event_id == id));
  ip_handler(ip_arg, IP_EVENT, id, NULL);
  if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
    assert(status().connected);
  } else {
    assert(!status().connected);
    printer_link_tick(now);
    assert(!status().state_known);
  }
}
int main(int argc, char **argv) {
  if (argc == 2) {
    if (!strcmp(argv[1], "network_ap"))
      network_event(IP_EVENT_ASSIGNED_IP_TO_CLIENT);
    else if (!strcmp(argv[1], "network_got_ip"))
      network_event(IP_EVENT_STA_GOT_IP);
    else if (!strcmp(argv[1], "network_lost_ip"))
      network_event(IP_EVENT_STA_LOST_IP);
    else if (!strcmp(argv[1], "first_connect"))
      first_connect_sends_one_pushall();
    else if (!strcmp(argv[1], "known_state"))
      known_state_no_periodic_pushall();
    else if (!strcmp(argv[1], "reconnect_interval"))
      reconnect_waits_for_pushall_interval();
    else if (!strcmp(argv[1], "report_without_state"))
      report_without_state_updates_received();
    else if (!strcmp(argv[1], "malformed"))
      malformed_report_marks_unknown_without_reconnect();
    else if (!strcmp(argv[1], "burst"))
      report_burst_never_reconnects();
    else if (!strcmp(argv[1], "reconnect_counter"))
      reconnect_counter();
    else if (!strcmp(argv[1], "disconnect"))
      disconnect_clears_state();
    else
      assert(!"unknown printer link case");
    puts("printer_link: PASS");
    return 0;
  }
  assert(!"pass a case name");
  return 1;
}
