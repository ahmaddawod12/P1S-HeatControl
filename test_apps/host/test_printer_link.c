#include "clock_service.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "printer_control.h"
#include "printer_control_engine.h"
#include "printer_link.h"
#include "printer_transport.h"
#include "runtime_auto_lease.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now;
const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
static esp_mqtt_event_handle_t unused;
static esp_event_handler_t mqtt_handler;
static void *mqtt_arg;
static esp_mqtt_client_config_t recorded;
static unsigned snapshot_request_count, aborted, clock_calls,
    resume_publish_count;
static bool block_dispatch, pending_dispatch;
static bool early_ack = true;
static bool report_before_publish;
static const printer_link_ops_t *native_ops;
static void event(int id, const char *json, bool retained);
static printer_dispatch_result_t
interposed_publish(const char *topic, const char *json, size_t len, int qos,
                   bool retain, uint64_t deadline, uint32_t validated_revision,
                   void *ctx) {
  if (report_before_publish && strstr(json, "\"resume\"")) {
    report_before_publish = false;
    /* A callback after link validation, before the native send is set up. */
    event(MQTT_EVENT_DATA, "{\"print\":{\"print_error\":42}}", false);
  }
  return native_ops->publish_live(topic, json, len, qos, retain, deadline,
                                  validated_revision, ctx);
}
static bool runtime_permitted = true;
static bool runtime_guard(void *ctx) {
  (void)ctx;
  return runtime_permitted;
}
static struct esp_mqtt_client client;
static esp_mqtt_event_t pending_event;
esp_err_t esp_mqtt_dispatch_custom_event(esp_mqtt_client_handle_t c,
                                         esp_mqtt_event_t *event) {
  assert(c == &client);
  pending_event = *event;
  /* ESP-MQTT posts custom event bytes unchanged; it does not fill client. */
  pending_dispatch = true;
  return ESP_OK;
}
void vTaskDelay(TickType_t ticks) {
  now += (uint64_t)ticks * 4;
  if (pending_dispatch && !block_dispatch) {
    pending_dispatch = false;
    mqtt_handler(mqtt_arg, "MQTT", MQTT_USER_EVENT, &pending_event);
  }
}
static bool clock_bad, ip_ready = true;
const char printer_bambu_ca[] =
    "-----BEGIN CERTIFICATE-----\ntest CA\n-----END CERTIFICATE-----\n";
