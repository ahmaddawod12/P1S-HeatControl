#include "cJSON.h"
#include "firmware_api.h"
#include "printer_api.h"
#include "printer_api_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static httpd_uri_t routes[5];
static unsigned route_count, arm_calls, stop_calls, config_calls;
static const char *input, *type = "application/json";
static bool trusted = true;
static char status_line[64], response[2048];
static unsigned allocation, fail_allocation, live_allocations;
static void *limited_malloc(size_t n) {
  if (++allocation == fail_allocation)
    return NULL;
  void *p = malloc(n);
  if (p)
    ++live_allocations;
  return p;
}
static void limited_free(void *p) {
  if (p) {
    assert(live_allocations);
    --live_allocations;
    free(p);
  }
}
static void test_status_allocation_failure_releases_memory(void) {
  const printer_control_status_t status = {0};
  cJSON_Hooks hooks = {limited_malloc, limited_free};
  cJSON_InitHooks(&hooks);
  for (unsigned i = 1; i < 100; ++i) {
    allocation = 0;
    fail_allocation = i;
    esp_err_t e =
        printer_api_status_json(&status, "csrf", response, sizeof(response));
    assert(e == ESP_OK || !response[0]);
    assert(live_allocations == 0);
  }
  cJSON_InitHooks(NULL);
}
static printer_control_status_t status;
esp_err_t httpd_register_uri_handler(httpd_handle_t s, const httpd_uri_t *r) {
  assert(s && route_count < 5);
  routes[route_count++] = *r;
  return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *v) {
  (void)r;
  strcpy(status_line, v);
  return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *v) {
  (void)r;
  assert(!strcmp(v, "application/json"));
  return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *v) {
  (void)r;
  assert(!strcmp(key, "Cache-Control") && !strcmp(v, "no-store"));
  return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *v) {
  (void)r;
  assert(strlen(v) < sizeof(response));
  strcpy(response, v);
  return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *v, int n) {
  (void)r;
  assert(!n && !v);
  response[0] = 0;
  return ESP_OK;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *key) {
  (void)r;
  assert(!strcmp(key, "Content-Type"));
  return strlen(type);
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *key,
                                      char *out, size_t cap) {
  (void)r;
  assert(!strcmp(key, "Content-Type") && strlen(type) < cap);
  strcpy(out, type);
  return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *out, size_t n) {
  (void)r;
  memcpy(out, input, n);
  input += n;
  return (int)n;
}
bool firmware_api_request_trusted(httpd_req_t *r) {
  (void)r;
  return trusted;
}
const char *firmware_api_csrf_token(void) {
  return "abcdefabcdefabcdefabcdefabcdefab";
}
static void get(printer_control_status_t *out) { *out = status; }
static esp_err_t arm(const printer_arm_request_t *r) {
  assert(r->initial_pause_confirmed);
  return arm_calls++ ? ESP_ERR_NOT_ALLOWED : ESP_OK;
}
static esp_err_t stop(void) {
  ++stop_calls;
  return ESP_OK;
}
static esp_err_t config(const printer_config_request_t *r) {
  assert(r->access_code_present);
  ++config_calls;
  return ESP_OK;
}
static esp_err_t forget(void) { return ESP_OK; }
static void request(httpd_method_t method, const char *path, const char *body,
                    int expected) {
  for (unsigned i = 0; i < route_count; ++i)
    if (routes[i].method == method && !strcmp(path, routes[i].uri)) {
      input = body;
      httpd_req_t r = {.content_len = strlen(body), .method = method};
      assert(routes[i].handler(&r) == ESP_OK);
      assert(atoi(status_line) == expected);
      return;
    }
  assert(0);
}
static void test_printer_api_rejects_ambiguous_body(void) {
  printer_arm_request_t r;
  const char *valid = "{\"arm_token\":\"0123456789abcdef0123456789abcdef\","
                      "\"initial_pause_confirmed\":true}";
  assert(printer_api_parse_arm(valid, strlen(valid), &r));
  const char duplicate_json[] =
      "{\"arm_token\":\"0123456789abcdef0123456789abcdef\",\"initial_pause_"
      "confirmed\":true,\"arm_token\":\"0123456789abcdef0123456789abcdef\"}";
  assert(
      !printer_api_parse_arm(duplicate_json, sizeof(duplicate_json) - 1, &r));
  const char *bad[] = {"{}",
                       "[]",
                       "{\"arm_token\":3}",
                       "{\"initial_pause_confirmed\":false}",
                       "{\"unknown\":true}",
                       "{\"arm_token\":\"\\u0000\"}",
                       "{\"arm_token\":{}}"};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
    assert(!printer_api_parse_arm(bad[i], strlen(bad[i]), &r));
  char padded[1026];
  memset(padded, ' ', sizeof(padded));
  memcpy(padded, valid, strlen(valid));
  assert(printer_api_parse_arm(padded, 1024, &r));
  assert(!printer_api_parse_arm(padded, 1025, &r));
  padded[strlen(valid)] = 0;
  assert(!printer_api_parse_arm(padded, 1024, &r));
  snprintf(padded, sizeof(padded), "%s {}", valid);
  assert(!printer_api_parse_arm(padded, strlen(padded), &r));
  printer_config_request_t c;
  const char good[] =
      "{\"enabled\":true,\"ipv4\":\"192.168.1.20\",\"serial\":\"SERIAL123\","
      "\"access_code\":\"SecretAccess\",\"ntp_host\":\"lan.example\"}";
  assert(printer_api_parse_config(good, sizeof(good) - 1, &c) &&
         c.access_code_present);
  const char omitted[] =
      "{\"enabled\":true,\"ipv4\":\"192.168.1.20\",\"serial\":\"SERIAL123\"}";
  assert(printer_api_parse_config(omitted, sizeof(omitted) - 1, &c) &&
         !c.access_code_present && !c.value.access_code[0]);
  const char wrong[] = "{\"enabled\":true,\"ipv4\":\"192.168.001.20\","
                       "\"serial\":\"SERIAL123\",\"access_code\":\"Secret\"}";
  assert(!printer_api_parse_config(wrong, sizeof(wrong) - 1, &c));
}
static void test_printer_status_never_contains_code(void) {
  const char *secret_access_code = "SecretAccess";
  status.configured = status.access_code_set = true;
  strcpy(status.serial, "SERIAL123");
  assert(printer_api_status_json(&status, firmware_api_csrf_token(), response,
                                 sizeof(response)) == ESP_OK);
  assert(strstr(response, secret_access_code) == NULL &&
         strstr(response, "access_code_set"));
  cJSON *j = cJSON_Parse(response);
  assert(j);
  cJSON_Delete(j);
  assert(printer_api_status_json(&status, "csrf", response, 8) ==
             ESP_ERR_INVALID_SIZE &&
         !response[0]);
  memset(status.observation.job.file, 1,
         sizeof(status.observation.job.file) - 1);
  esp_err_t e =
      printer_api_status_json(&status, "csrf", response, sizeof(response));
  assert(e == ESP_ERR_INVALID_SIZE || e == ESP_OK);
  if (e == ESP_OK) {
    j = cJSON_Parse(response);
    assert(j);
    cJSON_Delete(j);
  } else
    assert(!response[0]);
  status.observation.job.file[0] = 0;
}
int main(void) {
  test_status_allocation_failure_releases_memory();
  test_printer_api_rejects_ambiguous_body();
  test_printer_status_never_contains_code();
  web_server_ops_t ops = {.get_printer_status = get,
                          .request_printer_config = config,
                          .forget_printer_config = forget,
                          .arm_printer_cycle = arm,
                          .stop_printer_cycle = stop};
  assert(printer_api_register((void *)1, &ops) == ESP_OK && route_count == 5);
  const char *body = "{\"arm_token\":\"0123456789abcdef0123456789abcdef\","
                     "\"initial_pause_confirmed\":true}";
  request(HTTP_GET, "/api/printer", "", 200);
  trusted = false;
  request(HTTP_POST, "/api/printer/cycle", body, 403);
  assert(!arm_calls);
  trusted = true;
  type = "text/plain";
  request(HTTP_POST, "/api/printer/cycle", body, 400);
  type = "application/json";
  request(HTTP_POST, "/api/printer/cycle", body, 202);
  request(HTTP_POST, "/api/printer/cycle", body, 409);
  request(HTTP_POST, "/api/printer/cycle/stop", "{}", 204);
  request(HTTP_DELETE, "/api/printer/config", "", 204);
  assert(stop_calls == 1);
  request(HTTP_PUT, "/api/printer/config",
          "{\"enabled\":true,\"ipv4\":\"192.168.1.20\",\"serial\":"
          "\"SERIAL123\",\"access_code\":\"SecretAccess\"}",
          202);
  assert(config_calls == 1);
  routes[0] = (httpd_uri_t){0};
  route_count = 0;
  assert(printer_api_register((void *)1, &(web_server_ops_t){0}) == ESP_OK);
  request(HTTP_GET, "/api/printer", "", 503);
  puts("printer API: ok");
  return 0;
}
