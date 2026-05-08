/**
 * AWS IoT ESP32-C3 Demo
 *
 * Connects to WiFi, synchronizes time via SNTP, and establishes a secure
 * MQTT connection to AWS IoT Core using X.509 certificate authentication.
 *
 * Features:
 *   - Periodic publishing to "sdk/test/python" (every 15 minutes, counter 0-4999)
 *   - LED/GPIO control via MQTT topic "<thing_name>/led/set" with JSON payload {"message":"on"|"off"}
 *   - Network connectivity verification via ICMP ping at startup
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "esp_sntp.h"
#include "cJSON.h"

#include "wifi_credentials.h"

/* ------------------------------------------------------------------ */
/* Configuration constants                                            */
/* ------------------------------------------------------------------ */

#define AWS_IOT_ENDPOINT   "a3qhmfu2zenmjt-ats.iot.us-west-2.amazonaws.com"
#define AWS_IOT_PORT       8883
#define AWS_IOT_THING_NAME "esp32-c3_awsiot1"
#define PING_TARGET_HOST   AWS_IOT_ENDPOINT
#define CONTROL_GPIO_PIN   GPIO_NUM_5       /* LED control pin          */

#include "certs.h"                          /* CA cert, device cert, key */

static const int WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t wifi_event_group;
static const char *TAG = "aws_iot_demo";

/* ------------------------------------------------------------------ */
/* Wi-Fi helpers                                                      */
/* ------------------------------------------------------------------ */

/** Convert a Wi-Fi disconnect reason code to a human-readable string. */
static const char *wifi_disconnect_reason_to_str(wifi_err_reason_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:          return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_FAIL:            return "AUTH_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT:       return "BEACON_TIMEOUT";
        case WIFI_REASON_ASSOC_LEAVE:          return "ASSOC_LEAVE";
        case WIFI_REASON_NO_AP_FOUND:          return "NO_AP_FOUND";
        default:                               return "OTHER";
    }
}

/**
 * Wi-Fi event handler.
 *
 * - WIFI_EVENT_STA_START    : initiates the connection.
 * - WIFI_EVENT_STA_DISCONNECTED : logs reason and retries.
 * - IP_EVENT_STA_GOT_IP     : sets the connected bit so app_main can proceed.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d %s), retrying",
                 disconn->reason, wifi_disconnect_reason_to_str(disconn->reason));
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&got_ip->ip_info.ip));
    }
}

/**
 * Initialise Wi-Fi in station mode and block until an IP address is obtained.
 */
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

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid,     WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Block until connected */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ------------------------------------------------------------------ */
/* SNTP time synchronisation                                          */
/* ------------------------------------------------------------------ */

/**
 * Configure SNTP to poll pool.ntp.org and wait (up to 10 s) for the
 * first successful synchronisation.
 *
 * Accurate time is required for TLS certificate validation.
 */
static void time_sync_init(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    time_t now = 0;
    int retries = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retries < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    time(&now);
    ESP_LOGI(TAG, "Time synchronized: %s", ctime(&now));
}

/* ------------------------------------------------------------------ */
/* MQTT event handler                                                 */
/* ------------------------------------------------------------------ */

