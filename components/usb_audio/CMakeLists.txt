idf_component_register(SRCS "usb_audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES usb_device_uac
                       PRIV_REQUIRES espressif__tinyusb)
