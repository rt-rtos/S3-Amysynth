idf_component_register(SRCS
    "main.c"
    "input_dispatch.c"
    "render_clock.c"
    "render_clock_i2s.c"
    PRIV_REQUIRES seq_clamp spi_flash esp_partition esp_driver_i2s esp_driver_i2c esp_adc esp_driver_uart esp_timer esp_driver_gptimer display rotary_encoder u8g2 synth_core usb_audio my_buttons project_store wireless diagnostics status_led harness project_xfer
    REQUIRES amy
    INCLUDE_DIRS "." 
)

target_compile_options(${COMPONENT_TARGET} PUBLIC
    -Wno-uninitialized
    -DESP_PLATFORM
)
# -Wstack-usage is GCC-only; clang rejects the unknown warning option and IDF
# builds with -Werror, so it must not reach a clang build. The 1 KB stack budget
# check still applies under the shipping toolchain.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")
endif()
# `amy.c` needs relaxed strict-aliasing warnings; handled by the `amy` component.