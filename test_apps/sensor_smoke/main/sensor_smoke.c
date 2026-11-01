#include "board_outputs.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "temperature_sensor.h"

#define SAMPLE_COUNT 10
#define MIN_ROOM_TEMPERATURE_C 15.0f
#define MAX_ROOM_TEMPERATURE_C 35.0f

static const char *TAG = "sensor_smoke";

void app_main(void)
{
    ESP_ERROR_CHECK(board_outputs_init(NULL));
    ESP_ERROR_CHECK(board_outputs_force_off());

    temperature_sensor_t sensor = {0};
    const temperature_sensor_config_t config =
        temperature_sensor_default_config(BOARD_TEMPERATURE_SENSOR_GPIO);
    ESP_ERROR_CHECK(temperature_sensor_init(&sensor, &config));

    ESP_LOGI(TAG, "relays forced off; starting sensor samples in two seconds");
    vTaskDelay(pdMS_TO_TICKS(2000));

    unsigned healthy_count = 0U;
    float temperature_total_c = 0.0f;
    for (unsigned sample = 1U; sample <= SAMPLE_COUNT; ++sample) {
        temperature_sensor_reading_t reading;
        const esp_err_t err = temperature_sensor_read(&sensor, &reading);
        ESP_LOGI(TAG,
                 "sample=%u err=%s temp=%.3f fault=%d",
                 sample, esp_err_to_name(err),
                 (double) reading.temperature_c, (int) reading.fault);
        if (err == ESP_OK && reading.fault == TEMPERATURE_SENSOR_FAULT_NONE) {
            ++healthy_count;
            temperature_total_c += reading.temperature_c;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    const float average_c = healthy_count == 0U
        ? 0.0f
        : temperature_total_c / (float) healthy_count;
    if (healthy_count >= 8U && average_c >= MIN_ROOM_TEMPERATURE_C &&
        average_c <= MAX_ROOM_TEMPERATURE_C) {
        ESP_LOGI(TAG, "SENSOR_SMOKE_PASS healthy=%u average=%.2f C",
                 healthy_count, (double) average_c);
    } else {
        ESP_LOGE(TAG, "SENSOR_SMOKE_FAIL healthy=%u average=%.2f C",
                 healthy_count, (double) average_c);
    }

    ESP_ERROR_CHECK(board_outputs_force_off());
    ESP_ERROR_CHECK(temperature_sensor_deinit(&sensor));
}
