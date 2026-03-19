file(GLOB AMY_SRC_FILES LIST_DIRECTORIES OFF "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

# Exclude standalone examples and non-ESP drivers
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "amy-example\\.c$")
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "libminiaudio-audio\\.c$")

if(NOT AMY_SRC_FILES)
    message(FATAL_ERROR "No AMY sources found in ${CMAKE_CURRENT_LIST_DIR}/src. Make sure the `src` directory exists within the component.")
endif()

idf_component_register(
    SRCS ${AMY_SRC_FILES}
    INCLUDE_DIRS "src"
    REQUIRES esp_timer esp_driver_uart esp_driver_i2s esp_driver_i2c esp_driver_gpio
)

# Keep the same compile flag for amy.c (relax strict-aliasing warnings)
set(AMY_CORE_C "${CMAKE_CURRENT_LIST_DIR}/src/amy.c")
if(EXISTS ${AMY_CORE_C})
    set_source_files_properties(${AMY_CORE_C} PROPERTIES COMPILE_FLAGS "-Wno-strict-aliasing")
endif()
