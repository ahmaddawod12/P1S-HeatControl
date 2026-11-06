#include "printer_api.h"
#include "cJSON.h"
#include "firmware_api.h"
#include "http_request.h"
#include "printer_api_policy.h"
#include <stdlib.h>
#include <string.h>
static web_server_ops_t s_ops;
static esp_err_t response(httpd_req_t *req, const char *status,
                          const char *json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return json ? httpd_resp_sendstr(req, json) : httpd_resp_send(req, NULL, 0);
}
static esp_err_t result(httpd_req_t *req, esp_err_t err, bool accepted) {
  if (err == ESP_OK)
    return response(req, accepted ? "202 Accepted" : "204 No Content",
                    accepted ? "{\"accepted\":true}" : NULL);
  if (err == ESP_ERR_INVALID_ARG)
    return response(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");
  if (err == ESP_ERR_NOT_ALLOWED) {
    printer_control_status_t *s = calloc(1, sizeof(*s));
    char body[160];
    cJSON *root = cJSON_CreateObject();
    if (s && s_ops.get_printer_status)
      s_ops.get_printer_status(s);
    bool ok =
        root && cJSON_AddStringToObject(root, "error", "printer_conflict") &&
        cJSON_AddStringToObject(
            root, "reason", s && s->reason[0] ? s->reason : "operation_busy") &&
        cJSON_PrintPreallocated(root, body, sizeof(body), false);
    cJSON_Delete(root);
    free(s);
    return response(req, "409 Conflict",
                    ok ? body : "{\"error\":\"printer_conflict\"}");
  }
  return response(req, "503 Service Unavailable",
                  "{\"error\":\"unavailable\"}");
}
static esp_err_t get_printer(httpd_req_t *req) {
  if (!http_request_require_empty_body(req)) return ESP_FAIL;
  if (!s_ops.get_printer_status)
    return result(req, ESP_ERR_INVALID_STATE, false);
  printer_control_status_t *s = calloc(1, sizeof(*s));
  char *body = malloc(PRINTER_API_MAX_RESPONSE);
  if (!s || !body) {
    free(s);
    free(body);
    return result(req, ESP_ERR_NO_MEM, false);
  }
  s_ops.get_printer_status(s);
  esp_err_t err = printer_api_status_json(s, firmware_api_csrf_token(), body,
                                          PRINTER_API_MAX_RESPONSE);
  esp_err_t sent =
      err == ESP_OK ? response(req, "200 OK", body) : result(req, err, false);
  free(body);
  free(s);
  return sent;
}
static esp_err_t read_body(httpd_req_t *req, char **out) {
  if (req->content_len > PRINTER_API_MAX_BODY)
    return ESP_ERR_INVALID_SIZE;
  if (req->content_len) {
    char type[64];
    size_t n = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (!n || n >= sizeof(type) ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) !=
            ESP_OK ||
        (strcmp(type, "application/json") &&
         strncmp(type, "application/json;", 17)))
      return ESP_ERR_INVALID_ARG;
  }
  char *body = malloc(req->content_len + 1);
  if (!body)
    return ESP_ERR_NO_MEM;
  const esp_err_t err = http_request_receive_body(req, body, req->content_len + 1);
  if (err != ESP_OK) {
    memset(body, 0, req->content_len);
    free(body);
    return err;
  }
  *out = body;
  return ESP_OK;
}
enum { CONFIG_SAVE, CONFIG_FORGET, CYCLE_ARM, CYCLE_STOP };
static esp_err_t mutate(httpd_req_t *req, unsigned operation) {
  if (!firmware_api_request_trusted(req))
    return http_request_close_json(req, "403 Forbidden", "{\"error\":\"request_origin\"}");
  char *body = NULL;
  const esp_err_t read_err = read_body(req, &body);
  if (read_err == ESP_ERR_TIMEOUT)
    return http_request_close_json(req, "408 Request Timeout", "{\"error\":\"body_timeout\"}");
  if (read_err == ESP_ERR_NO_MEM)
    return http_request_close_json(req, "503 Service Unavailable", "{\"error\":\"unavailable\"}");
  if (read_err != ESP_OK)
    return http_request_close_json(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");
  esp_err_t err = ESP_ERR_INVALID_ARG;
  if (operation == CONFIG_SAVE) {
    printer_config_request_t request;
    if (printer_api_parse_config(body, req->content_len, &request))
      err = s_ops.request_printer_config
                ? s_ops.request_printer_config(&request)
                : ESP_ERR_INVALID_STATE;
  } else if (operation == CYCLE_ARM) {
    printer_arm_request_t request;
    if (printer_api_parse_arm(body, req->content_len, &request))
      err = s_ops.arm_printer_cycle ? s_ops.arm_printer_cycle(&request)
                                    : ESP_ERR_INVALID_STATE;
  } else if (printer_api_empty_body(body, req->content_len)) {
    if (operation == CONFIG_FORGET)
      err = s_ops.forget_printer_config ? s_ops.forget_printer_config()
                                        : ESP_ERR_INVALID_STATE;
    else
      err = s_ops.stop_printer_cycle ? s_ops.stop_printer_cycle()
                                     : ESP_ERR_INVALID_STATE;
  }
  memset(body, 0, req->content_len);
  free(body);
  return result(req, err, operation == CONFIG_SAVE || operation == CYCLE_ARM);
}
static esp_err_t save(httpd_req_t *r) { return mutate(r, CONFIG_SAVE); }
static esp_err_t forget(httpd_req_t *r) { return mutate(r, CONFIG_FORGET); }
static esp_err_t arm(httpd_req_t *r) { return mutate(r, CYCLE_ARM); }
static esp_err_t stop(httpd_req_t *r) { return mutate(r, CYCLE_STOP); }
esp_err_t printer_api_register(httpd_handle_t server,
                               const web_server_ops_t *ops) {
  if (!server || !ops)
    return ESP_ERR_INVALID_ARG;
  s_ops = *ops;
  const httpd_uri_t routes[] = {
      {.uri = "/api/printer", .method = HTTP_GET, .handler = get_printer},
      {.uri = "/api/printer/config", .method = HTTP_PUT, .handler = save},
      {.uri = "/api/printer/config", .method = HTTP_DELETE, .handler = forget},
      {.uri = "/api/printer/cycle", .method = HTTP_POST, .handler = arm},
      {.uri = "/api/printer/cycle/stop", .method = HTTP_POST, .handler = stop}};
  for (unsigned i = 0; i < sizeof(routes) / sizeof(*routes); ++i) {
    esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
    if (err != ESP_OK)
      return err;
  }
  return ESP_OK;
}
