#include "device_config.h"
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include "nvs.h"
#include "nvs_flash.h"

#define DEVICE_PARTITION "device_cfg"
#define DEVICE_NAMESPACE "p1s_device"
#define WIFI_BYTES 101U
#define ATTEMPT_BYTES 80U

static atomic_bool s_ready;
static uint32_t get_u32(const uint8_t *p)
{ return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static void put_u32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t) (v >> (8U * i)); }
static bool text_fits(const char *s, size_t capacity)
{ return memchr(s, '\0', capacity) != NULL; }
static bool wifi_valid(const device_wifi_credentials_t *wifi)
{
    if (!wifi || !text_fits(wifi->ssid, sizeof(wifi->ssid)) ||
        !text_fits(wifi->password, sizeof(wifi->password)) || wifi->ssid[0] == 0) return false;
    const size_t n = strlen(wifi->password);
    return n == 0 || (n >= 8 && n <= 63);
}

esp_err_t device_config_init(void)
{
    const esp_err_t err = nvs_flash_init_partition(DEVICE_PARTITION);
    atomic_store(&s_ready, err == ESP_OK);
    return err;
}

static esp_err_t read_record(const char *key, uint8_t *bytes, size_t expected)
{
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(DEVICE_PARTITION, DEVICE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    size_t size = expected;
    err = nvs_get_blob(handle, key, bytes, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    return size == expected && get_u32(bytes) == 1 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t write_record(const char *key, const uint8_t *bytes, size_t size)
{
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(DEVICE_PARTITION, DEVICE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, key, bytes, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t device_config_bootstrap_confirmed(void)
{
    uint8_t bytes[8];
    const esp_err_t err = read_record("boot_ok_v1", bytes, sizeof(bytes));
    if (err != ESP_OK) return err;
    return get_u32(bytes + 4) == 1 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t device_config_confirm_bootstrap(void)
{
    const esp_err_t existing = device_config_bootstrap_confirmed();
    if (existing == ESP_OK) return ESP_OK;
    if (existing != ESP_ERR_NOT_FOUND) return existing;
    uint8_t bytes[8] = {0}; put_u32(bytes, 1); put_u32(bytes + 4, 1);
    return write_record("boot_ok_v1", bytes, sizeof(bytes));
}

esp_err_t device_config_load_wifi(device_wifi_credentials_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t bytes[WIFI_BYTES];
    esp_err_t err = read_record("wifi_v1", bytes, sizeof(bytes));
    if (err != ESP_OK) return err;
    device_wifi_credentials_t wifi;
    memcpy(wifi.ssid, bytes + 4, 33);
    memcpy(wifi.password, bytes + 37, 64);
    if (!wifi_valid(&wifi)) return ESP_ERR_INVALID_RESPONSE;
    *out = wifi;
    return ESP_OK;
}

static esp_err_t save_wifi_record(const device_wifi_credentials_t *credentials)
{
    if (!wifi_valid(credentials)) return ESP_ERR_INVALID_ARG;
    uint8_t bytes[WIFI_BYTES] = {0};
    put_u32(bytes, 1);
    memcpy(bytes + 4, credentials->ssid, strlen(credentials->ssid));
    memcpy(bytes + 37, credentials->password, strlen(credentials->password));
    return write_record("wifi_v1", bytes, sizeof(bytes));
}

esp_err_t device_config_seed_wifi(const device_wifi_credentials_t *seed)
{
    if (!seed) return ESP_ERR_INVALID_ARG;
    device_wifi_credentials_t existing;
    const esp_err_t err = device_config_load_wifi(&existing);
    if (err == ESP_OK) return ESP_OK;
    if (err != ESP_ERR_NOT_FOUND) return err;
    return save_wifi_record(seed);
}

esp_err_t device_config_save_wifi(const device_wifi_credentials_t *credentials)
{
    if (!wifi_valid(credentials)) return ESP_ERR_INVALID_ARG;
    device_wifi_credentials_t existing;
    const esp_err_t err = device_config_load_wifi(&existing);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    return save_wifi_record(credentials);
}

static bool attempt_valid(const firmware_attempt_t *attempt)
{
    return attempt && attempt->format == 1 && attempt->id != 0 &&
        text_fits(attempt->previous, sizeof(attempt->previous)) &&
        text_fits(attempt->candidate, sizeof(attempt->candidate)) &&
        attempt->previous[0] && attempt->candidate[0] &&
        (unsigned) attempt->phase <= FIRMWARE_FAILED &&
        (unsigned) attempt->result <= FIRMWARE_RESULT_INTERRUPTED;
}

esp_err_t device_config_load_attempt(firmware_attempt_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t bytes[ATTEMPT_BYTES];
    esp_err_t err = read_record("attempt_v1", bytes, sizeof(bytes));
    if (err != ESP_OK) return err;
    firmware_attempt_t attempt = {.format = 1, .id = get_u32(bytes + 4),
        .phase = (firmware_phase_t) get_u32(bytes + 72),
        .result = (firmware_result_t) get_u32(bytes + 76)};
    memcpy(attempt.previous, bytes + 8, 32);
    memcpy(attempt.candidate, bytes + 40, 32);
    if (!attempt_valid(&attempt)) return ESP_ERR_INVALID_RESPONSE;
    *out = attempt;
    return ESP_OK;
}

esp_err_t device_config_save_attempt(const firmware_attempt_t *attempt)
{
    if (!attempt_valid(attempt)) return ESP_ERR_INVALID_ARG;
    uint8_t bytes[ATTEMPT_BYTES] = {0};
    put_u32(bytes, 1); put_u32(bytes + 4, attempt->id);
    memcpy(bytes + 8, attempt->previous, strlen(attempt->previous));
    memcpy(bytes + 40, attempt->candidate, strlen(attempt->candidate));
    put_u32(bytes + 72, (uint32_t) attempt->phase);
    put_u32(bytes + 76, (uint32_t) attempt->result);
    return write_record("attempt_v1", bytes, sizeof(bytes));
}

#define PRINTER_BYTES 376U

static bool ascii_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}
static bool ipv4_valid(const char *s) {
  for (unsigned part = 0; part < 4; part++) {
    const char *start = s;
    unsigned value = 0, n = 0;
    while (*s >= '0' && *s <= '9') {
      value = value * 10 + (unsigned)(*s++ - '0');
      if (++n > 3 || value > 255)
        return false;
    }
    if (!n || (n > 1 && *start == '0'))
      return false;
    if (part == 3)
      return *s == 0;
    if (*s++ != '.')
      return false;
  }
  return false;
}
static bool time_host_valid(const char *s) {
  bool numeric = true;
  for (const char *p = s; *p; p++)
    if ((*p < '0' || *p > '9') && *p != '.')
      numeric = false;
  if (numeric)
    return ipv4_valid(s);
  unsigned n = 0;
  char previous = 0;
  for (; *s; s++) {
    if (*s == '.') {
      if (!n || previous == '-')
        return false;
      n = 0;
    } else {
      if (!ascii_alnum(*s) && *s != '-')
        return false;
      if (!n && *s == '-')
        return false;
      if (++n > 63)
        return false;
    }
    previous = *s;
  }
  return n && previous != '-';
}
bool printer_config_valid(const printer_connection_config_t *p) {
  if (!p || !text_fits(p->ipv4, sizeof(p->ipv4)) ||
      !text_fits(p->serial, sizeof(p->serial)) ||
      !text_fits(p->access_code, sizeof(p->access_code)) ||
      !text_fits(p->ntp_host, sizeof(p->ntp_host)))
    return false;
  if (!ipv4_valid(p->ipv4) || !p->serial[0] || !p->access_code[0] ||
      !time_host_valid(p->ntp_host))
    return false;
  for (const char *s = p->serial; *s; s++)
    if (!ascii_alnum(*s))
      return false;
  return true;
}
esp_err_t device_config_load_printer(printer_connection_config_t *out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  uint8_t bytes[PRINTER_BYTES];
  esp_err_t err = read_record("printer_v1", bytes, sizeof(bytes));
  if (err != ESP_OK)
    return err;
  if (get_u32(bytes + 4) > 1)
    return ESP_ERR_INVALID_RESPONSE;
  printer_connection_config_t p = {.enabled = get_u32(bytes + 4) != 0};
  memcpy(p.ipv4, bytes + 8, 16);
  memcpy(p.serial, bytes + 24, 33);
  memcpy(p.access_code, bytes + 57, 65);
  memcpy(p.ntp_host, bytes + 122, 254);
  if (!printer_config_valid(&p))
    return ESP_ERR_INVALID_RESPONSE;
  *out = p;
  return ESP_OK;
}
esp_err_t device_config_save_printer(const printer_connection_config_t *p) {
  if (!printer_config_valid(p))
    return ESP_ERR_INVALID_ARG;
  printer_connection_config_t old;
  esp_err_t err = device_config_load_printer(&old);
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
    return err;
  uint8_t bytes[PRINTER_BYTES] = {0};
  put_u32(bytes, 1);
  put_u32(bytes + 4, p->enabled ? 1 : 0);
  memcpy(bytes + 8, p->ipv4, strlen(p->ipv4));
  memcpy(bytes + 24, p->serial, strlen(p->serial));
  memcpy(bytes + 57, p->access_code, strlen(p->access_code));
  memcpy(bytes + 122, p->ntp_host, strlen(p->ntp_host));
  return write_record("printer_v1", bytes, sizeof(bytes));
}
esp_err_t device_config_clear_printer(void) {
  if (!atomic_load(&s_ready))
    return ESP_ERR_INVALID_STATE;
  nvs_handle_t handle;
  esp_err_t err = nvs_open_from_partition(DEVICE_PARTITION, DEVICE_NAMESPACE,
                                          NVS_READWRITE, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND)
    return ESP_OK;
  if (err != ESP_OK)
    return err;
  err = nvs_erase_key(handle, "printer_v1");
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  else if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}
