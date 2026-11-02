#include "device_config.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Only replace the external flash boundary; exercise the real record codec,
 * validation, seed precedence and failure propagation. */
typedef struct {
  char key[16];
  unsigned char bytes[384];
  size_t size;
} record_t;
static record_t records[6], pending;
static char erase_key[16];
static esp_err_t init_result, commit_result;

esp_err_t nvs_flash_init_partition(const char *label) {
  assert(strcmp(label, "device_cfg") == 0);
  return init_result;
}
esp_err_t nvs_open_from_partition(const char *p, const char *ns,
                                  nvs_open_mode_t m, nvs_handle_t *h) {
  assert(strcmp(p, "device_cfg") == 0);
  assert(strcmp(ns, "p1s_device") == 0);
  *h = m + 1;
  return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out,
                       size_t *size) {
  (void)h;
  for (unsigned i = 0; i < 6; ++i)
    if (strcmp(records[i].key, key) == 0) {
      if (out && *size < records[i].size)
        return ESP_ERR_NVS_INVALID_LENGTH;
      if (out)
        memcpy(out, records[i].bytes, records[i].size);
      *size = records[i].size;
      return ESP_OK;
    }
  return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value,
                       size_t size) {
  assert(h == NVS_READWRITE + 1);
  assert(size <= sizeof(pending.bytes));
  strcpy(pending.key, key);
  memcpy(pending.bytes, value, size);
  pending.size = size;
  return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
  (void)h;
  for (unsigned i = 0; i < 6; i++)
    if (!strcmp(records[i].key, key)) {
      strcpy(erase_key, key);
      return ESP_OK;
    }
  return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h) {
  (void)h;
  if (commit_result != ESP_OK)
    return commit_result;
  if (erase_key[0]) {
    for (unsigned i = 0; i < 6; i++)
      if (!strcmp(records[i].key, erase_key))
        memset(&records[i], 0, sizeof(records[i]));
    return ESP_OK;
  }
  for (unsigned i = 0; i < 6; ++i)
    if (records[i].key[0] == 0 || strcmp(records[i].key, pending.key) == 0) {
      records[i] = pending;
      return ESP_OK;
    }
  return ESP_FAIL;
}
void nvs_close(nvs_handle_t h) {
  (void)h;
  memset(&pending, 0, sizeof(pending));
  erase_key[0] = 0;
}

