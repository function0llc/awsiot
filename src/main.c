/**
 * AWS IoT ESP32-C3 Demo
 *
 * Connects to WiFi, synchronizes time via SNTP, and establishes a secure
 * MQTT connection to AWS IoT Core using X.509 certificate authentication.
 *
 * Features:
 *   - Periodic publishing to "tenants/default/devices/esp32-c3_awsiot1/evt/state" (every 15 minutes, counter 0-4999)
 *   - Device shadow updates via "$aws/things/esp32-c3_awsiot1/shadow/name/gpio/update"
 *   - LED/GPIO control via shadow topic with JSON payload {"state":{"desired":{"gpio":"on"|"off"}}}
 *   - Network connectivity verification via ICMP ping at startup
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <time.h>

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

#define AWS_IOT_ENDPOINT    "a3qhmfu2zenmjt-ats.iot.us-west-2.amazonaws.com"
#define AWS_IOT_PORT        8883
#define AWS_IOT_THING_NAME  "esp32-c3_awsiot1"
#define AWS_IOT_TENANT_ID   "default"
#define SHADOW_NAME         "gpio"
#define FW_VERSION          "1.0.0"
#define PRIMARY_PIN_ID      "17"
#define PING_TARGET_HOST    AWS_IOT_ENDPOINT

/* MQTT Topics */
#define TOPIC_CMD_GPIO              "tenants/" AWS_IOT_TENANT_ID "/devices/" AWS_IOT_THING_NAME "/cmd/gpio"
#define TOPIC_EVT_STATE             "tenants/" AWS_IOT_TENANT_ID "/devices/" AWS_IOT_THING_NAME "/evt/state"
#define TOPIC_SHADOW_GET            "$aws/things/" AWS_IOT_THING_NAME "/shadow/name/" SHADOW_NAME "/get"
#define TOPIC_SHADOW_GET_ACCEPTED   "$aws/things/" AWS_IOT_THING_NAME "/shadow/name/" SHADOW_NAME "/get/accepted"
#define TOPIC_SHADOW_UPDATE         "$aws/things/" AWS_IOT_THING_NAME "/shadow/name/" SHADOW_NAME "/update"
#define TOPIC_SHADOW_UPDATE_ACCEPTED "$aws/things/" AWS_IOT_THING_NAME "/shadow/name/" SHADOW_NAME "/update/accepted"
#define TOPIC_SHADOW_DELTA          "$aws/things/" AWS_IOT_THING_NAME "/shadow/name/" SHADOW_NAME "/update/delta"

#define MAX_GPIO_PINS    16
#define MAX_PIN_ID_LEN   12

typedef struct {
    char       pin_id[MAX_PIN_ID_LEN];
    gpio_num_t gpio;
    bool       drive_hardware;
    int        level; /* 0 = LOW, 1 = HIGH */
} gpio_pin_state_t;

static gpio_pin_state_t gpio_pins[MAX_GPIO_PINS] = {
    /* Dashboard logical pin "17" controls physical GPIO5 on the ESP32-C3 */
    { .pin_id = PRIMARY_PIN_ID, .gpio = GPIO_NUM_5,  .drive_hardware = true,  .level = 0 },
    { .pin_id = "18", .gpio = GPIO_NUM_NC, .drive_hardware = false, .level = 0 },
    { .pin_id = "22", .gpio = GPIO_NUM_NC, .drive_hardware = false, .level = 0 },
    { .pin_id = "23", .gpio = GPIO_NUM_NC, .drive_hardware = false, .level = 0 },
    { .pin_id = "24", .gpio = GPIO_NUM_NC, .drive_hardware = false, .level = 0 },
    { .pin_id = "25", .gpio = GPIO_NUM_NC, .drive_hardware = false, .level = 0 },
    { .pin_id = "27", .gpio = GPIO_NUM_NC, .drive_hardware = false, .level = 0 },
};

static size_t gpio_pin_count = 7;
static double last_shadow_report_ts = 0.0;

#include "certs.h"                          /* CA cert, device cert, key */

static const int WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t wifi_event_group;
static const char *TAG = "aws_iot_demo";

/* ------------------------------------------------------------------ */
/* GPIO pin bookkeeping                                               */
/* ------------------------------------------------------------------ */

static gpio_pin_state_t *find_pin(const char *pin_id) {
    if (!pin_id) return NULL;
    for (size_t i = 0; i < gpio_pin_count; ++i) {
        if (strcmp(gpio_pins[i].pin_id, pin_id) == 0) {
            return &gpio_pins[i];
        }
    }
    return NULL;
}

