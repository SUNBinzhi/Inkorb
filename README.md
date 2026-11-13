# Inkorb

[English](README.md) | [简体中文](README_ZH.md)

A round-screen pocket AI companion and DJI camera remote, built on the
Waveshare ESP32-S3 1.75" round AMOLED board. ESP-IDF 5.5 + LVGL 9.5.

## Hardware

| Part | Detail |
| --- | --- |
| Board | Waveshare **ESP32-S3-Touch-AMOLED-1.75C** |
| SoC | ESP32-S3R8, dual-core 240 MHz, 8 MB PSRAM, 32 MB flash |
| Display | 1.75" round AMOLED, 466 × 466, CO5300 driver over QSPI, capacitive touch |
| Audio in | ES7210 ADC + dual microphones |
| Audio out | ES8311 codec + speaker |
| Power | AXP2101 PMIC, Li-ion battery, USB-C charging |
| Radio | 2.4 GHz Wi-Fi + Bluetooth LE 5 |
| Keys | Big key (talk / record), small key (screen on/off, long-press power) |

## Features

**Voice assistant "Inkorb"**
- Push-to-talk (big key or on-screen button), barge-in to interrupt replies.
- Cloud pipeline over HTTPS straight from the device: speech-to-text →
  LLM (with web search and tool calls) → streaming text-to-speech.
- Short-term chat context plus long-term memory saved across reboots.
- Device tools the assistant can call: set volume, set brightness, read
  device status.

**DJI camera remote (Bluetooth LE)**
- Works with cameras that speak DJI's open remote protocol
  (tested: Osmo Action 5, Osmo 360).
- Search, pair, remember up to 3 cameras, switch between them.
- Big shutter button with record timer; shows resolution, frame rate,
  shooting mode and camera battery; pops up overheat warnings.
- Sleep / wake the camera remotely (wake works on cameras put to sleep,
  within 30 min).
- Bluetooth only runs while this page is open.

**System**
- Watch-face home screen, boot animation, battery and charging indicator.
- Multiple saved Wi-Fi networks; set up a new one from your phone through a
  captive portal.
- Wi-Fi turns on only when an app needs it and powers down when idle.
- Round settings dial (volume, brightness) and a usage dashboard
  (local usage stats, LLM account balance).
- Screen-off power saving: CPU frequency scaling, panel sleep, mic off,
  key wake.

## Build

Requires ESP-IDF 5.5.x.

```bash
cp main/voice_cloud_config.h.example main/voice_cloud_config.h   # add your own API keys
cp main/voice_bridge_config.h.example main/voice_bridge_config.h
cp main/wifi_credentials.h.example main/wifi_credentials.h       # optional default Wi-Fi
idf.py set-target esp32s3
idf.py build flash monitor
```

The three copied headers are git-ignored. **API keys end up inside the
firmware image, so never share a built binary that contains real keys.**
More detail on the voice pipeline: [voice_cloud/README.md](voice_cloud/README.md).

## Layout

| Path | What |
| --- | --- |
| `main/` | Firmware: UI pages (`inkorb_*_ui.c`), voice, Wi-Fi, DJI remote (`inkorb_action.c`), power |
| `components/inkorb_esp_tts` | Offline TTS fallback (upstream, see its NOTICE) |
| `voice_cloud/` | Key setup scripts and notes for the cloud voice pipeline |
| `voice_bridge/` | Optional older LAN bridge server (not needed for the cloud pipeline) |
| `sim/` | Desktop LVGL simulator for UI work |

## Credits

DJI remote protocol per DJI's open-source
[Osmo-GPS-Controller-Demo](https://github.com/dji-sdk/Osmo-GPS-Controller-Demo).
Noto Sans SC font under the SIL OFL (`fonts/OFL.txt`).
