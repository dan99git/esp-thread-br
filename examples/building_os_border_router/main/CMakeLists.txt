set(requires
    nvs_flash
    esp_event
    esp_netif
    esp_eth
    esp_wifi
    esp_http_server
    spiffs
    esp_ot_br_server
    esp_rcp_update
    json
    driver
    esp_timer
    mdns
    openthread
    protocol_examples_common
    vfs
    esp_ot_cli_extension
    thread_border_router
    bos_server_registration
    bos_ledger_ingress
    bos_ledger_mesh_serve
    bos_convergence_aggregator
    bos_thread_dataset_anchor
    bos_diagnostics_server
)

if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND requires esp_driver_uart)
endif()

if(BOS_BR_NEEDS_OPENTHREAD_START_COMPAT)
    list(APPEND requires openthread_start_compat)
endif()

idf_component_register(
    SRCS
        "app_main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        ${requires}
)
