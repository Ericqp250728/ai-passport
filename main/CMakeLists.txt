idf_component_register(
    SRCS "main.c"
         "ancs_proto.c"
         "notif_model.c"
         "mirror_nav.c"
         "mirror_ble.c"
         "mirror_ui.c"
         "../assets/fonts/mirror_font_16.c"
         "../assets/fonts/mirror_font_24.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_timer nvs_flash
)
