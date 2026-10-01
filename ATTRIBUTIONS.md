# Third-party attributions

This project's own code is released under the MIT License (see `LICENSE`).
This repository also contains, and the firmware is built against, work by
others. Each item below keeps its own licence. The MIT License in `LICENSE`
does not apply to it.

## Included in this repository

| Component | Path | Origin | Licence | Licence text |
| --- | --- | --- | --- | --- |
| AMY synthesis engine | `components/amy/`, and the `amy` submodule (unmodified upstream) | [shorepine/amy](https://github.com/shorepine/amy), Brian Whitman and Daniel PW Ellis | MIT | `components/amy/LICENSE` |
| miniaudio (part of AMY) | `components/amy/src/miniaudio.h` | [mackron/miniaudio](https://github.com/mackron/miniaudio), David Reid | Public domain (Unlicense) or MIT No Attribution, at your choice | end of the file |
| pico-audio (part of AMY) | `components/amy/src/pico-audio/` | Raspberry Pi (Trading) Ltd. | BSD-3-Clause | file headers |
| U8g2 display library | `components/u8g2/` | [olikraus/u8g2](https://github.com/olikraus/u8g2), Oliver Kraus | BSD-2-Clause | `components/u8g2/LICENSE` |
| usb_device_uac | `components/usb_device_uac/` | [espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution), Espressif Systems | Apache-2.0 | `components/usb_device_uac/license.txt` |
| Button glue | `components/my_buttons/` | based on Espressif Systems example code | Apache-2.0 | file headers; full text in `components/usb_device_uac/license.txt` |
| GCC LTO helper | `cmake/gcc_lto.cmake` | adapted from `gcc.cmake` in [espressif/cmake_utilities](https://components.espressif.com/components/espressif/cmake_utilities), Espressif Systems | Apache-2.0 | full text in `components/usb_device_uac/license.txt` |

Notes:

- **AMY is modified.** The copy in `components/amy/` carries local changes,
  marked `// LOCAL EDIT` in the source and listed in `AMY-EDITS.md`. The `amy`
  submodule is the unmodified upstream it is compared against.
- **usb_device_uac is modified.** The changes are listed in `UAC-EDITS.md`.
- **Fonts.** The firmware uses three U8g2 fonts: `u8g2_font_4x6`,
  `u8g2_font_5x7` and `u8g2_font_6x10`. They come from the X11 misc-fixed
  collection, which is in the public domain. The U8g2 source tree contains
  many more fonts under their own terms; see
  <https://github.com/olikraus/u8g2/wiki/fntgrp>.
- **pico-audio** is part of the AMY source tree and is not built for the
  ESP32-S3.
- **Sample and patch data** in `components/amy/src/` (PCM samples, the Juno,
  DX7 and piano patch data) are distributed as part of AMY. The files carry no
  separate notice.

## Fetched at build time

These are not stored in this repository. The ESP-IDF component manager
downloads them, and they are compiled into the firmware. Versions are pinned
in `dependencies.lock`.

| Component | Version | Origin | Licence |
| --- | --- | --- | --- |
| ESP-IDF | 6.1.0 | [espressif/esp-idf](https://github.com/espressif/esp-idf), Espressif Systems | Apache-2.0, with bundled third-party components under their own licences |
| espressif/tinyusb | 0.19.0~3 | [hathach/tinyusb](https://github.com/hathach/tinyusb), Ha Thach, packaged by Espressif | MIT |
| espressif/button | 4.2.0 | Espressif Systems | Apache-2.0 |
| espressif/led_strip | 3.0.3 | Espressif Systems | Apache-2.0 |
| espressif/cmake_utilities | 1.1.1 | Espressif Systems | Apache-2.0 |
| joltwallet/littlefs | 1.22.3 | [joltwallet/esp_littlefs](https://github.com/joltwallet/esp_littlefs), Brian Pugh | MIT |
| littlefs (inside joltwallet/littlefs) | bundled | [littlefs-project/littlefs](https://github.com/littlefs-project/littlefs), the littlefs authors and Arm Limited | BSD-3-Clause |

ESP-IDF bundles further third-party software, among it the FreeRTOS kernel
(MIT) and Apache NimBLE (Apache-2.0). Its full list is at
<https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/COPYRIGHT.html>.

## Distributing a built firmware image

A firmware binary built from this repository contains code from every
component above. The MIT, BSD and Apache-2.0 licences require their copyright
and licence notices to accompany binary distributions, so ship this file and
the licence texts it points to together with the image.