int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
esp_err_t clock_service_wait(const char *host, uint32_t timeout,
                             bool (*cancel)(void *), void *ctx) {
  assert(!strcmp(host, "lan.example") && timeout <= 30000);
  ++clock_calls;
  if (cancel && cancel(ctx))
    return ESP_ERR_TIMEOUT;
  return clock_bad ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_mqtt_client_handle_t
esp_mqtt_client_init(const esp_mqtt_client_config_t *config) {
  recorded = *config;
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
  if (early_ack) {
    esp_mqtt_event_t event = {.client = c, .msg_id = 17};
    mqtt_handler(mqtt_arg, "MQTT", MQTT_EVENT_SUBSCRIBED, &event);
  }
  return 17;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic,
                            const char *data, int len, int qos, int retain) {
  assert(c == &client && !strcmp(topic, "device/SERIAL123/request"));
  assert(qos == 0 && !retain && len > 0);
  if (strstr(data, "pushall"))
    ++snapshot_request_count;
  else {
    assert(strstr(data, "resume") && !strstr(data, "file") &&
           !strstr(data, "ams"));
    ++resume_publish_count;
  }
  assert(esp_transport_write(recorded.network.transport, data, len, 10000) ==
         len);
  return 0;
}
esp_err_t
esp_event_handler_instance_register(esp_event_base_t b, int32_t id,
                                    esp_event_handler_t h, void *arg,
                                    esp_event_handler_instance_t *inst) {
  (void)b;
  (void)id;
  (void)h;
  (void)arg;
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
  return ip_ready ? (esp_netif_t *)1 : NULL;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *net, esp_netif_ip_info_t *ip) {
  (void)net;
  ip->ip.addr = 1;
  return ESP_OK;
}
static void event(int id, const char *json, bool retained) {
  esp_mqtt_event_t e = {
      .event_id = id, .client = &client, .msg_id = 17, .retain = retained};
  if (json) {
    e.topic = "device/SERIAL123/report";
    e.topic_len = (int)strlen(e.topic);
    e.data = (char *)json;
    e.data_len = e.total_data_len = (int)strlen(json);
  }
  mqtt_handler(mqtt_arg, "MQTT", id, &e);
}
static void connect_events(void) {
  event(MQTT_EVENT_CONNECTED, NULL, false);
  printer_link_tick(now);
  if (!early_ack)
    event(MQTT_EVENT_SUBSCRIBED, NULL, false);
  printer_link_tick(now);
}
static const char baseline[] =
    "{\"print\":{\"command\":\"push_status\",\"gcode_state\":\"PAUSE\",\"layer_"
    "num\":0,\"print_error\":0,\"hms\":[],\"gcode_start_time\":100,\"gcode_"
    "file\":\"job.3mf\"}}";
static void test_tls_identity_and_snapshot_cooldown(void) {
  native_ops = printer_transport_ops();
  printer_link_ops_t ops = *native_ops;
  ops.publish_live = interposed_publish;
  assert(printer_link_start_with_ops(&ops) == ESP_OK);
  printer_link_set_dispatch_guard(runtime_guard, NULL);
  printer_connection_config_t config = {.enabled = true,
                                        .ipv4 = "192.168.1.20",
                                        .serial = "SERIAL123",
                                        .access_code = "Secret",
                                        .ntp_host = "lan.example"};
  assert(printer_link_configure(&config) == ESP_OK);
  printer_link_tick(now);
  assert(recorded.broker.address.port == 8883);
  assert(recorded.broker.address.transport == MQTT_TRANSPORT_OVER_SSL);
  assert(!strcmp(recorded.broker.address.hostname, config.ipv4));
  assert(!recorded.broker.address.uri &&
         !strcmp(recorded.credentials.username, "bblp"));
  assert(!strcmp(recorded.credentials.authentication.password, "Secret"));
  assert(recorded.broker.verification.certificate == printer_bambu_ca);
  assert(!recorded.broker.verification.skip_cert_common_name_check);
  assert(!strcmp(recorded.broker.verification.common_name, config.serial));
  assert(!recorded.session.disable_clean_session &&
         recorded.network.disable_auto_reconnect);
  connect_events();
  assert(snapshot_request_count == 1);
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.startup_eligible);
  assert(printer_link_resume_once(1, &o.job, &o.history, o.connection_epoch,
                                  now + 1000) ==
         PRINTER_DISPATCH_SENT);
  assert(resume_publish_count == 1);
  assert(printer_link_resume_once(1, &o.job, &o.history, o.connection_epoch,
                                  now + 1000) ==
         PRINTER_DISPATCH_REJECTED);
  early_ack = false;
  event(MQTT_EVENT_DISCONNECTED, NULL, false);
  printer_link_snapshot(&o);
  assert(!o.connected);
  printer_link_tick(now);
  now = 1000;
  printer_link_tick(now);
  connect_events();
  assert(snapshot_request_count == 1);
  event(MQTT_EVENT_DISCONNECTED, NULL, false);
  printer_link_tick(now);
  now = 299999;
  printer_link_tick(now);
  connect_events();
  assert(snapshot_request_count == 1);
  event(MQTT_EVENT_DISCONNECTED, NULL, false);
  printer_link_tick(now);
  now = 300000;
  printer_link_tick(now);
  now += 1000;
  printer_link_tick(now);
  connect_events();
  assert(snapshot_request_count == 2);
}
static void test_overflow_invalidates_generation(void) {
  printer_observation_t o;
  event(MQTT_EVENT_DATA, baseline, true);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(!o.baseline_valid);
  uint32_t old = printer_link_generation();
  for (int i = 0; i < 8; ++i)
    event(MQTT_EVENT_DATA, baseline, false);
  printer_link_snapshot(&o);
  assert(!o.baseline_valid && !o.connected);
  printer_link_tick(now);
  assert(printer_link_generation() != old);
  printer_link_event_report(old, baseline, strlen(baseline), 0, now);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(!o.baseline_valid);
  printer_link_suspend(true);
  printer_link_tick(now);
  assert(aborted > 0);
  clock_bad = true;
  printer_link_suspend(false);
  now += 30000;
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(!o.startup_eligible);
  /* Missing clock / authentication errors never leave an eligible baseline. */
  unsigned calls = clock_calls;
  unsigned delays[] = {2000, 4000, 8000, 30000, 30000};
  for (unsigned i = 0; i < 5; ++i) {
    printer_link_tick(now);
    now += delays[i] - 1;
    printer_link_tick(now);
    assert(clock_calls == calls);
    ++now;
    printer_link_tick(now);
    assert(clock_calls == ++calls);
    printer_link_snapshot(&o);
    assert(!o.startup_eligible);
  }
  clock_bad = false;
  printer_link_tick(now);
  now += 30000;
  printer_link_tick(now);
  connect_events();
  event(MQTT_EVENT_ERROR, NULL, false);
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(!o.baseline_valid && !o.startup_eligible);
}
static void start_link_test(void) {
  native_ops = printer_transport_ops();
  assert(printer_link_start_with_ops(native_ops) == ESP_OK);
  const printer_connection_config_t config = {.enabled = true,
                                             .ipv4 = "192.168.1.20",
                                             .serial = "SERIAL123",
                                             .access_code = "Secret",
                                             .ntp_host = "lan.example"};
  assert(printer_link_configure(&config) == ESP_OK);
  printer_link_tick(now);
}
static void start_baseline_recovery_test(void) {
  start_link_test();
  connect_events();
  assert(snapshot_request_count == 1);
}
static const char decision_baseline[] =
    "{\"print\":{\"gcode_state\":\"PAUSE\",\"layer_num\":0,"
    "\"print_error\":0,\"hms\":[]}}";
