#include "app_config.h"

#include "sdkconfig.h"

#ifndef CONFIG_APP_WIFI_SSID
#define CONFIG_APP_WIFI_SSID "YOUR_WIFI"
#endif

#ifndef CONFIG_APP_WIFI_PASSWORD
#define CONFIG_APP_WIFI_PASSWORD "YOUR_WIFI_PASS"
#endif

#ifndef CONFIG_APP_AWS_ENDPOINT
#define CONFIG_APP_AWS_ENDPOINT "xxxxxxxxxxxxx-ats.iot.us-west-2.amazonaws.com"
#endif

#ifndef CONFIG_APP_AWS_CLIENT_ID
#define CONFIG_APP_AWS_CLIENT_ID "esp32c3-01"
#endif

#ifndef CONFIG_APP_MQTT_PUB_TOPIC
#define CONFIG_APP_MQTT_PUB_TOPIC "esp32c3/test"
#endif

#ifndef CONFIG_APP_MQTT_SUB_TOPIC
#define CONFIG_APP_MQTT_SUB_TOPIC "esp32c3/test"
#endif

#ifndef CONFIG_APP_MQTT_PORT
#define CONFIG_APP_MQTT_PORT 8883
#endif

#ifndef CONFIG_APP_PUBLISH_INTERVAL_MS
#define CONFIG_APP_PUBLISH_INTERVAL_MS 5000
#endif

static const app_config_t APP_CONFIG = {
    .wifi_ssid = CONFIG_APP_WIFI_SSID,
    .wifi_pass = CONFIG_APP_WIFI_PASSWORD,
    .aws_endpoint = CONFIG_APP_AWS_ENDPOINT,
    .aws_client_id = CONFIG_APP_AWS_CLIENT_ID,
    .pub_topic = CONFIG_APP_MQTT_PUB_TOPIC,
    .sub_topic = CONFIG_APP_MQTT_SUB_TOPIC,
    .mqtt_port = CONFIG_APP_MQTT_PORT,
    .publish_interval_ms = CONFIG_APP_PUBLISH_INTERVAL_MS,
};

const app_config_t *app_config_get(void)
{
    return &APP_CONFIG;
}
