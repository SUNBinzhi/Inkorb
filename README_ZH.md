# Inkorb

[English](README.md) | [简体中文](README_ZH.md)

一个圆屏的口袋 AI 伙伴 + DJI 相机遥控器，基于微雪 ESP32-S3 1.75 寸圆形 AMOLED 开发板。ESP-IDF 5.5 + LVGL 9.5。

## 硬件

| 部件 | 说明 |
| --- | --- |
| 开发板 | 微雪 **ESP32-S3-Touch-AMOLED-1.75C** |
| 主控 | ESP32-S3R8，双核 240 MHz，8 MB PSRAM，32 MB Flash |
| 屏幕 | 1.75 寸圆形 AMOLED，466×466，CO5300 驱动（QSPI），电容触摸 |
| 录音 | ES7210 + 双麦克风 |
| 播放 | ES8311 + 喇叭 |
| 电源 | AXP2101 电源管理，锂电池，USB-C 充电 |
| 无线 | 2.4 GHz Wi-Fi + 蓝牙 LE 5 |
| 按键 | 大键（说话/录制）、小键（亮灭屏，长按关机） |

## 功能

**语音助手 Inkorb**
- 按住大键或屏幕按钮说话，可打断正在播放的回答。
- 设备直接通过 HTTPS 调用云端：语音识别 → 大模型（支持联网搜索、工具调用）→ 流式语音合成。
- 短期对话上下文 + 跨重启的长期记忆。
- 助手可调用设备工具：调音量、调亮度、读取设备状态。

**DJI 相机遥控（蓝牙 LE）**
- 支持使用 DJI 开放遥控协议的相机（已实测 Osmo Action 5、Osmo 360）。
- 搜索、配对、记住最多 3 台相机，可切换。
- 圆心大快门 + 录制计时；显示分辨率、帧率、拍摄模式、相机电量；过热时弹出提醒。
- 远程休眠 / 唤醒（唤醒只对被休眠且不超过 30 分钟的相机有效）。
- 只有打开该页面时才开启蓝牙。

**系统**
- 表盘主页、开机动画、电量与充电指示。
- 多个 Wi-Fi 记忆；用手机连设备热点即可配置新 Wi-Fi。
- Wi-Fi 按需开启，闲置自动关闭。
- 圆形设置盘（音量、亮度）和用量面板（本地用量统计、大模型账户余额）。
- 息屏省电：CPU 降频、屏幕休眠、关麦克风、按键唤醒。

## 编译

需要 ESP-IDF 5.5.x。

```bash
cp main/voice_cloud_config.h.example main/voice_cloud_config.h   # 填入你自己的 API Key
cp main/voice_bridge_config.h.example main/voice_bridge_config.h
cp main/wifi_credentials.h.example main/wifi_credentials.h       # 可选：默认 Wi-Fi
idf.py set-target esp32s3
idf.py build flash monitor
```

以上三个头文件已被 Git 忽略。**API Key 会被编进固件，不要分享含真实密钥的固件。** 语音链路详情见 [voice_cloud/README.md](voice_cloud/README.md)。

## 致谢

DJI 遥控协议参考 DJI 官方开源的 [Osmo-GPS-Controller-Demo](https://github.com/dji-sdk/Osmo-GPS-Controller-Demo)。中文字体 Noto Sans SC 采用 SIL OFL 协议（`fonts/OFL.txt`）。