static void test_initial_snapshot_includes_missing_identity(void) {
  early_ack = false;
  start_link_test();
  event(MQTT_EVENT_CONNECTED, NULL, false);
  printer_link_tick(now);
  /* SUBACK and a sparse report can both arrive before the next worker tick. */
  event(MQTT_EVENT_SUBSCRIBED, NULL, false);
  event(MQTT_EVENT_DATA, decision_baseline, false);
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.baseline_valid && !o.job.identity_valid);
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_start_time\":100,\"gcode_file\":\"job.3mf\"}}", false);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(o.startup_eligible);
  now = 300000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
}
static void test_missing_identity_recovers_after_cooldown(void) {
  start_baseline_recovery_test();
  event(MQTT_EVENT_DATA, decision_baseline, false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.baseline_valid && !o.job.identity_valid);
  now = 299999;
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
  now = 300000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  assert(snapshot_request_count == 2);
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_start_time\":100,\"gcode_file\":\"job.3mf\"}}", false);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(o.startup_eligible);
  now = 600000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  assert(snapshot_request_count == 2);
}
static void test_idle_identity_does_not_repeat_snapshots(void) {
  start_baseline_recovery_test();
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_state\":\"IDLE\",\"layer_num\":0,"
        "\"print_error\":0,\"hms\":[]}}", false);
  printer_link_tick(now);
  now = 300000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.state == PRINTER_RUN_IDLE && o.baseline_valid &&
         !o.job.identity_valid && snapshot_request_count == 1);
}
static void test_new_run_recovers_missing_baseline(void) {
  start_baseline_recovery_test();
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_state\":\"IDLE\",\"layer_num\":0,"
        "\"print_error\":0,\"hms\":[]}}", false);
  printer_link_tick(now);
  now = 1000;
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_state\":\"PREPARE\",\"gcode_start_time\":100,"
        "\"gcode_file\":\"job.3mf\"}}", false);
  printer_link_tick(now);
  now = 2000;
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_state\":\"PAUSE\",\"layer_num\":0}}", false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.fresh && !o.baseline_valid && !o.startup_eligible);
  for (now = 3000; now < 300000; now += 1000) {
    event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
    printer_link_tick(now);
    assert(snapshot_request_count == 1);
  }
  printer_link_tick(now);
  assert(snapshot_request_count == 2);
  event(MQTT_EVENT_DATA, "{\"print\":{\"print_error\":0,\"hms\":[]}}", false);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  assert(o.baseline_valid && o.startup_eligible);
  now = 600000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  assert(snapshot_request_count == 2);
}
static void reconnect_without_baseline(void) {
  start_baseline_recovery_test();
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  event(MQTT_EVENT_DISCONNECTED, NULL, false);
  printer_link_tick(now);
  now += 1000;
  printer_link_tick(now);
  connect_events();
  assert(snapshot_request_count == 1);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.connected && !o.baseline_valid);
}
static void test_reconnect_retries_after_snapshot_cooldown(void) {
  reconnect_without_baseline();
  now = 299999;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
  now = 300000;
  printer_link_tick(now);
  assert(snapshot_request_count == 2);
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.startup_eligible);
}
static void test_delta_baseline_avoids_another_snapshot(void) {
  reconnect_without_baseline();
  event(MQTT_EVENT_DATA,
        "{\"print\":{\"gcode_state\":\"PAUSE\",\"layer_num\":0,"
        "\"gcode_start_time\":100,\"gcode_file\":\"job.3mf\"}}", false);
  printer_link_tick(now);
  event(MQTT_EVENT_DATA, "{\"print\":{\"print_error\":0,\"hms\":[]}}", false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.startup_eligible);
  now = 300000;
  event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":60}}", false);
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
}
static void test_failed_snapshot_consumes_cooldown(void) {
  start_baseline_recovery_test();
  now = 300000;
  block_dispatch = true;
  printer_link_tick(now);
  assert(now == 301000 && snapshot_request_count == 1);
  block_dispatch = false;
  vTaskDelay(1); /* Deliver the expired event; it must not write later. */
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
  now = 599999;
  printer_link_tick(now);
  assert(snapshot_request_count == 1);
  now = 600000;
  printer_link_tick(now);
  assert(snapshot_request_count == 2);
}
/* Real coordinator, status parser, link and native send path; only chamber
 * hardware, storage and the MQTT/socket boundaries are synthetic. */
