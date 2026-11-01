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
