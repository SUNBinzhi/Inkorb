idf_component_register(INCLUDE_DIRS include EMBED_FILES voice_xiaole.dat)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/lib/libesp_tts_chinese.a"
    "${CMAKE_CURRENT_LIST_DIR}/lib/libvoice_set_xiaole.a")
