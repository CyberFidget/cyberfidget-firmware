# Third-Party Licenses

Cyber Fidget firmware uses the following open-source libraries. We are grateful to their authors.

| Library | Version | License | URL |
|---|---|---|---|
| arduino-audio-tools | 1.2.0 | GPL-3.0 | https://github.com/pschatzmann/arduino-audio-tools |
| arduino-libhelix | 0.8.7 | GPL-3.0 | https://github.com/pschatzmann/arduino-libhelix |
| ESP32-A2DP | 1.8.3 | Apache-2.0 | https://github.com/pschatzmann/ESP32-A2DP |
| Adafruit NeoPixel | 1.12.3 | LGPL-3.0 | https://github.com/adafruit/Adafruit_NeoPixel |
| AsyncTCP | 3.4.9 | LGPL-3.0 | https://github.com/mathieucarbou/AsyncTCP |
| ESPAsyncWebServer | 3.6.0 | LGPL-3.0 | https://github.com/mathieucarbou/ESPAsyncWebServer |
| ESP8266/ESP32 OLED SSD1306 | 4.6.1 | MIT | https://github.com/ThingPulse/esp8266-oled-ssd1306 |
| SparkFun LIS2DH12 | 1.0.3 | MIT | https://github.com/sparkfun/SparkFun_LIS2DH12_Arduino_Library |
| SparkFun MAX1704x | 1.0.4 | MIT | https://github.com/sparkfun/SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library |
| WiFiManager | 2.0.17 | MIT | https://github.com/tzapu/WiFiManager |
| wasm3 | d77cd814 (vendored in lib/wasm3, spike/wasm3-apps) | MIT | https://github.com/wasm3/wasm3 |

## Note on arduino-libhelix

The `arduino-libhelix` library wraps RealNetworks' Helix DNA MP3 decoder. The inner `libhelix-mp3` code is derived from the RealNetworks Public Source License (RPSL), which is an OSI-approved open-source license. The wrapper is distributed under GPL-3.0 by its author. RPSL terms are satisfied through this attribution.