/**
 * Handles MQTT lifecycle events.
 *
 * - CONNECTED : subscribes to the data topic and the LED control topic.
 * - DATA      : logs the message; if it arrives on the "led/set" topic,
 *               parses the JSON body and toggles the GPIO pin.
 * - ERROR     : logs the failure.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to AWS IoT Core");

            /* Subscribe to the general data topic */
            esp_mqtt_client_subscribe(client, "sdk/test/python", 1);

            /* Subscribe to the LED control topic: <thing>/led/set */
            char control_topic[128];
            snprintf(control_topic, sizeof(control_topic),
                     "%s/led/set", AWS_IOT_THING_NAME);
            esp_mqtt_client_subscribe(client, control_topic, 1);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data:  %.*s", event->data_len, event->data);

            /* Check if this message is on the LED control topic */
            char *topic = strndup(event->topic, event->topic_len);
            if (strstr(topic, "led/set") != NULL) {
                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
                if (root != NULL) {
                    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
                    if (cJSON_IsString(msg) && msg->valuestring != NULL) {
                        if (strcmp(msg->valuestring, "on") == 0) {
                            gpio_set_level(CONTROL_GPIO_PIN, 1);
                            ESP_LOGI(TAG, "GPIO pin set HIGH");
                        } else if (strcmp(msg->valuestring, "off") == 0) {
                            gpio_set_level(CONTROL_GPIO_PIN, 0);
                            ESP_LOGI(TAG, "GPIO pin set LOW");
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            free(topic);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Ping test helpers                                                    */
/* ------------------------------------------------------------------ */

static void ping_on_success(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time_ms = 0;
    uint32_t recv_len = 0;
    ip_addr_t target_addr;

    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP,  &elapsed_time_ms, sizeof(elapsed_time_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,    &recv_len,       sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,   &target_addr,    sizeof(target_addr));

    ESP_LOGI(TAG, "ping success: seq=%lu time=%lums target=%s",
             (unsigned long)recv_len,
             (unsigned long)elapsed_time_ms,
             ipaddr_ntoa(&target_addr));
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args) {
    uint32_t seqno = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout: seq=%lu", (unsigned long)seqno);
}

static void ping_on_end(esp_ping_handle_t hdl, void *args) {
    uint32_t transmitted = 0;
    uint32_t received    = 0;
    uint32_t total_time_ms = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST,  &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,    &received,    sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));

    ESP_LOGI(TAG, "ping finished: tx=%lu rx=%lu time=%lums",
             (unsigned long)transmitted,
             (unsigned long)received,
             (unsigned long)total_time_ms);
}

/**
 * Run a short ICMP ping session (4 packets, 1 s interval) to *host*.
 * Used at boot to verify DNS resolution and basic connectivity to AWS.
 */
static esp_err_t run_ping_test(const char *host) {
    struct addrinfo hints = { .ai_family = AF_INET };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, NULL, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for %s (err=%d)", host, err);
        return ESP_FAIL;
    }

    struct in_addr addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;
    ping_config.target_addr.u_addr.ip4.addr = addr.s_addr;
    ping_config.count        = 4;
    ping_config.interval_ms  = 1000;
    ping_config.timeout_ms   = 1000;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end     = ping_on_end,
    };

    esp_ping_handle_t ping_handle = NULL;
    esp_err_t ret = esp_ping_new_session(&ping_config, &callbacks, &ping_handle);
    if (ret != ESP_OK) {
        freeaddrinfo(res);
        ESP_LOGE(TAG, "esp_ping_new_session failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pinging %s (%s)", host, inet_ntoa(addr));
    esp_ping_start(ping_handle);

    /* Wait for the session to finish */
    vTaskDelay(pdMS_TO_TICKS((ping_config.count + 1) * ping_config.interval_ms));

    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    freeaddrinfo(res);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Application entry point                                             */
/* ------------------------------------------------------------------ */

void app_main(void) {
    /* 1. Initialise non-volatile storage (required for Wi-Fi) */
    ESP_ERROR_CHECK(nvs_flash_init());

    /* 2. Connect to Wi-Fi (blocks until IP obtained) */
    wifi_init_sta();

    /* 3. Synchronise RTC via SNTP (TLS cert validation needs correct time) */
    time_sync_init();

    /* 4. Verify connectivity to AWS IoT endpoint */
    ESP_ERROR_CHECK(run_ping_test(PING_TARGET_HOST));

    /* 5. Build the MQTTS URI */
    char uri[256];
    snprintf(uri, sizeof(uri), "mqtts://%s:%d", AWS_IOT_ENDPOINT, AWS_IOT_PORT);

    /* 6. Configure MQTT client with TLS mutual-auth certificates */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri              = uri,
        .broker.verification.certificate = AmazonRootCA1_pem,
        .credentials.client_id           = "basicPubSub",
        .credentials.authentication.certificate = device_cert_pem,
        .credentials.authentication.key  = private_key_pem,
    };

    /* 7. Configure GPIO pin for LED output (default LOW) */
    gpio_reset_pin(CONTROL_GPIO_PIN);
    gpio_set_direction(CONTROL_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONTROL_GPIO_PIN, 0);

    /* 8. Create, register callbacks, and start the MQTT client */
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    /* 9. Main publish loop */
    uint32_t counter = 0;
    char     payload[128];

    while (1) {
        int len = snprintf(payload, sizeof(payload),
                           "{\"thing\":\"%s\",\"count\":%lu}",
                           AWS_IOT_THING_NAME, (unsigned long)counter++);

        /* Roll over at 5 000 messages */
        if (counter >= 5000) counter = 0;

        esp_mqtt_client_publish(client, "sdk/test/python", payload, len, 1, 0);
        ESP_LOGI(TAG, "Published: %s", payload);

        /* Publish every 15 minutes */
        vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
    }
}
