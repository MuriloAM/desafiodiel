#include <stdio.h>

#include "esp_event.h"
#include "esp_err.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

// defines.
#define APP_MAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 5)
#define APP_MAIN_TASK_STACK (1024 * 2)

// statics variables.
static const char *TAG = "esp_app";

// functions prototypes.
static void app_wifi_init(void);

// task prototypes.
static void app_main_task(void *pvParameters);

// --- funcoes handlers / auxiliares ---
static void app_wifi_init()
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// --- tasks da aplicacao ---
static void app_main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "%s [start]", __func__);
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    // initialize the default NVS partition
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Inicializar sistema padrão de eventos.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_wifi_init();

    xTaskCreatePinnedToCore(
        app_main_task,          // Task function.
        "app_main_task",        // Task name function.
        APP_MAIN_TASK_STACK,    // Size of task stack.
        NULL,                   // Pointer to pass parameter in task creation.
        APP_MAIN_TASK_PRIORITY, // Priority that task should run.
        NULL,                   // Handle for reference the created task.
        APP_CPU_NUM);           // Core to run this task.

}