static gpio_pin_state_t *ensure_pin(const char *pin_id) {
    gpio_pin_state_t *pin = find_pin(pin_id);
    if (pin || !pin_id) {
        return pin;
    }

    if (gpio_pin_count >= MAX_GPIO_PINS) {
        ESP_LOGW(TAG, "Maximum GPIO pin entries reached (%d)", MAX_GPIO_PINS);
        return NULL;
    }

    gpio_pin_state_t *slot = &gpio_pins[gpio_pin_count++];
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->pin_id, pin_id, sizeof(slot->pin_id));
    slot->gpio = GPIO_NUM_NC;
    slot->drive_hardware = false;
    slot->level = 0;
    return slot;
}

static gpio_pin_state_t *primary_pin(void) {
    return find_pin(PRIMARY_PIN_ID);
}

static const char *primary_pin_shadow_state(void) {
    const gpio_pin_state_t *pin = primary_pin();
    if (!pin) return "off";
    return pin->level ? "on" : "off";
}

static bool parse_level(const char *state, int *level_out) {
    if (!state || !level_out) return false;
    if (strcasecmp(state, "HIGH") == 0 || strcasecmp(state, "ON") == 0 || strcmp(state, "1") == 0) {
        *level_out = 1;
        return true;
    }
    if (strcasecmp(state, "LOW") == 0 || strcasecmp(state, "OFF") == 0 || strcmp(state, "0") == 0) {
        *level_out = 0;
        return true;
    }
    return false;
}

static bool update_pin_level(gpio_pin_state_t *pin, const char *state) {
    if (!pin || !state) return false;
    int new_level = pin->level;
    if (!parse_level(state, &new_level)) {
        ESP_LOGW(TAG, "Unsupported GPIO state '%s' for pin %s", state, pin->pin_id);
        return false;
    }

    const bool changed = pin->level != new_level;
    pin->level = new_level;

    if (pin->drive_hardware && GPIO_IS_VALID_OUTPUT_GPIO(pin->gpio)) {
        esp_err_t err = gpio_set_level(pin->gpio, new_level);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set GPIO%d to %d (err=%s)", pin->gpio, new_level, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Pin %s mapped to GPIO%d -> %s (changed=%s)",
             pin->pin_id,
             pin->gpio,
             new_level ? "HIGH" : "LOW",
             changed ? "yes" : "no");

    return changed;
}

static bool apply_pin_map_from_object(const cJSON *pins) {
    if (!cJSON_IsObject(pins)) return false;
    bool mutated = false;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, pins) {
        if (!cJSON_IsString(entry) || entry->string == NULL) continue;
        gpio_pin_state_t *pin = ensure_pin(entry->string);
        if (pin && update_pin_level(pin, entry->valuestring)) {
            mutated = true;
        }
    }
    return mutated;
}

static bool apply_default_gpio_state(const char *state_value) {
    if (!state_value) return false;
    ESP_LOGI(TAG, "Applying legacy gpio state '%s' to primary pin", state_value);
    gpio_pin_state_t *pin = ensure_pin(PRIMARY_PIN_ID);
    if (!pin) return false;
    return update_pin_level(pin, state_value);
}

static void configure_gpio_pins(void) {
    for (size_t i = 0; i < gpio_pin_count; ++i) {
        if (!gpio_pins[i].drive_hardware || !GPIO_IS_VALID_OUTPUT_GPIO(gpio_pins[i].gpio)) {
            continue;
        }
        gpio_reset_pin(gpio_pins[i].gpio);
        gpio_set_direction(gpio_pins[i].gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio_pins[i].gpio, gpio_pins[i].level);
    }
}

static void publish_json(esp_mqtt_client_handle_t client, const char *topic, const char *payload) {
    if (!payload) return;
    int len = strlen(payload);
    esp_mqtt_client_publish(client, topic, payload, len, 1, 0);
}

static cJSON *create_pins_json(void) {
    cJSON *pins = cJSON_CreateObject();
    if (!pins) return NULL;

    for (size_t i = 0; i < gpio_pin_count; ++i) {
        cJSON_AddStringToObject(pins, gpio_pins[i].pin_id, gpio_pins[i].level ? "HIGH" : "LOW");
    }

    return pins;
}

