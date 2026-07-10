# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
cmake_policy(SET CMP0079 NEW)

# Night-shift 2026-06-13 isolation-test shim (original kept at
# CMakeLists.txt.orig-copy): installed ESP-IDF 5.5 removed
# esp_openthread_start/esp_openthread_config_t that this upstream example
# expects. Reuse the fork's API compat component so the example builds; the
# OpenThread/BR bring-up logic itself stays 100% upstream.
set(STOCK_TEST_COMPAT_PATH
    "${CMAKE_SOURCE_DIR}/../building_os_border_router/local_helpers/openthread_start_compat")
set(STOCK_TEST_NEEDS_COMPAT OFF)
set(STOCK_TEST_ESP_OPENTHREAD_HEADER "$ENV{IDF_PATH}/components/openthread/include/esp_openthread.h")
if(EXISTS "${STOCK_TEST_ESP_OPENTHREAD_HEADER}")
    file(READ "${STOCK_TEST_ESP_OPENTHREAD_HEADER}" STOCK_TEST_ESP_OPENTHREAD_HEADER_CONTENT)
    if(NOT STOCK_TEST_ESP_OPENTHREAD_HEADER_CONTENT MATCHES "esp_openthread_start")
        set(STOCK_TEST_NEEDS_COMPAT ON)
    endif()
endif()
if(STOCK_TEST_NEEDS_COMPAT)
    list(APPEND EXTRA_COMPONENT_DIRS "${STOCK_TEST_COMPAT_PATH}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_ot_br)

if(STOCK_TEST_NEEDS_COMPAT)
    set(STOCK_TEST_COMPAT_HEADER
        "${STOCK_TEST_COMPAT_PATH}/include/bos_openthread_compat.h")
    foreach(component_name main thread_border_router)
        idf_component_get_property(component_lib ${component_name} COMPONENT_LIB)
        # C/C++ only: main also compiles generated .S embeds (ca_cert.pem.S)
        # where a forced C header include breaks the assembler.
        target_compile_options(${component_lib} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${STOCK_TEST_COMPAT_HEADER}>")
    endforeach()
    idf_component_get_property(stock_tbr_lib thread_border_router COMPONENT_LIB)
    idf_component_get_property(stock_compat_lib openthread_start_compat COMPONENT_LIB)
    target_link_libraries(${stock_tbr_lib} PUBLIC ${stock_compat_lib})
endif()
