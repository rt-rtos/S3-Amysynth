idf_component_register(
    SRCS "rotary_encoder.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_pcnt esp_driver_gpio
)