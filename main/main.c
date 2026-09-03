#include <stdio.h>

#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "iot_button.h"
#include "button_gpio.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

typedef enum
{
    ID_BUTTON_SINGLE_CLICK = 0,
    ID_TIMER_TRIG,
    ID_MAX
} id_t;

typedef struct
{
    uint32_t id;
    void *buffer;
} msg_t;

typedef struct
{
    bool button_state;
    uint32_t sensor1;
    uint32_t sensor2;
} data_t;

// defines.
#define APP_MAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 5)
#define APP_MAIN_TASK_STACK (1024 * 2)
#define APP_DEFAULT_INTERVAL (5 * 1000 * 1000)
// GPIO button
#define APP_BUTTON_LONG_PRESS_TIME (2000)
#define APP_BUTTON_SHORT_PRESS_TIME (200)
#define APP_BUTTON_0_ACTIVE_LEVEL (0)
#define APP_BUTTON_0_GPIO (0)
// MQTT.
#define MQTT_BROKER_URI "mqtts://broker.emqx.io:8883" //"mqtt.eclipse.org" //"broker.hivemq.com"//
#define MQTT_TOPIC_CMD_SUB "desafiodiel/murilo/esp32/cmd"
#define MQTT_TOPIC_CMD_PUB "desafiodiel/murilo/esp32/status"
/* Embedded Mosquitto CA certificate for test.mosquitto.org:8883 */
extern const uint8_t mqtt_crt_start[] asm("_binary_broker_emqx_io_ca_crt_start");
extern const uint8_t mqtt_crt_end[] asm("_binary_broker_emqx_io_ca_crt_end");

// statics variables.
static const char *TAG = "esp_app";
static esp_netif_t *esp_netif_sta = NULL;
static esp_event_handler_instance_t instance_any_id = NULL;
static esp_event_handler_instance_t instance_got_ip = NULL;
static QueueHandle_t xQueue = NULL;

// functions prototypes.
static esp_err_t app_button_init(button_handle_t *btn);
static esp_err_t app_sensor_get_value(uint32_t *data);
static void app_mqtt_init(void);
static void app_wifi_init(void);
static void button_single_click_event_cb(void *arg, void *data);

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
        app_mqtt_init();
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, MQTT_TOPIC_CMD_SUB, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_publish(client, MQTT_TOPIC_CMD_PUB, "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d, return code=0x%02x ", event->msg_id, (uint8_t)*event->data);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        }
        else
        {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void button_single_click_event_cb(void *arg, void *data)
{
    msg_t msg = {.id = ID_BUTTON_SINGLE_CLICK, .buffer = NULL};
    xQueueSend(xQueue, &msg, portMAX_DELAY);
}

static esp_err_t app_button_init(button_handle_t *btn)
{
    const button_config_t btn_cfg = {
        .long_press_time = APP_BUTTON_LONG_PRESS_TIME,
        .short_press_time = APP_BUTTON_SHORT_PRESS_TIME};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = APP_BUTTON_0_GPIO,
        .active_level = APP_BUTTON_0_ACTIVE_LEVEL,
        .disable_pull = false};
    // Button handle
    button_handle_t new_btn;
    // Create a new button device
    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &new_btn);
    *btn = new_btn;
    return ret;
}

static esp_err_t app_sensor_get_value(uint32_t *data)
{
    uint8_t buff[4];
    esp_fill_random(buff, 4);
    memcpy(data, buff, 4);
    return ESP_OK;
}

static void app_mqtt_init()
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_BROKER_URI,
            .verification.certificate = (const char *)mqtt_crt_start},
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "can't create mqtt client");
        return;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
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

static void s_periodic_timer_callback(void *arg)
{
    msg_t msg = {.id = ID_TIMER_TRIG, .buffer = NULL};
    xQueueSend(xQueue, &msg, portMAX_DELAY);
}

// --- tasks da aplicacao ---
static void app_main_task(void *pvParameters)
{
    data_t data_sys = {
        .button_state = false,
        .sensor1 = 0,
        .sensor2 = 0};

    if (xQueue == NULL)
        xQueue = xQueueCreate(10, sizeof(msg_t));

    // init gpio button
    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(app_button_init(&btn));
    // Register callback for button press
    esp_err_t ret = iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_single_click_event_cb, NULL);
    ESP_ERROR_CHECK(ret);

    // Timer to controll periodic mensages.
    esp_timer_handle_t periodic_timer;
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &s_periodic_timer_callback,
        .arg = xTaskGetCurrentTaskHandle(),
        .name = "battery_timer"};
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, APP_DEFAULT_INTERVAL));

    for (;;)
    {
        msg_t xMsg;
        if (xQueueReceive(xQueue, &xMsg, portMAX_DELAY) == pdTRUE)
        {
            if (xMsg.id == ID_BUTTON_SINGLE_CLICK)
            {
                /* send mqtt msg onclick */
                if (data_sys.button_state)
                {
                    data_sys.button_state = false;
                    ESP_LOGI(TAG, "Button:OFF");
                }
                else
                {
                    data_sys.button_state = true;
                    ESP_LOGI(TAG, "Button:ON");
                }
                app_sensor_get_value(&data_sys.sensor1);
                app_sensor_get_value(&data_sys.sensor2);
                ESP_LOGI(TAG, "ButtonClick sensor1:%.4d sensor2:%.4d button_status:%s",
                         data_sys.sensor1, data_sys.sensor2, data_sys.button_state ? "ON" : "OFF");
            }
            else if (xMsg.id == ID_TIMER_TRIG)
            {
                /* create message buffer in json */
                // sendo via mqtt if connected to the broken.
                app_sensor_get_value(&data_sys.sensor1);
                app_sensor_get_value(&data_sys.sensor2);
                ESP_LOGI(TAG, "TimerTrigg sensor1:%.4d sensor2:%.4d button_status:%s",
                         data_sys.sensor1, data_sys.sensor2, data_sys.button_state ? "ON" : "OFF");
            }
            else if (xMsg.id == ID_MAX)
            {
                /* code */
            }
        }
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