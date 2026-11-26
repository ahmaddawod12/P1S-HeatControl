/* Printer link + native transport with synthetic ESP-MQTT/netif boundaries.
 * The link subscribes to reports and only reads gcode_state. */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "printer_link.h"
#include "printer_monitor.h"
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
static unsigned aborted, inits;
static struct esp_mqtt_client client;
static enum { SUBSCRIBE_ACCEPT, SUBSCRIBE_SILENT, SUBSCRIBE_REJECT } subscription_reply;
static uint8_t station_mac[] = {0x02, 0x16, 0x3e, 0x10, 0x20, 0x30};
esp_err_t esp_wifi_get_mac(int interface, uint8_t mac[6]) {
  assert(interface == WIFI_IF_STA);
  memcpy(mac, station_mac, sizeof(station_mac));
  return ESP_OK;
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
  return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c) {
  assert(c == &client);
  ++aborted;
  return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) {
  assert(c == &client);
  return ESP_OK;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic,
                              int qos) {
  assert(c == &client && !strcmp(topic, "device/SERIAL123/report") && qos == 0);
  if (subscription_reply != SUBSCRIBE_SILENT) {
    esp_mqtt_error_codes_t result = {
        .error_type = subscription_reply == SUBSCRIBE_REJECT
            ? MQTT_ERROR_TYPE_SUBSCRIBE_FAILED : MQTT_ERROR_TYPE_NONE,
    };
    esp_mqtt_event_t event = {.client = c, .msg_id = 17, .error_handle = &result};
    mqtt_handler(mqtt_arg, "MQTT", MQTT_EVENT_SUBSCRIBED, &event);
  }
  return 17;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic,
                            const char *data, int len, int qos, int retain) {
  (void)c;
  (void)topic;
  (void)data;
  (void)len;
  (void)qos;
  (void)retain;
  assert(!"the printer link must only subscribe");
  return -1;
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
static void start_link(void) {
  now = 0;
  aborted = inits = 0;
  assert(printer_link_start() == ESP_OK);
  const printer_connection_config_t config = {.enabled = true,
                                             .ipv4 = "192.168.1.20",
                                             .serial = "SERIAL123",
                                             .access_code = "Secret"};
  assert(printer_link_configure(&config) == ESP_OK);
  printer_link_tick(now);
}
/* Fresh link subscribed at t=0, before the first report. */
static void start_connected(void) {
  start_link();
  connect_events();
}
/* Disconnect, then let the 1 s retry delay elapse so the link reconnects at t. */
static void reconnect_at(uint64_t t) {
  event(MQTT_EVENT_DISCONNECTED, NULL);
  tick_at(t - 1000);
  tick_at(t);
  connect_events();
}

static void first_connect_subscribes_and_waits_for_state(void) {
  start_connected();
  assert(recorded.broker.address.port == 8883);
  assert(recorded.broker.address.transport == MQTT_TRANSPORT_OVER_SSL);
  assert(!strcmp(recorded.broker.address.hostname, "192.168.1.20"));
  assert(!strcmp(recorded.credentials.username, "bblp"));
  assert(strlen(recorded.credentials.client_id) == 23);
  assert(!strncmp(recorded.credentials.client_id, "p1s02163e102030", 15));
  assert(!strcmp(recorded.credentials.authentication.password, "Secret"));
  assert(recorded.broker.verification.certificate == printer_bambu_ca);
  assert(!recorded.broker.verification.skip_cert_common_name_check);
  assert(!strcmp(recorded.broker.verification.common_name, "SERIAL123"));
  assert(recorded.network.disable_auto_reconnect);
  assert(recorded.network.timeout_ms == 10000);
  printer_link_status_t s = status();
  assert(s.connected && !s.state_known && s.reconnects == 0);
  report_state("PAUSE");
  printer_link_tick(now);
  s = status();
  assert(s.state_known && s.state == PRINTER_RUN_PAUSED && s.received_ms == now);
}
static void connect_timeout_retries(void) {
  start_link();
  tick_at(29999);
  assert(inits == 1 && aborted == 0 && !status().connected);
  tick_at(30000);
  tick_at(30001);
  assert(inits == 1 && aborted == 1 && !status().connected);
  tick_at(31001);
  assert(inits == 2);
  connect_events();
}
static void subscribe_timeout_retries(void) {
  subscription_reply = SUBSCRIBE_SILENT;
  start_link();
  event(MQTT_EVENT_CONNECTED, NULL);
  tick_at(2000);
  tick_at(16999);
  assert(inits == 1 && aborted == 0 && !status().connected);
  tick_at(17000);
  tick_at(17001);
  assert(inits == 1 && aborted == 1 && !status().connected);
  tick_at(18001);
  assert(inits == 2);
  subscription_reply = SUBSCRIBE_ACCEPT;
  connect_events();
}
static void rejected_subscription_retries_with_backoff(void) {
  subscription_reply = SUBSCRIBE_REJECT;
  start_link();
  event(MQTT_EVENT_CONNECTED, NULL);
  tick_at(0);
  assert(!status().connected);
  tick_at(1);
  assert(aborted == 1 && inits == 1);
  tick_at(1001);
  assert(inits == 2);
  event(MQTT_EVENT_CONNECTED, NULL);
  tick_at(1001);
  tick_at(1002);
  assert(aborted == 2 && !status().connected);
  tick_at(2002);
  assert(inits == 2);
  tick_at(3002);
  assert(inits == 3);
  subscription_reply = SUBSCRIBE_ACCEPT;
  connect_events();
}
static void client_id_uses_the_controller_address(void) {
  start_connected();
  char first_id[24];
  memcpy(first_id, recorded.credentials.client_id, sizeof(first_id));
  station_mac[5] = 0x31;
  reconnect_at(11000);
  assert(strncmp(first_id, recorded.credentials.client_id, 15) != 0);
  assert(!strncmp(recorded.credentials.client_id, "p1s02163e102031", 15));
}
static void known_state_keeps_receiving_deltas(void) {
  start_connected();
  report_state("RUNNING");
  for (uint64_t t = 1000; t <= 600000; t += 1000) {
    tick_at(t);
    if (t % 20000 == 0)
      event(MQTT_EVENT_DATA, "{\"print\":{\"command\":\"push_status\",\"mc_percent\":5}}");
  }
  assert(status().state_known && status().state == PRINTER_RUN_RUNNING);
  assert(aborted == 0);
}
static void reconnect_waits_for_print_state(void) {
  start_connected();
  report_state("RUNNING");
  printer_link_tick(now);
  printer_monitor_status_t monitor;
  printer_monitor_init(&monitor);
  printer_link_status_t link = status();
  assert(printer_monitor_start(&monitor, &link, now));
  reconnect_at(60000);
  now = 300000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"command\":\"push_status\",\"mc_percent\":5}}");
  tick_at(now);
  link = status();
  assert(link.connected && !link.state_known && link.received_ms == now);
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.hold == PRINTER_HOLD_UNKNOWN);
  assert(!printer_monitor_heat_allowed(&monitor));

  now = 300500;
  report_state("RUNNING");
  tick_at(now);
  link = status();
  assert(link.state_known && link.state == PRINTER_RUN_RUNNING);
  printer_monitor_step(&monitor, &link, now);
  assert(printer_monitor_heat_allowed(&monitor));
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
  tick_at(300000);
  assert(!status().state_known && aborted == 0);
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
static void monitor_observes_completion_burst(const char *scenario) {
  start_connected();
  report_state(!strcmp(scenario, "waiting_burst") ? "FINISH" : "RUNNING");
  tick_at(1000);
  printer_monitor_status_t monitor;
  printer_monitor_init(&monitor);
  printer_link_status_t link = status();
  assert(printer_monitor_start(&monitor, &link, now));

  now = 2000;
  report_state("RUNNING");
  report_state("FINISH");
  if (!strcmp(scenario, "watching_burst"))
    report_state("RUNNING");
  else if (!strcmp(scenario, "completed_unknown"))
    event(MQTT_EVENT_DATA, "{malformed");
  else if (!strcmp(scenario, "completed_reconnect")) {
    reconnect_at(4000);
    report_state("RUNNING");
  }
  printer_link_tick(now);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_OFF);
  assert(monitor.last_end == PRINTER_MONITOR_END_FINISHED);
  assert(!printer_monitor_heat_allowed(&monitor));
  assert(printer_monitor_end_disables_heating(monitor.last_end));
}
static void monitor_retains_start_when_latest_state_is_lost(const char *scenario) {
  const bool disconnected = !strcmp(scenario, "started_disconnect");
  const bool malformed = !strcmp(scenario, "started_malformed");
  start_connected();
  report_state("FINISH");
  tick_at(1000);
  printer_monitor_status_t monitor;
  printer_monitor_init(&monitor);
  printer_link_status_t link = status();
  assert(printer_monitor_start(&monitor, &link, now));
  assert(monitor.phase == PRINTER_MONITOR_WAITING);

  /* The start arrives just before the no-print deadline, then is hidden
   * before the coordinator can observe it. */
  now = 3600000;
  report_state(disconnected ? "RUNNING" : malformed ? "PAUSE" : "PREPARE");
  if (disconnected)
    event(MQTT_EVENT_DISCONNECTED, NULL);
  else if (malformed)
    event(MQTT_EVENT_DATA, "{malformed");
  else
    report_state("UNKNOWN");
  printer_link_tick(now);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(!printer_monitor_heat_allowed(&monitor));
  assert(monitor.hold == (disconnected ? PRINTER_HOLD_DISCONNECTED
                                      : PRINTER_HOLD_UNKNOWN));

  now = 3601000;
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_WATCHING);
  assert(monitor.last_end == PRINTER_MONITOR_END_NONE);
  assert(!printer_monitor_heat_allowed(&monitor));

  if (disconnected) {
    printer_link_tick(now);
    connect_events();
    link = status();
    printer_monitor_step(&monitor, &link, now);
    assert(monitor.phase == PRINTER_MONITOR_WATCHING);
    assert(monitor.hold == PRINTER_HOLD_UNKNOWN);
    assert(!printer_monitor_heat_allowed(&monitor));
  }
  now = 3602000;
  report_state("RUNNING");
  printer_link_tick(now);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_WATCHING);
  assert(printer_monitor_heat_allowed(&monitor));

  now = 3603000;
  report_state("FINISH");
  printer_link_tick(now);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_OFF);
  assert(monitor.last_end == PRINTER_MONITOR_END_FINISHED);
  assert(printer_monitor_end_disables_heating(monitor.last_end));
}
static void prior_completion_does_not_end_new_monitor(void) {
  start_connected();
  report_state("RUNNING");
  report_state("FINISH");
  tick_at(1000);
  printer_monitor_status_t monitor;
  printer_monitor_init(&monitor);
  printer_link_status_t link = status();
  assert(printer_monitor_start(&monitor, &link, now));
  assert(monitor.phase == PRINTER_MONITOR_WAITING);
  report_state("FINISH");
  tick_at(2000);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_WAITING);
  assert(printer_monitor_heat_allowed(&monitor));

  event(MQTT_EVENT_DISCONNECTED, NULL);
  tick_at(3000);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_WAITING);
  assert(monitor.hold == PRINTER_HOLD_DISCONNECTED);
  tick_at(4000);
  connect_events();
  report_state("FINISH");
  printer_link_tick(now);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_WAITING);
  assert(printer_monitor_heat_allowed(&monitor));
  report_state("RUNNING");
  report_state("FAILED");
  tick_at(5000);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_OFF);
  assert(monitor.last_end == PRINTER_MONITOR_END_FAILED);
}
static void unrecognized_state_waits_for_known_report(void) {
  start_connected();
  report_state("UNKNOWN");
  tick_at(1000);
  assert(status().connected && status().state == PRINTER_RUN_UNKNOWN);
  assert(!status().state_known);
  tick_at(300000);
  assert(!status().state_known && inits == 1 && aborted == 0);
  report_state("RUNNING");
  tick_at(300100);
  assert(status().state_known && status().state == PRINTER_RUN_RUNNING);
}
static void pending_completion_before_start_is_historical(void) {
  start_connected();
  report_state("FINISH");
  tick_at(1000);
  now = 2000;
  report_state("RUNNING");
  report_state("FINISH");
  /* Start before the worker has drained the two reports above. */
  printer_link_status_t link = status();
  printer_monitor_status_t monitor;
  printer_monitor_init(&monitor);
  assert(printer_monitor_start(&monitor, &link, now));
  tick_at(2100);
  link = status();
  printer_monitor_step(&monitor, &link, now);
  assert(monitor.phase == PRINTER_MONITOR_WAITING);
  assert(printer_monitor_heat_allowed(&monitor));
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
      first_connect_subscribes_and_waits_for_state();
    else if (!strcmp(argv[1], "connect_timeout"))
      connect_timeout_retries();
    else if (!strcmp(argv[1], "subscribe_timeout"))
      subscribe_timeout_retries();
    else if (!strcmp(argv[1], "subscribe_rejected"))
      rejected_subscription_retries_with_backoff();
    else if (!strcmp(argv[1], "client_id"))
      client_id_uses_the_controller_address();
    else if (!strcmp(argv[1], "known_state"))
      known_state_keeps_receiving_deltas();
    else if (!strcmp(argv[1], "reconnect_waits_for_report"))
      reconnect_waits_for_print_state();
    else if (!strcmp(argv[1], "report_without_state"))
      report_without_state_updates_received();
    else if (!strcmp(argv[1], "malformed"))
      malformed_report_marks_unknown_without_reconnect();
    else if (!strcmp(argv[1], "burst"))
      report_burst_never_reconnects();
    else if (!strcmp(argv[1], "waiting_burst") ||
             !strcmp(argv[1], "watching_burst") ||
             !strcmp(argv[1], "completed_unknown") ||
             !strcmp(argv[1], "completed_reconnect"))
      monitor_observes_completion_burst(argv[1]);
    else if (!strcmp(argv[1], "started_disconnect") ||
             !strcmp(argv[1], "started_unknown") ||
             !strcmp(argv[1], "started_malformed"))
      monitor_retains_start_when_latest_state_is_lost(argv[1]);
    else if (!strcmp(argv[1], "prior_completion"))
      prior_completion_does_not_end_new_monitor();
    else if (!strcmp(argv[1], "pending_completion"))
      pending_completion_before_start_is_historical();
    else if (!strcmp(argv[1], "unknown_recovery"))
      unrecognized_state_waits_for_known_report();
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
