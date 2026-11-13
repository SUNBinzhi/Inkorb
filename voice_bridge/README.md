> 当前已切换设备直连云端，Mac中转服务已停止。本文件保留旧版运行方法；当前实现见 `../voice_cloud/README.md`。

# Inkorb 语音对话体验版

## 使用

1. Mac 和设备连接同一局域网，保持 Mac 唤醒。
2. 双击此目录的 `start.command` 启动中转服务。首次模型下载较大，日志出现 `ASR ready` 后可用。
3. 设备正常进入时钟后，轻点蓝色 Wi-Fi/电池胶囊进入对话页。
4. 按住屏幕上的“按住说话”或大的 PWR 实体键说话，松开后发送；单次最长 12 秒。
5. 设备显示识别文字和 AI 回答，并通过扬声器播放。回答结束后可继续下一轮。
6. 点左上角返回时钟。小键改为短按熄屏、按住约两秒关机；关机后仍需长按大键约两秒开机。

此版本为半双工：AI 回答时不接受下一次录音，不支持插话打断。
只在按住时保存本轮麦克风音频；原音量光晕仍在时钟页工作。

## 运行环境

- Mac Apple Silicon，Python 3.12、`ffmpeg`、edge-tts 小晓神经语音；系统中文声音 Tingting 作为失败回退。
- `.venv` 安装 `requirements.lock` 中的依赖；语音识别为 faster-whisper small CPU/int8。
- DeepSeek 使用 `deepseek-flash` 非思考模式，只向它发送文字及最多四轮上下文。
- 密钥从 Mac 的 `DEEPSEEK_API_KEY` 环境变量读取，或运行时读取 `~/.dsh/.env` 中同名配置。
- **密钥没有复制进工程配置或设备固件。** 不要将原始 `.env` 或 `.credentials.yaml` 加入 Git、日志或知识库。
- `config.json` 和 `main/voice_bridge_config.h` 是本机生成的设备配对配置，已加入忽略规则；不要公开设备令牌。
- 当前 Mac 地址为 `<your-mac-lan-ip>:8765`，服务仅用于受信任局域网。Mac 地址变化后需同步更新上述两个配置并重新编译烧录。

模型和临时语音处理在 Mac 上执行；合成语音的临时文件会清理。
对话上下文与最近任务只保存在进程内存，不写聊天记录文件；服务重启后上下文清空。

## HTTP 协议

各路由要求 `Authorization: Bearer <设备令牌>`。

- `GET /health`：模型是否准备完成。
- `POST /turn`：22050Hz、单声道、16-bit little-endian PCM，最长 12 秒，返回任务编号。
- `GET /jobs/<编号>`：识别中、思考中、联网搜索、语音准备、完成或错误；包含文字结果。
- `GET /audio/<编号>`：22050Hz、单声道 PCM，设备边下载边播放。
- `POST /text`：本机诊断入口，以 JSON `text` 代替录音，同样要求令牌。

最多一轮同时处理；录音长度、任务数及响应内容有上限。旧任务在后续请求时清理。
回答默认简短，语音合成播放最长 24 秒，较长回答仍可在屏幕滚动查看。

## 实现与验收边界

- 优先复用开源 faster-whisper（MIT），Python 直接集成，识别无需额外付费 API。
- 备选 whisper.cpp（MIT）：Apple Silicon 加速好，但需另外构建和管理原生接口。
- 备选 MLX Whisper（MIT）：适合 Mac GPU，平台范围更窄，首版先控制接入复杂度。
- ASR、显示和传输复用现成组件，自建部分为本地桥接、按键状态与音频调度，属于中等接入工作量；未另购语音 API。
- 已核对上游许可证、README、识别入口和依赖声明；执行 pip-audit 检查本版锁定的运行依赖。
- 用户已选择不安装 Socket，因此**未执行 Socket 扫描或声称通过 Socket 安全检查**。pip-audit 只反映已知依赖漏洞，不等同全面代码审计。
- Noto Sans SC 中文字库采用 SIL OFL 1.1，许可证在 `fonts/OFL.txt`。需启用 LVGL `FONT_FMT_TXT_LARGE`。

