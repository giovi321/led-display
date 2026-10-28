## cJSON: in IDF <6.0 it lives in the built-in `json` core component; in
## IDF >=6.0 it has been extracted to the `espressif/cjson` managed component
## (which the manager pulls in via main/idf_component.yml).
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(_cjson_dep espressif__cjson)
else()
    set(_cjson_dep json)
endif()

## esp-mqtt: same split - built-in `mqtt` core component in IDF <6.0, the
## `espressif/mqtt` managed component (via main/idf_component.yml) from 6.0.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(_mqtt_dep espressif__mqtt)
else()
    set(_mqtt_dep mqtt)
endif()

## ---- web assets ----------------------------------------------------------
## The web UI lives as editable source files in main/web/ and is gzipped at
## configure time (deterministic, mtime=0) into the build dir, then embedded.
## CMAKE_CONFIGURE_DEPENDS makes an asset edit re-trigger the configure step.
idf_build_get_property(_python PYTHON)
if(NOT _python)
    set(_python python3)
endif()

set(_web_dir    ${COMPONENT_DIR}/web)
set(_web_gz_dir ${CMAKE_CURRENT_BINARY_DIR}/webgz)
file(MAKE_DIRECTORY ${_web_gz_dir})

## favicon.svg is a copy of docs/public/favicon.svg. It has to live here: the
## WSL build helper mirrors only Firmware/, so a path reaching outside it would
## not resolve. Keep the two in step by hand if the icon ever changes.
set(_web_assets index.html style.css app.js editor.js favicon.svg)
set(_web_gz_files "")
foreach(_asset ${_web_assets})
    execute_process(
        COMMAND ${_python} ${COMPONENT_DIR}/../tools/gzip_asset.py
                ${_web_dir}/${_asset} ${_web_gz_dir}/${_asset}.gz
        RESULT_VARIABLE _gz_rc)
    if(NOT _gz_rc EQUAL 0)
        message(FATAL_ERROR "gzip of web asset ${_asset} failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${_web_dir}/${_asset})
    list(APPEND _web_gz_files ${_web_gz_dir}/${_asset}.gz)
endforeach()

idf_component_register(
    SRCS
        "main.c"
        "mapping.c"
        "valuefmt.c"
        "fallback.c"
        "jsonpath.c"
        "tz.c"
        "tzdata.c"
        "fonts.c"
        "devname.c"
        "light_curve.c"
        "power_model.c"
        "light_sensor.c"
        "led_driver.c"
        "display.c"
        "wifi_manager.c"
        "wifi_pick.c"
        "settings.c"
        "scheduler.c"
        "sched_window.c"
        "integrations.c"
        "value_feed.c"
        "value_poll.c"
        "webhooks.c"
        "ota_update.c"
        "wg_client.c"
        "mqtt_link.c"
        "web_server.c"
        "web_common.c"
        "web_static.c"
        "api_display.c"
        "api_layout.c"
        "api_colors.c"
        "api_wifi.c"
        "api_system.c"
        "api_config.c"
        "api_ota.c"
        "api_integrations.c"
        "api_value_poll.c"
    INCLUDE_DIRS "."
    EMBED_FILES ${_web_gz_files}
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_http_client
        esp_https_ota
        app_update
        mbedtls
        esp_timer
        esp_adc
        esp_driver_i2c
        ${_cjson_dep}
        lwip
        driver
        led_strip
        esp_wireguard
        ${_mqtt_dep}
)
