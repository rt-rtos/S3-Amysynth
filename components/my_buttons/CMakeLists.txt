idf_component_register(
    SRCS "my_buttons.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
    PRIV_REQUIRES espressif__button
)
