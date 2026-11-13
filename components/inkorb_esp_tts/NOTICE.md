# Official Espressif ESP-TTS subset

Pinned upstream revision is recorded in UPSTREAM.json with per-file hashes.
Only the ESP32-S3 Chinese synthesis engine and Xiaole voice are included.
No managed component is edited and no speech recognition model is imported.

Root license: ESPRESSIF MIT License, permission is free for use on Espressif products.
The esp_tts.h interface also contains its upstream Apache-2.0 notice, preserved as-is.
Engine and voice are supplied upstream as prebuilt libraries; their complete implementation source was not available for audit. No claim of a complete code security audit or Socket scan is made.
