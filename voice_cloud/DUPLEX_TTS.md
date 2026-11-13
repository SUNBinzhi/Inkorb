# Inkorb 双向流式自然语音

## 架构

设备录音 → SiliconFlow TeleSpeechASR → DeepSeek Flash Chat Completions SSE → Volcengine WebSocket双向流式TTS → 设备音频缓冲/扬声器。仍不需要Mac中转服务器。

- `CONFIG_INKORB_DUPLEX_TTS=y`；本地ESP-TTS选项关闭，旧版与本地版源码/恢复固件保留。
- 新模块 `main/inkorb_duplex_tts.c/.h`；官方组件 `espressif/esp_websocket_client==1.8.0`，Apache-2.0，来源/头文件/核心入口与许可经GitHub核对。
- WSS `openspeech.bytedance.com/api/v3/tts/bidirection`，新控制台X-Api-Key鉴权、资源seed-tts-2.0。
- Vivi自然中文女声 `zh_female_vv_uranus_bigtts`，22050Hz mono16 PCM，直接匹配现有I2S；软件1.5倍增益、峰值压缩，与硬件音量100共同使用。
- DeepSeek保持非思考模式，SSE增量文字同时显示并送入一个TTS会话；联网搜索仍执行原工具调用循环，之后继续在同一会话传输回答。不是按句重复提交独立TTS请求。
- 独立播放器、128KiB PSRAM音频缓冲、约150ms预缓冲、24秒朗读上限、90秒轮次预算保持。
- 显示4行双PSRAM缓冲、96KiB内部保留、1KiB普通分配优先内部阈值保持；WS任务8KiB内部栈，主语音任务和播放器栈在PSRAM。

## 凭据和维护

新密钥仅在 `voice_cloud/credentials.json` 和 `main/voice_cloud_config.h`，均权限600且Git忽略；不能复制到文档或日志。prepare_config.py已支持保留新密钥宏。实际固件含云端凭据，不公开固件或备份。

音频从云端返回，文字会发送给火山引擎合成。DeepSeek/识别的既有隐私边界保持。计费以用户开通的语音服务控制台为准，未自建或购买服务器。

SDK transport_ws在写失败时可能打印整条请求头，因此应用将这个tag日志关闭；自有模块只输出数字错误代码，不打印请求头或用户正文。

## 已做验证和现场修复

- 真实主机WebSocket鉴权/会话成功；两个增量文本包测试首PCM0.384秒，整段5.81秒音频在约1.63秒返回。此为主机测量，不能替代设备测量。
- 对实际C二进制帧解析器运行ASan/UBSan，覆盖合法PCM、每个截断长度和20000个随机帧，通过。控制消息/音频类型、帧大小/奇数字节、WS分片和输入边界有校验。
- 初次实机语音失败的具体原因：默认CONFIG_WS_BUFFER_SIZE=1024装不下服务器握手响应；现场TLS证书已验证，失败尚未到应用会话。独立主机测响应头约1224字节。现将握手缓冲配置8192，未改SDK源码/跳过证书验证；client buffer_size=4096只管数据接收，不能替代此配置。
- SSE读取块改256字节，避免为了填满大块而延迟前几个token。
- 按用户早先选择未连接/运行Socket扫描；源码核对及解析器测试不等于全面依赖或代码安全审计。

官方依据：
- https://docs.volcengine.com/docs/doubao-voice/bidirectional-streaming-text-to-speech-websocket?lang=zh
- https://github.com/espressif/esp-protocols/tree/master/components/esp_websocket_client

本聊work/duplex-tts-baseline/保留此前本地朗读源码/私有恢复固件（勿公开）；work/volc-probe.json、duplex-frame-check.c、duplex-tts-{build,flash,runtime}.log保留测量与构建证据。最终起播/听感/联网搜索和内存稳定性以本聊天后续设备日志与用户验收为准。


## 本次用户验收与实机结果（2026-10-28）

修复8KiB握手缓冲后，用户确认“有自然女声，速度和播放正常”。115秒实机记录7轮audio=played，全部underruns=0，NO_MEM/崩溃为0，包含一次实际HTTP search=200后继续SSE生成与语音播放。

首段文字到出声399–581ms；松手到出声2174–3914ms（其中联网查询那轮3914ms）。语音会话建立即可用，Vivi自然女声与原本地逐字播报听感由用户确认改善。该结果是现场多轮样本，不是任意网络下的保证。

当前以双向流式自然语音版为用户确认版。本地朗读和旧单向云TTS仅保留作回退，不自动混用。模型/密钥不写入本说明；最新固件哈希与统计在本聊work/duplex-tts-manifest.json。