static void publish_event_state(esp_mqtt_client_handle_t client, const char *req_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON *pins = create_pins_json();
    if (!pins) {
        cJSON_Delete(root);
        return;
    }

    cJSON_AddItemToObject(root, "pins", pins);
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    cJSON_AddStringToObject(root, "gpio", primary_pin_shadow_state());
    if (req_id) {
        cJSON_AddStringToObject(root, "reqId", req_id);
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        publish_json(client, TOPIC_EVT_STATE, payload);
        cJSON_free(payload);
    }
    cJSON_Delete(root);
}

static void publish_shadow_state(esp_mqtt_client_handle_t client, const char *req_id) {
    cJSON *pins = create_pins_json();
    if (!pins) return;

    cJSON *reported = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *root = cJSON_CreateObject();
    if (!reported || !state || !root) {
        cJSON_Delete(pins);
        cJSON_Delete(reported);
        cJSON_Delete(state);
        cJSON_Delete(root);
        return;
    }

    cJSON_AddItemToObject(reported, "pins", pins);
    double now_ts = (double)time(NULL);
    cJSON_AddNumberToObject(reported, "ts", now_ts);
    cJSON_AddStringToObject(reported, "fw", FW_VERSION);
    cJSON_AddStringToObject(reported, "gpio", primary_pin_shadow_state());
    cJSON_AddItemToObject(state, "reported", reported);
    cJSON_AddItemToObject(root, "state", state);
    if (req_id) {
        cJSON_AddStringToObject(root, "clientToken", req_id);
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        publish_json(client, TOPIC_SHADOW_UPDATE, payload);
        cJSON_free(payload);
    }
    cJSON_Delete(root);

    last_shadow_report_ts = now_ts;
}

static void sync_reported_state(esp_mqtt_client_handle_t client, const char *req_id) {
    publish_event_state(client, req_id);
    publish_shadow_state(client, req_id);
}

static void handle_gpio_command_message(esp_mqtt_client_handle_t client, const char *payload, int len) {
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) return;

    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *req_id = cJSON_GetObjectItemCaseSensitive(root, "reqId");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");

    if (cJSON_IsString(pin) && cJSON_IsString(state)) {
        gpio_pin_state_t *pin_entry = ensure_pin(pin->valuestring);
        if (pin_entry && update_pin_level(pin_entry, state->valuestring)) {
            ESP_LOGI(TAG, "Applied GPIO command: pin=%s state=%s mode=%s", pin->valuestring, state->valuestring, cJSON_IsString(mode) ? mode->valuestring : "n/a");
            sync_reported_state(client, cJSON_IsString(req_id) ? req_id->valuestring : NULL);
        } else if (pin_entry) {
            /* Even if unchanged, echo state so UI confirms */
            sync_reported_state(client, cJSON_IsString(req_id) ? req_id->valuestring : NULL);
        }
    }

    cJSON_Delete(root);
}

