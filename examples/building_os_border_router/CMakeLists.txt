cmake_minimum_required(VERSION 3.16)

# Building OS Border Router firmware.
# This example lives inside the Building OS fork of espressif/esp-thread-br.
# The Building OS architecture spec lives in the Building OS repository at
# docs/08.8-border-router.md.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR
        "IDF_PATH is not set.\n"
        "Export ESP-IDF v5.5.2 before building examples/building_os_border_router.")
endif()

# Use this fork repository as the upstream esp-thread-br component source.
set(BOS_BR_VENDOR_PATH "${CMAKE_SOURCE_DIR}/../..")
set(BOS_BR_THREAD_BORDER_ROUTER_PATH "${BOS_BR_VENDOR_PATH}/examples/common/thread_border_router")
set(BOS_BR_WEB_FRONTEND_DIR "${CMAKE_SOURCE_DIR}/web_frontend" CACHE PATH "Building OS web frontend for the upstream BR HTTP server")
set(BOS_BR_PROTOCOL_EXAMPLES_PATH "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")
set(BOS_BR_OT_EXAMPLES_COMMON_PATH "$ENV{IDF_PATH}/examples/openthread/ot_common_components/ot_examples_common")
set(BOS_BR_LOCAL_OT_EXAMPLES_COMMON_PATH "${CMAKE_SOURCE_DIR}/local_helpers/ot_examples_common")
set(BOS_BR_OPENTHREAD_COMPAT_PATH "${CMAKE_SOURCE_DIR}/local_helpers/openthread_start_compat")
set(BOS_BR_NEEDS_OPENTHREAD_START_COMPAT OFF CACHE INTERNAL "Whether this ESP-IDF needs esp_openthread_start compatibility")

if(NOT IS_DIRECTORY "${BOS_BR_VENDOR_PATH}/components")
    message(FATAL_ERROR
        "esp-thread-br repository root not found at ${BOS_BR_VENDOR_PATH}.\n"
        "Build this example from inside the dan99git/esp-thread-br fork.")
endif()

if(NOT IS_DIRECTORY "${BOS_BR_PROTOCOL_EXAMPLES_PATH}")
    message(FATAL_ERROR
        "ESP-IDF protocol_examples_common missing at ${BOS_BR_PROTOCOL_EXAMPLES_PATH}.")
endif()

if(NOT IS_DIRECTORY "${BOS_BR_THREAD_BORDER_ROUTER_PATH}")
    message(FATAL_ERROR
        "Upstream thread_border_router helper missing at ${BOS_BR_THREAD_BORDER_ROUTER_PATH}.")
endif()

if(IS_DIRECTORY "${BOS_BR_OT_EXAMPLES_COMMON_PATH}")
    set(BOS_BR_OT_EXAMPLES_COMMON_COMPONENT "${BOS_BR_OT_EXAMPLES_COMMON_PATH}")
else()
    set(BOS_BR_OT_EXAMPLES_COMMON_COMPONENT "${BOS_BR_LOCAL_OT_EXAMPLES_COMMON_PATH}")
endif()

set(BOS_BR_ESP_OPENTHREAD_HEADER "$ENV{IDF_PATH}/components/openthread/include/esp_openthread.h")
if(EXISTS "${BOS_BR_ESP_OPENTHREAD_HEADER}")
    file(READ "${BOS_BR_ESP_OPENTHREAD_HEADER}" BOS_BR_ESP_OPENTHREAD_HEADER_CONTENT)
    if(NOT BOS_BR_ESP_OPENTHREAD_HEADER_CONTENT MATCHES "esp_openthread_start")
        set(BOS_BR_NEEDS_OPENTHREAD_START_COMPAT ON CACHE INTERNAL "Whether this ESP-IDF needs esp_openthread_start compatibility" FORCE)
    endif()
endif()

# Building OS shared firmware component library
# (docs/08.2-base-firmware-architecture.md section 7): individual shared
# components are appended by RELATIVE path (never the whole shared-components
# directory, so the bench-only sdmmc shadow is not pulled in). This keeps the
# fork mergeable against espressif/esp-thread-br upstream - the only
# fork-facing change is additive lines in this example CMakeLists.txt.
set(BOS_BR_SHARED_COMPONENTS_PATH "${CMAKE_SOURCE_DIR}/../../../../shared-components")
if(NOT IS_DIRECTORY "${BOS_BR_SHARED_COMPONENTS_PATH}/bos_thread_diag")
    message(FATAL_ERROR
        "Building OS shared component bos_thread_diag missing at "
        "${BOS_BR_SHARED_COMPONENTS_PATH}/bos_thread_diag.\n"
        "Build this example from the vendor/esp-thread-br checkout inside the building-os repository.")
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    "${BOS_BR_VENDOR_PATH}/components"
    "${BOS_BR_THREAD_BORDER_ROUTER_PATH}"
    "${BOS_BR_PROTOCOL_EXAMPLES_PATH}"
    "${BOS_BR_OT_EXAMPLES_COMMON_COMPONENT}"
    "${BOS_BR_SHARED_COMPONENTS_PATH}/bos_thread_diag"
    "${CMAKE_SOURCE_DIR}/components")

if(BOS_BR_NEEDS_OPENTHREAD_START_COMPAT)
    list(APPEND EXTRA_COMPONENT_DIRS "${BOS_BR_OPENTHREAD_COMPAT_PATH}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(building-os-border-router)

if(BOS_BR_NEEDS_OPENTHREAD_START_COMPAT)
    set(BOS_BR_OPENTHREAD_COMPAT_HEADER
        "${BOS_BR_OPENTHREAD_COMPAT_PATH}/include/bos_openthread_compat.h")
    foreach(component_name main thread_border_router)
        idf_component_get_property(component_lib ${component_name} COMPONENT_LIB)
        target_compile_options(${component_lib} PRIVATE
            "-include" "${BOS_BR_OPENTHREAD_COMPAT_HEADER}")
    endforeach()
    idf_component_get_property(thread_border_router_lib thread_border_router COMPONENT_LIB)
    idf_component_get_property(openthread_start_compat_lib openthread_start_compat COMPONENT_LIB)
    target_link_libraries(${thread_border_router_lib} PUBLIC ${openthread_start_compat_lib})
endif()

set(BOS_BR_APP_VERSION "0.1.0" CACHE STRING "Semver of this Building OS BR firmware build")
