# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 独立热点传书组件与嵌入页面；微信读书作为第二个内容来源。/ Standalone transfer component with a second WeRead source.
set(kiiko_selected_sources
    "read_pico_transfer.c"
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
idf_component_register(SRCS ${kiiko_selected_sources}
    INCLUDE_DIRS "include" "weread"
    REQUIRES ble_page_turner read_pico_search esp_wifi esp_netif esp_event esp_http_server esp_http_client
             mbedtls nvs_flash fatfs esp_timer espressif__cjson
    EMBED_TXTFILES "upload.html")
