cmake_minimum_required(VERSION 3.16)

file(GLOB SOURCES "*.cpp" "*.c")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS
        "."
        "../lib/cspot/cspot/include"
        "../lib/cspot/cspot/bell/main/audio-sinks/include"
        "../lib/cspot/cspot/bell/main/utilities/include"
        "../lib/cspot/cspot/bell/main/io/include"
        "../lib/cspot/cspot/bell/external/civetweb/include"
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        spiffs
        mdns
        freertos
        esp_netif
        mbedtls
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
)

# Build cspot + bell as plain CMake static libraries
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/cspot/cspot"
    "${CMAKE_CURRENT_BINARY_DIR}/cspot_build"
    EXCLUDE_FROM_ALL
)

# Bell is a plain CMake lib; provide mbedtls headers it needs.
# IDF 6.0 uses mbedtls 4.0 which moved crypto headers to private/;
# compat/ has forwarding shims so bell's old #include <mbedtls/aes.h> still works.
if(TARGET bell)
    # port/include must come BEFORE tf-psa-crypto/drivers/builtin/include because
    # port/include/mbedtls/bignum.h uses #include_next "mbedtls/private/bignum.h"
    # which searches paths AFTER port/include — those must include builtin/include.
    target_include_directories(bell PUBLIC
        "${CMAKE_SOURCE_DIR}/compat"
        "$ENV{IDF_PATH}/components/mbedtls/mbedtls/include"
        "$ENV{IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/include"
        "$ENV{IDF_PATH}/components/mbedtls/port/include"
        "$ENV{IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/include"
        "${CMAKE_BINARY_DIR}/config"
    )
    # mbedtls 4.0: md_hmac_* and other APIs are gated on MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS.
    # IDF port headers define this in their wrappers; bell needs it globally.
    target_compile_definitions(bell PUBLIC MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)
endif()

target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++17
    -fdiagnostics-color=always
    -Wno-missing-field-initializers
)

# Bell's external libraries are old C code; suppress all warnings to avoid -Werror
foreach(_ext_lib opencore-aacdec opus tremor civetweb mdnssvc bell_nanopb civetweb-cpp bell)
    if(TARGET ${_ext_lib})
        target_compile_options(${_ext_lib} PRIVATE -w)
    endif()
endforeach()

# IDF 6.0 picolibc already provides timegm; tell civetweb (part of bell) not to redefine it
if(TARGET bell)
    target_compile_definitions(bell PRIVATE HAVE_TIMEGM)
endif()

# GCC 15.2.0 ICE workaround: -fzero-init-padding-bits=all triggers an ICE in esp_lcd_panel_rgb.c
# at the RTL/ira pass. Suppress it for esp_lcd only.
if(TARGET __idf_esp_lcd)
    target_compile_options(__idf_esp_lcd PRIVATE -fzero-init-padding-bits=standard)
endif()
