#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

#define AWS_IOT_ENDPOINT "YOUR_AWS_ENDPOINT-ats.iot.YOUR_REGION.amazonaws.com"
#define AWS_IOT_PORT 8883
#define AWS_IOT_THING_NAME "esp32c3-demo"

/* Replace with AmazonRootCA1.pem contents */
static const char AWS_ROOT_CA_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"REPLACE_WITH_AMAZON_ROOT_CA_1\n"
"-----END CERTIFICATE-----\n";

/* Replace with your AWS IoT device certificate PEM */
static const char AWS_DEVICE_CERT_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"REPLACE_WITH_DEVICE_CERTIFICATE\n"
"-----END CERTIFICATE-----\n";

/* Replace with your AWS IoT private key PEM */
static const char AWS_PRIVATE_KEY_PEM[] =
"-----BEGIN RSA PRIVATE KEY-----\n"
"REPLACE_WITH_PRIVATE_KEY\n"
"-----END RSA PRIVATE KEY-----\n";

static const int WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t wifi_event_group;
static const char *TAG = "aws_iot_demo";

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to AWS IoT Core");
            esp_mqtt_client_subscribe(client, "esp32c3/in", 1);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data:  %.*s", event->data_len, event->data);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            break;
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    char uri[256];
    snprintf(uri, sizeof(uri), "mqtts://%s:%d", AWS_IOT_ENDPOINT, AWS_IOT_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .broker.verification.certificate = AWS_ROOT_CA_PEM,
        .credentials.client_id = AWS_IOT_THING_NAME,
        .credentials.authentication.certificate = AWS_DEVICE_CERT_PEM,
        .credentials.authentication.key = AWS_PRIVATE_KEY_PEM,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    uint32_t counter = 0;
    char payload[128];

    while (1) {
        int len = snprintf(payload, sizeof(payload), "{\"thing\":\"%s\",\"count\":%lu}", AWS_IOT_THING_NAME, (unsigned long)counter++);
        esp_mqtt_client_publish(client, "esp32c3/out", payload, len, 1, 0);
        ESP_LOGI(TAG, "Published: %s", payload);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
