# i2c_u8g2 (ESP-IDF component)

Drop-in hardware-I2C U8G2 component extracted from the demo project, without demo rendering routines.

## What it provides

- ESP-IDF I2C master bus/device setup
- U8G2 byte + gpio/delay callbacks
- U8G2 display initialization and power control
- Public API to get `u8g2_t*` and render from your app

## Files

- `include/i2c_u8g2.h`: public API
- `i2c_u8g2.c`: implementation
- `Kconfig`: menuconfig options
- `CMakeLists.txt`: component registration
- `idf_component.yml`: dependency metadata

## Quick usage

```c
#include "i2c_u8g2.h"

void app_main(void)
{
    i2c_u8g2_handle_t display;
    i2c_u8g2_config_t cfg = i2c_u8g2_config_default();

    // Optional: use your own display setup function
    // cfg.setup_fn = u8g2_Setup_sh1106_i2c_128x64_noname_f;

    ESP_ERROR_CHECK(i2c_u8g2_init(&display, &cfg));

    u8g2_t *u8g2 = i2c_u8g2_get_u8g2(&display);
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_ncenB08_tr);
    u8g2_DrawStr(u8g2, 0, 12, "Hello");
    u8g2_SendBuffer(u8g2);
}
```

## Notes

- Default setup is `u8g2_Setup_ssd1306_i2c_128x64_noname_f`.
- Change `cfg.setup_fn` if your controller differs.
- For multi-task access, guard U8G2 calls with your own mutex.