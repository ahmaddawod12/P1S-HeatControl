/* Policy tests use real configuration validation, never native persistence. */
#include "nvs.h"
#include "nvs_flash.h"
#include <assert.h>
esp_err_t nvs_flash_init_partition(const char *a) {
  (void)a;
  assert(0);
  return ESP_FAIL;
}
esp_err_t nvs_open_from_partition(const char *a, const char *b,
                                  nvs_open_mode_t c, nvs_handle_t *d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  assert(0);
  return ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t a, const char *b, void *c, size_t *d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  assert(0);
  return ESP_FAIL;
}
esp_err_t nvs_set_blob(nvs_handle_t a, const char *b, const void *c, size_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  assert(0);
  return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t a) {
  (void)a;
  assert(0);
  return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t a, const char *b) {
  (void)a;
  (void)b;
  assert(0);
  return ESP_FAIL;
}
void nvs_close(nvs_handle_t a) {
  (void)a;
  assert(0);
}
