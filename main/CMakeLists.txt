# 摊主胸牌内容:若存在 badge_content_local.c(已被 .gitignore 忽略,放你自己的店名和收款码)
# 就用它代替仓库里的示例内容。改内容后先运行 tools/gen_badge_fonts.py 重新生成字库。
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/badge_content_local.c")
    set(BADGE_CONTENT_SRC "badge_content_local.c")
else()
    set(BADGE_CONTENT_SRC "badge_content.c")
endif()

idf_component_register(
    SRCS "main.c"
         "badge_ledger.c"
         "badge_nav.c"
         "badge_fmt.c"
         "badge_store.c"
         "badge_sfx.c"
         "badge_ui.c"
         "${BADGE_CONTENT_SRC}"
         "../assets/fonts/badge_font_16.c"
         "../assets/fonts/badge_font_20.c"
         "../assets/fonts/badge_font_32.c"
         "../assets/fonts/badge_num_48.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)
