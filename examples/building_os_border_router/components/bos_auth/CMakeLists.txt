idf_component_register(
    SRCS "bos_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bos_server_registration
        esp_http_server
)
