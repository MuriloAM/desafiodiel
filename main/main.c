#include <stdio.h>

#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "sdkconfig.h"

// defines.
#define APP_MAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 5)
#define APP_MAIN_TASK_STACK (1024 * 2)

// statics variables.
static const char *TAG = "esp_app";
static esp_netif_t *esp_netif_sta = NULL;
static esp_event_handler_instance_t instance_any_id = NULL;
static esp_event_handler_instance_t instance_got_ip = NULL;

// functions prototypes.
static void app_wifi_init(void);

// task prototypes.
static void app_main_task(void *pvParameters);

// --- funcoes handlers / auxiliares ---
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void app_wifi_init()
{
    // gurad to start esp netif stack once.
    if (esp_netif_is_netif_up(esp_netif_sta) == false)
    {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_netif_sta = esp_netif_create_default_wifi_sta();
    }
    if (instance_any_id == NULL)
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_any_id));
    if (instance_got_ip == NULL)
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_got_ip));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    wifi_config_t wifi_conf = {.sta = {.ssid = CONFIG_WIFI_SSID, .password = CONFIG_WIFI_PSWD}};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_conf));
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