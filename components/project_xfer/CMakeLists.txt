idf_component_register(
    SRCS "project_xfer.c" "project_xfer_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES project_store
    PRIV_REQUIRES synth_core esp_driver_uart
)
