set(TMFS_HELPERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../threadtorrent_lite")

idf_component_register(
    SRCS
        "main.c"
        "${TMFS_HELPERS_DIR}/threadtorrent_lite_auth.c"
        "${TMFS_HELPERS_DIR}/threadtorrent_lite_wire.c"
    INCLUDE_DIRS
        "."
        "${TMFS_HELPERS_DIR}/include"
    REQUIRES
        esp_driver_gpio
        esp_event
        esp_netif
        mbedtls
        nvs_flash
        openthread
        vfs
)