static void handle_shadow_delta_message(esp_mqtt_client_handle_t client, const char *payload, int len) {
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) return;

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *delta = cJSON_IsObject(state) ? cJSON_GetObjectItemCaseSensitive(state, "delta") : NULL;
    const cJSON *desired = cJSON_IsObject(state) ? cJSON_GetObjectItemCaseSensitive(state, "desired") : NULL;
    const cJSON *pins = cJSON_IsObject(delta) ? cJSON_GetObjectItemCaseSensitive(delta, "pins") : NULL;
    const cJSON *desired_pins = cJSON_IsObject(desired) ? cJSON_GetObjectItemCaseSensitive(desired, "pins") : NULL;
    const cJSON *metadata_root = cJSON_GetObjectItemCaseSensitive(root, "metadata");

    bool changed = false;
    ESP_LOGI(TAG, "Processing shadow delta payload");
    if (cJSON_IsObject(pins)) {
        changed = apply_pin_map_from_object(pins);
    } else if (cJSON_IsObject(desired_pins)) {
        changed = apply_pin_map_from_object(desired_pins);
    } else {
        typedef enum {
            LEGACY_SOURCE_NONE,
            LEGACY_SOURCE_DELTA,
            LEGACY_SOURCE_DESIRED,
            LEGACY_SOURCE_STATE
        } legacy_source_t;

        legacy_source_t legacy_source = LEGACY_SOURCE_NONE;
        const cJSON *legacy_gpio = NULL;
        if (cJSON_IsObject(delta)) {
            const cJSON *candidate = cJSON_GetObjectItemCaseSensitive(delta, "gpio");
            if (cJSON_IsString(candidate)) {
                legacy_gpio = candidate;
                legacy_source = LEGACY_SOURCE_DELTA;
            }
        }
        if (!legacy_gpio && cJSON_IsObject(desired)) {
            const cJSON *candidate = cJSON_GetObjectItemCaseSensitive(desired, "gpio");
            if (cJSON_IsString(candidate)) {
                legacy_gpio = candidate;
                legacy_source = LEGACY_SOURCE_DESIRED;
            }
        }
        if (!legacy_gpio && cJSON_IsObject(state)) {
            const cJSON *candidate = cJSON_GetObjectItemCaseSensitive(state, "gpio");
            if (cJSON_IsString(candidate)) {
                legacy_gpio = candidate;
                legacy_source = LEGACY_SOURCE_STATE;
            }
        }

        const cJSON *legacy_meta = NULL;
        if (legacy_source != LEGACY_SOURCE_NONE && cJSON_IsObject(metadata_root)) {
            switch (legacy_source) {
                case LEGACY_SOURCE_DELTA:
                    legacy_meta = cJSON_GetObjectItemCaseSensitive(metadata_root, "delta");
                    if (legacy_meta) legacy_meta = cJSON_GetObjectItemCaseSensitive(legacy_meta, "gpio");
                    break;
                case LEGACY_SOURCE_DESIRED:
                    legacy_meta = cJSON_GetObjectItemCaseSensitive(metadata_root, "desired");
                    if (legacy_meta) legacy_meta = cJSON_GetObjectItemCaseSensitive(legacy_meta, "gpio");
                    break;
                case LEGACY_SOURCE_STATE:
                    legacy_meta = cJSON_GetObjectItemCaseSensitive(metadata_root, "gpio");
                    if (!legacy_meta) {
                        legacy_meta = cJSON_GetObjectItemCaseSensitive(metadata_root, "state");
                        if (legacy_meta) legacy_meta = cJSON_GetObjectItemCaseSensitive(legacy_meta, "gpio");
                    }
                    break;
                default:
                    break;
            }
        }

        if (cJSON_IsString(legacy_gpio)) {
            bool stale = false;
            if (legacy_meta && cJSON_IsObject(legacy_meta)) {
                const cJSON *ts_node = cJSON_GetObjectItemCaseSensitive(legacy_meta, "timestamp");
                if (cJSON_IsNumber(ts_node)) {
                    double ts_value = ts_node->valuedouble;
                    if (last_shadow_report_ts > 0.0 && ts_value <= last_shadow_report_ts) {
                        stale = true;
                        ESP_LOGW(TAG, "Ignoring stale legacy gpio delta (ts=%.0f, last_report=%.0f)", ts_value, last_shadow_report_ts);
                    }
                }
            }

            if (!stale) {
                changed = apply_default_gpio_state(legacy_gpio->valuestring);
            }
        } else {
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, delta) {
                if (cJSON_IsString(entry)) {
                    changed = apply_default_gpio_state(entry->valuestring) || changed;
                }
            }
            if (!changed && cJSON_IsObject(desired)) {
                cJSON_ArrayForEach(entry, desired) {
                    if (cJSON_IsString(entry)) {
                        changed = apply_default_gpio_state(entry->valuestring) || changed;
                    }
                }
            }
            if (!changed) {
                ESP_LOGW(TAG, "Shadow delta payload missing expected fields");
            }
        }
    }

    if (changed) {
        sync_reported_state(client, NULL);
    }

    cJSON_Delete(root);
}

