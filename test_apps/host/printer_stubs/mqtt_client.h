#pragma once
#include "esp_event.h"
#include <stdbool.h>
struct esp_mqtt_client {
  int unused;
};
typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
enum {
  MQTT_TRANSPORT_OVER_SSL = 2,
  MQTT_EVENT_CONNECTED = 1,
  MQTT_EVENT_DISCONNECTED,
  MQTT_EVENT_SUBSCRIBED,
  MQTT_EVENT_DATA,
  MQTT_EVENT_ERROR
};
typedef struct {
  struct {
    struct {
      const char *hostname, *uri;
      int transport, port;
    } address;
    struct {
      const char *certificate, *common_name;
      esp_err_t (*crt_bundle_attach)(void *);
      bool skip_cert_common_name_check;
    } verification;
  } broker;
  struct {
    const char *username, *client_id;
    struct {
      const char *password;
    } authentication;
  } credentials;
  struct {
    bool disable_clean_session;
    int keepalive;
  } session;
  struct {
    int timeout_ms;
    bool disable_auto_reconnect;
  } network;
  struct {
    int size, out_size;
  } buffer;
  struct {
    int stack_size, priority;
  } task;
} esp_mqtt_client_config_t;
typedef enum {
  MQTT_ERROR_TYPE_NONE = 0,
  MQTT_ERROR_TYPE_SUBSCRIBE_FAILED = 3
} esp_mqtt_error_type_t;
typedef struct {
  esp_mqtt_error_type_t error_type;
} esp_mqtt_error_codes_t;
typedef struct {
  int event_id, msg_id;
  esp_mqtt_client_handle_t client;
  char *topic, *data;
  int topic_len, data_len, current_data_offset, total_data_len;
  bool retain;
  esp_mqtt_error_codes_t *error_handle;
  void *user_context;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int32_t,
                                         esp_event_handler_t, void *);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *,
                            const char *, int, int, int);