重新安装运行依赖：

```bash
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python -r requirements.lock
./start.command
```

## 保留旧版

本次语音改动前的固件及非敏感主程序源码已保留在本次 Codex 聊天的
`work/voice-baseline/`，其中固件是本地恢复用途，勿公开分发。
回退只恢复原应用固件及对应源码，不需要擦除整个 Flash 或删除 Wi-Fi 配置。


## 2026-10-28：按需联网与自然语音

- 架构：ESP32 录音/显示/播放 → Mac Whisper small 识别 → DeepSeek deepseek-flash（非思考）→ Microsoft Edge 小晓神经语音 → PCM 回传扬声器。依赖 Mac 中转，尚不能脱离 Mac 独立运行。
- 模型的训练截止日未从可靠元数据验证；不使用模型自报的“2024年”作为事实。系统提示注入当前北京时间和实际配置模型名。
- 在 Chat Completions 中提供 web_search 工具；模型按问题选择，实时问题要求先查。直接调用 DeepSeek 官方 Search API，使用现有 API Key。一次最多两次搜索，每次三条结果；不额外安装搜索 SDK。
- 屏幕显示“正在联网查资料”及最多两个来源标题/域名；来源完整 URL 保存在本轮 job 的 sources 字段，不朗读链接。网页摘要按不可信数据处理，只供回答参考。搜索失败/没有结果会明确提示无法核实。
- 搜索会向 DeepSeek 发送查询词，官方标价 $0.75/1000 次调用，另外计算模型 token 费用。参考 https://api-docs.deepseek.com/guides/search/ 。模型别名和服务规则可能变化，以官方为准。
- edge-tts 固定 7.3.1，音色 zh-CN-XiaoxiaoNeural，语速 +5%。回答文字会发送到 Microsoft 在线语音服务，录音仍在 Mac 本地识别。无需新 API Key；此开源客户端使用 Edge 在线接口，不代表获得 Azure 商用服务 SLA，也不保证接口长期可用。
- 在线语音限时 8 秒，失败自动使用 Mac Tingting，并在屏幕注明回退。硬件音量保持80，PCM 22050Hz/mono16，语音仍最长24秒。
- 复用开源客户端（Free）：接入工作量低于自行实现语音协议；不自行训练 TTS（Build，工作量高）。Azure 等正式托管 TTS（Buy）需另行账号/计费，本体验版未开通。
- 已读 edge-tts 上游 README、通信源码和 LICENSE：主代码 LGPLv3，仅 srt_composer.py 为 MIT；不把整个包标成 MIT。上游 https://github.com/rany2/edge-tts 。
- 已执行本版完整锁定运行依赖的 pip-audit，无已知漏洞；按用户之前选择，仍未安装/运行 Socket。此结论不等同全面代码审计。
- 固件保留8行PSRAM双缓冲、LCD交易队列深度1、PSRAM语音任务栈，避免抢占内部SPI DMA内存；小键负责熄屏/两秒关机，大键负责语音/断电后两秒开机；保留时钟与电量功能。


## 实体键调换（2026-10-28，当前映射）

- 大键PWR（GPIO3，高电平按下）：进入语音页，按住录音、松开发送；熄屏时按下会先亮屏。开机时持续按住的键会忽略到松开，避免误触录音。
- 小键BOOT（GPIO0，低电平按下）：短按松手熄屏/亮屏，按住约两秒发送AXP2101实际关机命令。
- **真正关机后仍由大键长按约两秒开机**：小键未连PMIC，软件无法改变断电后的硬件唤醒接线。上电/复位时不要持续按小BOOT键，否则可能进入芯片下载模式。
- 大键硬件四秒长按关机已关闭（REG22[1:0]=00），避免长句录音意外断电；温度保护等其他位保持。REG27维持2秒开机配置。固件失去响应时小键不能通过软件关机，不再保留大键四秒强制关机兜底。
- PMIC定义核对：XPowersAXP2101.hpp 的 disableLongPressShutdown 清REG22 bit1；https://github.com/lewisxhe/XPowersLib/blob/master/src/XPowersAXP2101.hpp 。
