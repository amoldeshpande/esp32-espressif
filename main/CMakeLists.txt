idf_component_register(SRCS "main.cpp" "SomfyReceiver.cpp" "SomfyTransmitter.cpp" "SomfyController.cpp" "SomfyHttp.cpp" "SomfyNetwork.cpp"
                    EMBED_TXTFILES "Somfy.html"
                    INCLUDE_DIRS "."
                    REQUIRES CC1101Lib esp_driver_rmt nvs_flash esp_hw_support esp_wifi esp_event esp_netif esp_http_server json)

