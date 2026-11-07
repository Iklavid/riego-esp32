# Smart irrigation with ESP32

[Español](README.md)

An irrigation system for a home garden built on ESP32 boards, MQTT and a small server (a Mac or Linux machine). It waters by zone using soil moisture, rain and the weather forecast, and it alerts you when something fails. Code comments and the web interface are in Spanish.

- `firmware/s3-central`: ESP32-S3 with a touch screen (LVGL) and 8 valve relays.
- `firmware/sensores-devkit`: rain, tank level and street inlet valve.
- `firmware/puente-c3`: ESP-NOW (and 433 MHz) to MQTT bridge, which also updates the nodes over the air.
- `firmware/nodo-humedad`: battery soil-moisture nodes (ESP32-C3 SuperMini) that can also act as repeaters.
- `gestor/`: password-protected web (FastAPI) with a weather-aware "brain" (Open-Meteo, optional AEMET).
- `servidor/`: Mosquitto with per-board ACLs, a watchdog that restarts services and sends alerts, node OTA and a node simulator.

Each board reads its Wi-Fi and passwords from `include/secretos.h`; copy it from `secretos.example.h`. That file never goes to Git.

## License
**Free for personal and other noncommercial use** under the [PolyForm Noncommercial License 1.0.0](LICENSE.md). Selling it, installing it for customers or using it in a business needs a paid commercial license: contact me on [GitHub](https://github.com/Iklavid).
Because it restricts commercial use, this license is not "open source" under the OSI definition; the source code is still public.
