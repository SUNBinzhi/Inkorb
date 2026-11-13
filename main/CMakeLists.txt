idf_component_register(
    SRCS main.c inkorb_action.c wifi_sta.c inkorb_ui.c inkorb_boot.c inkorb_mic.c inkorb_battery.c inkorb_device.c inkorb_voice.c inkorb_cloud.c inkorb_duplex_tts.c inkorb_local_tts.c inkorb_chat.c inkorb_wifi_ui.c inkorb_settings_ui.c inkorb_usage_ui.c inkorb_round.c inkorb_usage.c inkorb_portal.c inkorb_display.c lv_font_inkorb_cn18.c inkorb_pixel_text.c lv_font_montserrat_time_72.c inkorb_time_bloom_img.c
    INCLUDE_DIRS .)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=cpp)

idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
