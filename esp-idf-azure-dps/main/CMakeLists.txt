idf_component_register(SRCS "app_main.c"
                    PRIV_REQUIRES mqtt nvs_flash esp_netif json app_update mbedtls esp_http_client esp_wifi
                    INCLUDE_DIRS ".")