#include "device_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "unity.h"
#include <string.h>

TEST_CASE("optional printer record survives reload and clear preserves Wi-Fi", "[device_config]")
{
    TEST_ESP_OK(device_config_init());
    TEST_ESP_OK(device_config_clear_printer());
    printer_connection_config_t p={.enabled=true,.ipv4="192.168.1.9",.serial="TEST123",.access_code="test-code",.ntp_host="pool.ntp.org"}, got;
    device_wifi_credentials_t wifi={"fixture","fixture-password"}, saved;
    TEST_ESP_OK(device_config_save_wifi(&wifi));
    TEST_ESP_OK(device_config_save_printer(&p));
    TEST_ESP_OK(device_config_init());
    TEST_ESP_OK(device_config_load_printer(&got));
    TEST_ASSERT_EQUAL_STRING(p.serial,got.serial);
    TEST_ASSERT_EQUAL_STRING(p.access_code,got.access_code);
    TEST_ESP_OK(device_config_clear_printer());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,device_config_load_printer(&got));
    TEST_ESP_OK(device_config_load_wifi(&saved));
    TEST_ASSERT_EQUAL_STRING(wifi.ssid,saved.ssid);
}

TEST_CASE("saved Wi-Fi survives generic firmware and attempt updates", "[device_config]")
{
    (void) nvs_flash_deinit_partition("device_cfg");
    TEST_ESP_OK(nvs_flash_erase_partition("device_cfg"));
    TEST_ESP_OK(device_config_init());
    device_wifi_credentials_t seed = {"fixture-network", "fixture-password"}, got;
    TEST_ESP_OK(device_config_seed_wifi(&seed));
    device_wifi_credentials_t empty = {0};
    TEST_ESP_OK(device_config_seed_wifi(&empty));
    TEST_ESP_OK(nvs_flash_deinit_partition("device_cfg"));
    TEST_ESP_OK(device_config_init());
    TEST_ESP_OK(device_config_load_wifi(&got));
    TEST_ASSERT_EQUAL_STRING("fixture-network", got.ssid);
    firmware_attempt_t attempt = {.format = 1, .id = 1, .previous = "1.0.0",
        .candidate = "1.0.1", .phase = FIRMWARE_REBOOTING};
    TEST_ESP_OK(device_config_save_attempt(&attempt));
    TEST_ESP_OK(device_config_load_wifi(&got));
    TEST_ASSERT_EQUAL_STRING("fixture-password", got.password);
    TEST_ESP_OK(nvs_flash_init());
    TEST_ESP_OK(settings_set_target(55.5f));
    TEST_ESP_OK(settings_set_calibration(-1.25f));
    TEST_ESP_OK(device_config_save_attempt(&attempt));
    settings_data_t settings;
    settings_load_result_t load_result;
    TEST_ESP_OK(settings_load(&settings, &load_result));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 55.5f, settings.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.25f, settings.calibration_offset_c);
}

TEST_CASE("future device record cannot be erased or reseeded", "[device_config]")
{
    TEST_ESP_OK(device_config_init());
    nvs_handle_t handle;
    TEST_ESP_OK(nvs_open_from_partition("device_cfg", "p1s_device", NVS_READWRITE, &handle));
    uint8_t future[101] = {2};
    TEST_ESP_OK(nvs_set_blob(handle, "wifi_v1", future, sizeof(future)));
    TEST_ESP_OK(nvs_commit(handle));
    nvs_close(handle);
    device_wifi_credentials_t seed = {"replacement", "replacement-password"};
    TEST_ASSERT_NOT_EQUAL(ESP_OK, device_config_seed_wifi(&seed));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, device_config_save_wifi(&seed));
    TEST_ESP_OK(nvs_open_from_partition("device_cfg", "p1s_device", NVS_READONLY, &handle));
    uint8_t got[101]; size_t size = sizeof(got);
    TEST_ESP_OK(nvs_get_blob(handle, "wifi_v1", got, &size));
    nvs_close(handle);
    TEST_ASSERT_EQUAL_MEMORY(future, got, sizeof(future));
    TEST_ESP_OK(nvs_flash_deinit_partition("device_cfg"));
    TEST_ESP_OK(nvs_flash_erase_partition("device_cfg"));
}

TEST_CASE("Wi-Fi replacement preserves exact text and survives restart", "[device_config]")
{
    (void)nvs_flash_deinit_partition("device_cfg");
    TEST_ESP_OK(nvs_flash_erase_partition("device_cfg"));
    TEST_ESP_OK(device_config_init());
    device_wifi_credentials_t first={"first", "first-password"};
    device_wifi_credentials_t next={" spaced network ", " visible password "}, got;
    TEST_ESP_OK(device_config_seed_wifi(&first));
    TEST_ESP_OK(device_config_save_wifi(&next));
    TEST_ESP_OK(nvs_flash_deinit_partition("device_cfg"));
    TEST_ESP_OK(device_config_init());
    TEST_ESP_OK(device_config_load_wifi(&got));
    TEST_ASSERT_EQUAL_STRING(next.ssid, got.ssid);
    TEST_ASSERT_EQUAL_STRING(next.password, got.password);
    TEST_ESP_OK(nvs_flash_deinit_partition("device_cfg"));
    TEST_ESP_OK(nvs_flash_erase_partition("device_cfg"));
}

TEST_CASE("bootstrap health confirmation needs an explicit persistent marker", "[device_config]")
{
    (void) nvs_flash_deinit_partition("device_cfg");
    TEST_ESP_OK(nvs_flash_erase_partition("device_cfg"));
    TEST_ESP_OK(device_config_init());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, device_config_bootstrap_confirmed());
    TEST_ESP_OK(device_config_confirm_bootstrap());
    TEST_ESP_OK(nvs_flash_deinit_partition("device_cfg"));
    TEST_ESP_OK(device_config_init());
    TEST_ESP_OK(device_config_bootstrap_confirmed());
    nvs_handle_t handle;
    TEST_ESP_OK(nvs_open_from_partition("device_cfg", "p1s_device", NVS_READWRITE, &handle));
    const uint8_t future[8] = {2,0,0,0,1};
    TEST_ESP_OK(nvs_set_blob(handle, "boot_ok_v1", future, sizeof(future)));
    TEST_ESP_OK(nvs_commit(handle));
    nvs_close(handle);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, device_config_confirm_bootstrap());
    TEST_ESP_OK(nvs_flash_deinit_partition("device_cfg"));
    TEST_ESP_OK(nvs_flash_erase_partition("device_cfg"));
}
