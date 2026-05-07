idf_component_register(
    SRCS
        "app_main.c"
        "wifi_manager.c"
        "mqtt_aws.c"
        "app_config.c"
        "time_sync.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "AmazonRootCA1.pem"
        "device_cert.pem"
        "private_key.pem"
)
