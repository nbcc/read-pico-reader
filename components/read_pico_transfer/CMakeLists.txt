include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake")
set(weread_sources
    "weread/read_pico_weread.c"
    "weread/weread_protocol.c"
    "weread/weread_session.c"
    "weread/weread_http.c"
    "weread/weread_json.c"
    "weread/weread_shelf.c"
    "weread/weread_bookpath.c"
    "weread/weread_content.c"
    "weread/weread_toc.c"
    "weread/weread_xhtml.c"
    "weread/weread_zip.c")
if(KIIKO_BOARD_METALIO47)
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 独立热点传书组件与嵌入页面；微信读书作为第二个内容来源。/ Standalone transfer component with a second WeRead source.
kiiko_board_sources(kiiko_selected_sources "${CMAKE_CURRENT_LIST_DIR}" "read_pico_transfer.c" "transfer_http_time.c" "transfer_ntp_time.c" ${weread_sources})
idf_component_register(SRCS ${kiiko_selected_sources}
    INCLUDE_DIRS "include" "weread"
    REQUIRES esp_app_format ble_page_turner read_pico_search esp_wifi esp_netif esp_event esp_http_server esp_http_client mbedtls lwip nvs_flash fatfs esp_timer espressif__cjson
    EMBED_TXTFILES "upload.html")
option(METALIO_NETWORK_PROBE "Local S31 read-only radio diagnostics" OFF)
if(METALIO_NETWORK_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_LOCAL_NETWORK_PROBE=1)
endif()

else()
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 独立热点传书组件与嵌入页面；微信读书作为第二个内容来源。/ Standalone transfer component with a second WeRead source.
kiiko_board_sources(kiiko_selected_sources "${CMAKE_CURRENT_LIST_DIR}" "read_pico_transfer.c" ${weread_sources})
idf_component_register(SRCS ${kiiko_selected_sources}
    INCLUDE_DIRS "include" "weread"
    REQUIRES ble_page_turner read_pico_search esp_wifi esp_netif esp_event esp_http_server esp_http_client
             mbedtls nvs_flash fatfs esp_timer espressif__cjson
    EMBED_TXTFILES "upload.html")

endif()
