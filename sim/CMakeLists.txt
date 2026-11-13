cmake_minimum_required(VERSION 3.16)
project(inkorb_sim C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LVGL_DIR "${PROJ_ROOT}/managed_components/lvgl__lvgl")
set(MAIN_DIR "${PROJ_ROOT}/main")

execute_process(COMMAND sdl2-config --cflags OUTPUT_VARIABLE SDL_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND sdl2-config --libs OUTPUT_VARIABLE SDL_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(SDL_CFLAGS_LIST NATIVE_COMMAND "${SDL_CFLAGS}")
separate_arguments(SDL_LIBS_LIST NATIVE_COMMAND "${SDL_LIBS}")

set(SDL_INC_DIRS "")
foreach(flag ${SDL_CFLAGS_LIST})
  if(flag MATCHES "^-I(.*)")
    list(APPEND SDL_INC_DIRS "${CMAKE_MATCH_1}")
  endif()
endforeach()
# Also parent include so <SDL2/SDL.h> works if needed
list(APPEND SDL_INC_DIRS "/opt/homebrew/include")

add_compile_options(-Wall -Wextra -O2)

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS
    "${LVGL_DIR}/src/*.c"
)

add_library(lvgl_sim STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_sim PUBLIC
    "${LVGL_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MAIN_DIR}"
    ${SDL_INC_DIRS}
)
target_compile_definitions(lvgl_sim PUBLIC "LV_CONF_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h\"")

add_executable(inkorb_sim
    main_sim.c
    "${MAIN_DIR}/inkorb_ui.c"
    "${MAIN_DIR}/inkorb_boot.c"
    "${MAIN_DIR}/inkorb_chat.c"
    "${MAIN_DIR}/inkorb_wifi_ui.c"
    "${MAIN_DIR}/inkorb_settings_ui.c"
    "${MAIN_DIR}/inkorb_usage_ui.c"
    "${MAIN_DIR}/inkorb_round.c"
    "${MAIN_DIR}/lv_font_inkorb_cn18.c"
    "${MAIN_DIR}/inkorb_mic.c"
    "${MAIN_DIR}/inkorb_pixel_text.c"
    "${MAIN_DIR}/lv_font_montserrat_time_72.c"
    "${MAIN_DIR}/inkorb_time_bloom_img.c"
)
target_include_directories(inkorb_sim PRIVATE
    "${MAIN_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LVGL_DIR}"
    ${SDL_INC_DIRS}
)
target_compile_definitions(inkorb_sim PRIVATE "LV_CONF_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h\"")
target_link_libraries(inkorb_sim PRIVATE lvgl_sim ${SDL_LIBS_LIST})
