# Inkorb Mac/SDL simulator

与固件共用 `main/inkorb_ui.c` + Press Start 2P（标题 24 / 时间 48 / 日期·IP 16）。

```bash
cd .../02_lvgl_demo_v9/sim
cmake -S . -B build && cmake --build build -j
./build/inkorb_sim                 # 466 窗口
./build/inkorb_sim --shot out.ppm  # 无头导出像素
```

依赖：Homebrew `sdl2` / `sdl2-compat`（`sdl2-config` 在 PATH）。

## Boot screen preview

```bash
./build/inkorb_sim --boot                         # animated INKORB orbit
./build/inkorb_sim --boot --shot boot.ppm
./build/inkorb_sim --boot-ready --shot ready.ppm
./build/inkorb_sim --boot-offline --shot offline.ppm
```

Firmware keeps the splash visible until Wi-Fi/IP, synchronized time, the first
battery read attempt, and microphone initialization have completed. After 20
seconds it offers `Continue offline` while connection attempts continue. The
home screen is constructed and populated together, before its first render.
