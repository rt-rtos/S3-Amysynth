idf_component_register(SRCS "sequencer_core.c" "sequencer_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES u8g2 priv_i2c_u8g2 amy)