static void test_saved_wifi_wins_over_bootstrap(void)
{
    device_wifi_credentials_t saved = {"first-network", "first-password"};
    device_wifi_credentials_t changed = {"different-network", "different-password"}, loaded;
    assert(device_config_init() == ESP_OK);
    assert(device_config_seed_wifi(&saved) == ESP_OK);
    assert(device_config_seed_wifi(&changed) == ESP_OK);
    assert(device_config_init() == ESP_OK);
    assert(device_config_load_wifi(&loaded) == ESP_OK);
    assert(strcmp(loaded.ssid, "first-network") == 0);
    assert(strcmp(loaded.password, "first-password") == 0);
}
static void test_empty_release_reuses_saved_wifi(void)
{
    device_wifi_credentials_t empty = {0}, loaded;
    assert(device_config_seed_wifi(&empty) == ESP_OK);
    assert(device_config_load_wifi(&loaded) == ESP_OK);
    assert(strcmp(loaded.ssid, "first-network") == 0);
}
static void test_attempt_record_does_not_change_wifi(void)
{
    firmware_attempt_t a = { .format = 1, .id = 7, .previous = "1.0.0",
        .candidate = "1.0.1", .phase = FIRMWARE_REBOOTING, .result = FIRMWARE_RESULT_NONE }, loaded;
    assert(device_config_save_attempt(&a) == ESP_OK);
    assert(device_config_load_attempt(&loaded) == ESP_OK);
    assert(loaded.id == 7 && strcmp(loaded.previous, "1.0.0") == 0);
    device_wifi_credentials_t wifi;
    assert(device_config_load_wifi(&wifi) == ESP_OK);
    assert(strcmp(wifi.ssid, "first-network") == 0);
    commit_result = ESP_FAIL;
    a.id = 8;
    assert(device_config_save_attempt(&a) == ESP_FAIL);
    assert(device_config_load_attempt(&loaded) == ESP_OK && loaded.id == 7);
    commit_result = ESP_OK;
}
static void test_bad_or_future_config_is_not_erased(void)
{
    records[0].bytes[0] = 2;
    const record_t before = records[0];
    device_wifi_credentials_t seed = {"replacement", "replacement-password"};
    assert(device_config_seed_wifi(&seed) != ESP_OK);
    assert(device_config_save_wifi(&seed) != ESP_OK);
    assert(memcmp(&before, &records[0], sizeof(before)) == 0);
    init_result = ESP_FAIL;
    assert(device_config_init() == ESP_FAIL);
    assert(memcmp(&before, &records[0], sizeof(before)) == 0);
}
static void test_wifi_replace_preserves_exact_bytes_and_failed_commit(void)
{
    device_wifi_credentials_t changed = {" new \\\"network\\\" ", " password\\with\\spaces "}, loaded;
    assert(device_config_save_wifi(&changed) == ESP_OK);
    assert(device_config_load_wifi(&loaded) == ESP_OK);
    assert(strcmp(loaded.ssid, changed.ssid) == 0);
    assert(strcmp(loaded.password, changed.password) == 0);
    firmware_attempt_t attempt = {.format=1,.id=9,.previous="1.0.4",.candidate="1.0.5"};
    assert(device_config_save_attempt(&attempt) == ESP_OK);
    device_wifi_credentials_t next = {"next", "next-password"};
    commit_result = ESP_FAIL;
    assert(device_config_save_wifi(&next) == ESP_FAIL);
    assert(device_config_load_wifi(&loaded) == ESP_OK);
    assert(strcmp(loaded.ssid, changed.ssid) == 0);
    commit_result = ESP_OK;
    next = (device_wifi_credentials_t){"first-network", "first-password"};
    assert(device_config_save_wifi(&next) == ESP_OK);
    assert(device_config_load_attempt(&attempt) == ESP_OK && attempt.id == 9);
    next = (device_wifi_credentials_t){0};
    assert(device_config_save_wifi(&next) == ESP_ERR_INVALID_ARG);
}
static void test_bootstrap_confirmation_requires_a_committed_record(void)
{
    assert(device_config_bootstrap_confirmed() == ESP_ERR_NOT_FOUND);
    commit_result = ESP_FAIL;
    assert(device_config_confirm_bootstrap() == ESP_FAIL);
    assert(device_config_bootstrap_confirmed() == ESP_ERR_NOT_FOUND);
    commit_result = ESP_OK;
    assert(device_config_confirm_bootstrap() == ESP_OK);
    assert(device_config_init() == ESP_OK);
    assert(device_config_bootstrap_confirmed() == ESP_OK);
    for (unsigned i=0; i<4; ++i) if (strcmp(records[i].key,"boot_ok_v1")==0) {
        records[i].bytes[0]=2;
        const record_t before=records[i];
        assert(device_config_confirm_bootstrap() != ESP_OK);
        assert(memcmp(&before,&records[i],sizeof(before))==0);
    }
}
int main(void) {
  test_saved_wifi_wins_over_bootstrap();
  test_empty_release_reuses_saved_wifi();
  test_wifi_replace_preserves_exact_bytes_and_failed_commit();
  test_attempt_record_does_not_change_wifi();
  test_bootstrap_confirmation_requires_a_committed_record();
  printer_connection_config_t p = {.enabled = true,
                                   .ipv4 = "192.168.1.9",
                                   .serial = "ABC123",
                                   .access_code = " exact code ",
                                   .ntp_host = "pool.ntp.org"},
                              loaded;
  assert(device_config_load_printer(&loaded) == ESP_ERR_NOT_FOUND);
  assert(printer_config_valid(&p));
  assert(device_config_save_printer(&p) == ESP_OK);
  assert(device_config_load_printer(&loaded) == ESP_OK &&
         !strcmp(loaded.access_code, " exact code "));
  assert(!strcmp(loaded.serial, "ABC123"));
  commit_result = ESP_FAIL;
  strcpy(p.serial, "CHANGED");
  assert(device_config_save_printer(&p) == ESP_FAIL);
  assert(device_config_load_printer(&loaded) == ESP_OK &&
         !strcmp(loaded.serial, "ABC123"));
  assert(device_config_clear_printer() == ESP_FAIL);
  commit_result = ESP_OK;
  for (unsigned i = 0; i < 6; i++)
    if (!strcmp(records[i].key, "printer_v1"))
      records[i].bytes[0] = 2;
  assert(device_config_load_printer(&loaded) == ESP_ERR_INVALID_RESPONSE);
  assert(device_config_save_printer(&p) == ESP_ERR_INVALID_RESPONSE);
  assert(device_config_clear_printer() == ESP_OK &&
         device_config_clear_printer() == ESP_OK);
  device_wifi_credentials_t preserved;
  assert(device_config_load_wifi(&preserved) == ESP_OK &&
         !strcmp(preserved.ssid, "first-network"));
  const char *bad_ip[] = {"1.2.3", "256.1.2.3", "1.2.3.4suffix", "01.2.3.4",
                          "1.2.3.-1"};
  for (unsigned i = 0; i < sizeof(bad_ip) / sizeof(*bad_ip); i++) {
    strcpy(p.ipv4, bad_ip[i]);
    assert(!printer_config_valid(&p));
  }
  strcpy(p.ipv4, "192.168.1.9");
  strcpy(p.ntp_host, "a..b");
  assert(!printer_config_valid(&p));
  strcpy(p.ntp_host, "-bad.example");
  assert(!printer_config_valid(&p));
  strcpy(p.ntp_host, "time.local");
  assert(printer_config_valid(&p));
  test_bad_or_future_config_is_not_erased();
  puts("device configuration passed");
  return 0;
}
