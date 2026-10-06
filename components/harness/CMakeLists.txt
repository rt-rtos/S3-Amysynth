idf_component_register(
    SRCS "harness.c" "harness_exec.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES synth_core diagnostics usb_device_uac esp_driver_uart esp_app_format
)