static app_status_t chamber;
static runtime_auto_lease_t chamber_lease;
static service_operation_t cycle_owner;
static const char *dispatch_history_case;
static bool inject_at_link, inject_history;
static unsigned dispatch_ticks;
static void intervening_reports(void) {
  assert(inject_history);
  inject_history = false;
  if (!strcmp(dispatch_history_case, "identity")) {
    event(MQTT_EVENT_DATA, "{\"print\":{\"gcode_file\":\"other.3mf\"}}", false);
    event(MQTT_EVENT_DATA, "{\"print\":{\"gcode_file\":\"job.3mf\"}}", false);
  } else if (!strcmp(dispatch_history_case, "hms")) {
    event(MQTT_EVENT_DATA, "{\"print\":{\"hms\":[{\"attr\":1}]}}", false);
    event(MQTT_EVENT_DATA, "{\"print\":{\"hms\":[]}}", false);
  } else if (!strcmp(dispatch_history_case, "error")) {
    event(MQTT_EVENT_DATA, "{\"print\":{\"print_error\":42}}", false);
    event(MQTT_EVENT_DATA, "{\"print\":{\"print_error\":0}}", false);
  } else {
    assert(!strcmp(dispatch_history_case, "benign"));
    event(MQTT_EVENT_DATA, "{\"print\":{\"bed_temper\":61}}", false);
  }
}
static esp_err_t chamber_acquire(uint64_t id, float target, uint64_t until,
                                 void *ctx) {
  (void)ctx;
  assert(target == 60);
  esp_err_t result = runtime_auto_lease_acquire(&chamber_lease, id, now, until);
  if (result == ESP_OK)
    chamber.enabled = true;
  return result;
}
static esp_err_t chamber_renew(uint64_t id, uint64_t until, void *ctx) {
  (void)ctx;
  return runtime_auto_lease_renew(&chamber_lease, id, now, until);
}
static void chamber_revoke(uint64_t id, void *ctx) {
  (void)ctx;
  runtime_auto_lease_revoke(&chamber_lease, id, RUNTIME_AUTO_OPERATOR_STOP);
  chamber.enabled = false;
  chamber_lease.status.teardown_applied = true;
}
static void chamber_snapshot(app_status_t *out, void *ctx) {
  (void)ctx;
  *out = chamber;
}
static void chamber_auto_snapshot(runtime_auto_status_t *out, void *ctx) {
  (void)ctx;
  (void)runtime_auto_lease_step(&chamber_lease, now);
  *out = chamber_lease.status;
}
static esp_err_t control_configure(const printer_connection_config_t *config,
                                   void *ctx) {
  (void)ctx;
  return printer_link_configure(config);
}
static void control_tick(uint64_t at, void *ctx) {
  (void)ctx;
  if (inject_history && !inject_at_link && ++dispatch_ticks == 2)
    intervening_reports();
  printer_link_tick(at);
}
static void control_snapshot(printer_observation_t *out, void *ctx) {
  (void)ctx;
  printer_link_snapshot(out);
}
static printer_dispatch_result_t control_resume(
    uint64_t id, const printer_job_key_t *job,
    const printer_safety_history_t *history, uint32_t epoch,
    uint64_t deadline, void *ctx) {
  (void)ctx;
  if (inject_history && inject_at_link)
    intervening_reports();
  return printer_link_resume_once(id, job, history, epoch, deadline);
}
static esp_err_t control_load(printer_connection_config_t *out, void *ctx) {
  (void)ctx;
  *out = (printer_connection_config_t){.enabled = true,
                                       .ipv4 = "192.168.1.20",
                                       .serial = "SERIAL123",
                                       .access_code = "Secret",
                                       .ntp_host = "lan.example"};
  return ESP_OK;
}
static esp_err_t control_save(const printer_connection_config_t *config,
                              void *ctx) {
  (void)config;
  (void)ctx;
  return ESP_OK;
}
static esp_err_t control_clear(void *ctx) {
  (void)ctx;
  return ESP_OK;
}
static bool control_begin(service_operation_t owner, void *ctx) {
  (void)ctx;
  if (cycle_owner)
    return false;
  cycle_owner = owner;
  return true;
}
static void control_end(service_operation_t owner, void *ctx) {
  (void)ctx;
  assert(cycle_owner == owner);
  cycle_owner = SERVICE_OPERATION_NONE;
}
static void control_random(void *out, size_t len, void *ctx) {
  (void)ctx;
  memset(out, 0x23, len);
}
static uint64_t control_now(void *ctx) {
  (void)ctx;
  return now;
}
static void control_refresh(void) {
  chamber.temperature.timestamp_ms = now;
  event(MQTT_EVENT_DATA, baseline, false);
  printer_control_poll();
}
static void test_dispatch_history(const char *kind, const char *boundary) {
  now = 1000;
  native_ops = printer_transport_ops();
  assert(printer_link_start_with_ops(native_ops) == ESP_OK);
  printer_link_set_dispatch_guard(runtime_guard, NULL);
  chamber = app_status_initial();
  chamber.target_c = 60;
  chamber.temperature = (temperature_sample_t){.valid = true,
                                              .consecutive_valid = 3,
                                              .timestamp_ms = now,
                                              .raw_c = 60,
                                              .filtered_c = 60};
  runtime_auto_lease_init(&chamber_lease);
  const printer_control_ops_t ops = {.heat_acquire = chamber_acquire,
                                     .heat_renew = chamber_renew,
                                     .heat_revoke = chamber_revoke,
                                     .heat_snapshot = chamber_snapshot,
                                     .auto_snapshot = chamber_auto_snapshot,
                                     .link_configure = control_configure,
                                     .link_tick = control_tick,
                                     .link_snapshot = control_snapshot,
                                     .link_resume = control_resume,
                                     .load_config = control_load,
                                     .save_config = control_save,
                                     .clear_config = control_clear,
                                     .gate_begin = control_begin,
                                     .gate_end = control_end,
                                     .now_ms = control_now,
                                     .random_bytes = control_random,
                                     .resume_qualified = true};
  assert(printer_control_start_with_ops(&ops) == ESP_OK);
  printer_control_poll();
  connect_events();
  control_refresh();
  printer_control_status_t status;
  printer_control_get_status(&status);
  assert(status.can_arm);
  printer_arm_request_t request = {.initial_pause_confirmed = true};
  strcpy(request.arm_token, status.cycle.arm_token);
  assert(printer_control_arm(&request) == ESP_OK);
  control_refresh();
  for (unsigned i = 0; i < 29; ++i) {
    now += 1000;
    control_refresh();
  }
  now += 1000;
  dispatch_history_case = kind;
  inject_at_link = !strcmp(boundary, "link");
  dispatch_ticks = 0;
  inject_history = true;
  control_refresh();
  assert(!inject_history);
  printer_observation_t observed;
  printer_link_snapshot(&observed);
  assert(observed.startup_eligible && !observed.blocking_error);
  bool benign = !strcmp(kind, "benign");
  assert(resume_publish_count == (benign ? 1U : 0U));
  if (!benign) {
    assert(!chamber_lease.status.active);
    now += 100;
    control_refresh();
    assert(cycle_owner == SERVICE_OPERATION_NONE);
  }
}
int main(int argc, char **argv) {
  if (argc == 3 && !strncmp(argv[1], "dispatch_", 9)) {
    test_dispatch_history(argv[1] + 9, argv[2]);
    return 0;
  }
  if (argc == 2) {
    if (!strcmp(argv[1], "baseline_new_run"))
      test_new_run_recovers_missing_baseline();
    else if (!strcmp(argv[1], "baseline_reconnect"))
      test_reconnect_retries_after_snapshot_cooldown();
    else if (!strcmp(argv[1], "baseline_delta"))
      test_delta_baseline_avoids_another_snapshot();
    else if (!strcmp(argv[1], "baseline_failed_request"))
      test_failed_snapshot_consumes_cooldown();
    else if (!strcmp(argv[1], "baseline_initial_identity"))
      test_initial_snapshot_includes_missing_identity();
    else if (!strcmp(argv[1], "baseline_identity"))
      test_missing_identity_recovers_after_cooldown();
    else if (!strcmp(argv[1], "baseline_idle"))
      test_idle_identity_does_not_repeat_snapshots();
    else
      assert(0);
    return 0;
  }
  assert(argc == 1);
  (void)unused;
  test_tls_identity_and_snapshot_cooldown();
  test_overflow_invalidates_generation();
  now += 30000;
  printer_link_tick(now);
  connect_events();
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  printer_observation_t o;
  printer_link_snapshot(&o);
  assert(o.startup_eligible);
  block_dispatch = true;
  uint64_t before = now;
  unsigned abort_count = aborted;
  assert(printer_link_resume_once(2, &o.job, &o.history, o.connection_epoch,
                                  now + 1000) ==
         PRINTER_DISPATCH_REJECTED);
  assert(now == before + 1000 && resume_publish_count == 1);
  block_dispatch = false;
  vTaskDelay(1);
  printer_link_tick(now);
  assert(aborted == abort_count + 1 && !pending_dispatch &&
         resume_publish_count == 1);
  now += 30000;
  printer_link_tick(now);
  connect_events();
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  runtime_permitted = false;
  assert(printer_link_resume_once(3, &o.job, &o.history, o.connection_epoch,
                                  now + 1000) ==
         PRINTER_DISPATCH_REJECTED);
  assert(resume_publish_count == 1);
  runtime_permitted = true;
  printer_link_tick(now);
  now += 30000;
  printer_link_tick(now);
  connect_events();
  event(MQTT_EVENT_DATA, baseline, false);
  printer_link_tick(now);
  printer_link_snapshot(&o);
  report_before_publish = true;
  assert(o.startup_eligible);
  unsigned before_publish = resume_publish_count;
  assert(printer_link_resume_once(4, &o.job, &o.history, o.connection_epoch,
                                  now + 1000) ==
         PRINTER_DISPATCH_REJECTED);
  assert(!report_before_publish && resume_publish_count == before_publish);
  puts("printer link: ok");
  return 0;
}