static void handle_shadow_get_message(esp_mqtt_client_handle_t client, const char *payload, int len) {
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) return;

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *desired = cJSON_IsObject(state) ? cJSON_GetObjectItemCaseSensitive(state, "desired") : NULL;
    const cJSON *desired_pins = cJSON_IsObject(desired) ? cJSON_GetObjectItemCaseSensitive(desired, "pins") : NULL;
    const cJSON *metadata_root = cJSON_GetObjectItemCaseSensitive(root, "metadata");

    bool changed = false;
    if (cJSON_IsObject(desired_pins)) {
        changed = apply_pin_map_from_object(desired_pins);
    } else {
        const cJSON *legacy_gpio = cJSON_IsObject(desired) ? cJSON_GetObjectItemCaseSensitive(desired, "gpio") : NULL;
        if (!cJSON_IsString(legacy_gpio) && cJSON_IsObject(state)) {
            legacy_gpio = cJSON_GetObjectItemCaseSensitive(state, "gpio");
        }
        if (cJSON_IsString(legacy_gpio)) {
            bool stale = false;
            if (cJSON_IsObject(metadata_root)) {
                const cJSON *meta_desired = cJSON_GetObjectItemCaseSensitive(metadata_root, "desired");
                const cJSON *meta_gpio = NULL;
                if (meta_desired) {
                    meta_gpio = cJSON_GetObjectItemCaseSensitive(meta_desired, "gpio");
                }
                if (!meta_gpio) {
                    meta_gpio = cJSON_GetObjectItemCaseSensitive(metadata_root, "gpio");
                }
                if (meta_gpio) {
                    const cJSON *ts_node = cJSON_GetObjectItemCaseSensitive(meta_gpio, "timestamp");
                    if (cJSON_IsNumber(ts_node) && last_shadow_report_ts > 0.0 && ts_node->valuedouble <= last_shadow_report_ts) {
                        stale = true;
                        ESP_LOGW(TAG, "Ignoring stale legacy gpio from get (ts=%.0f, last_report=%.0f)", ts_node->valuedouble, last_shadow_report_ts);
                    }
                }
            }

            if (!stale) {
                changed = apply_default_gpio_state(legacy_gpio->valuestring);
            }
        } else {
            const cJSON *entry = NULL;
            if (cJSON_IsObject(desired)) {
                cJSON_ArrayForEach(entry, desired) {
                    if (cJSON_IsString(entry)) {
                        changed = apply_default_gpio_state(entry->valuestring) || changed;
                    }
                }
            }
            if (!changed && cJSON_IsObject(state)) {
                cJSON_ArrayForEach(entry, state) {
                    if (cJSON_IsString(entry)) {
                        changed = apply_default_gpio_state(entry->valuestring) || changed;
                    }
                }
            }
            if (!changed) {
                ESP_LOGW(TAG, "Shadow get payload missing desired pins/gpio fields");
            }
        }
    }

    if (changed) {
        sync_reported_state(client, NULL);
    } else {
        publish_event_state(client, NULL);
    }

    cJSON_Delete(root);
}

static bool topic_matches(const char *incoming, int incoming_len, const char *expected) {
    size_t expected_len = strlen(expected);
    return (incoming_len == (int)expected_len) && strncmp(incoming, expected, expected_len) == 0;
}

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
 * - CONNECTED : subscribes to the event state topic and shadow delta topic.
 * - DATA      : logs the message; if it arrives on the shadow delta topic,
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

            esp_mqtt_client_subscribe(client, TOPIC_CMD_GPIO, 1);
            esp_mqtt_client_subscribe(client, TOPIC_SHADOW_DELTA, 1);
            esp_mqtt_client_subscribe(client, TOPIC_SHADOW_GET_ACCEPTED, 1);
            esp_mqtt_client_subscribe(client, TOPIC_SHADOW_UPDATE_ACCEPTED, 1);

            /* Request current shadow state on boot */
            esp_mqtt_client_publish(client, TOPIC_SHADOW_GET, "{}", 2, 1, 0);
            ESP_LOGI(TAG, "Requested shadow state via %s", TOPIC_SHADOW_GET);

            /* Broadcast our current GPIO state */
            sync_reported_state(client, NULL);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data:  %.*s", event->data_len, event->data);
            if (topic_matches(event->topic, event->topic_len, TOPIC_CMD_GPIO)) {
                handle_gpio_command_message(client, event->data, event->data_len);
            } else if (topic_matches(event->topic, event->topic_len, TOPIC_SHADOW_DELTA)) {
                handle_shadow_delta_message(client, event->data, event->data_len);
            } else if (topic_matches(event->topic, event->topic_len, TOPIC_SHADOW_GET_ACCEPTED)) {
                handle_shadow_get_message(client, event->data, event->data_len);
            } else if (topic_matches(event->topic, event->topic_len, TOPIC_SHADOW_UPDATE_ACCEPTED)) {
                ESP_LOGI(TAG, "Shadow update acknowledged");
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %ld", (long)event_id);
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
        .credentials.client_id           = AWS_IOT_THING_NAME,
        .credentials.authentication.certificate = device_cert_pem,
        .credentials.authentication.key  = private_key_pem,
    };

    /* 7. Configure GPIO pins defined in gpio_pins[] */
    configure_gpio_pins();

    /* 8. Create, register callbacks, and start the MQTT client */
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    /* 9. Main publish loop */
    while (1) {
        publish_event_state(client, NULL);
        ESP_LOGI(TAG, "Published periodic GPIO state");

        /* Publish every 15 minutes */
        vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
    }
}
