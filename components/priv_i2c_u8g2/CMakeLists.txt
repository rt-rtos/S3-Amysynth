idf_component_register(
    SRCS "priv_i2c_u8g2.c" "priv_u8g2_seq.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio u8g2
)
