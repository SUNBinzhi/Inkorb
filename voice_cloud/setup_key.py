#!/usr/bin/env python3
"""Save the SiliconFlow key locally; never print it or pass it as an argv value."""
import json
import os
import subprocess
import tempfile
import urllib.request
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parent

def main():
    script = '''try
set resultDialog to display dialog "请粘贴 cloud.siliconflow.cn 控制台新建的完整 API Key。保存前会验证，不会显示到聊天中。" default answer "" with hidden answer buttons {"取消", "保存"} default button "保存" with title "Inkorb 云端语音配置"
return text returned of resultDialog
on error number -128
return ""
end try'''
    result = subprocess.run(["/usr/bin/osascript", "-e", script], capture_output=True, text=True)
    if result.returncode:
        print("无法打开录入窗口，请返回 Codex 告知。")
        return 1
    key = result.stdout.strip()
    if not key:
        print("已取消，没有更改配置。")
        return 0
    if len(key) < 16 or len(key) > 512 or any(c.isspace() for c in key):
        print("密钥格式不符合要求，请复制完整 API Key 后重试。")
        return 1
    request = urllib.request.Request("https://api.siliconflow.cn/v1/models",
        headers={"Authorization": "Bearer " + key})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            json.load(response)
    except urllib.error.HTTPError as exc:
        note = "平台拒绝此密钥（HTTP " + str(exc.code) + "）。请从 cloud.siliconflow.cn 的 API 密钥页面新建并复制完整 Key，再重新运行配置工具。"
        subprocess.run(["/usr/bin/osascript", "-e", 'display dialog ' + json.dumps(note, ensure_ascii=False) + ' buttons {"知道了"} default button "知道了"'], capture_output=True)
        print("密钥验证失败，没有覆盖本地配置。HTTP", exc.code)
        return 1
    except Exception:
        print("暂时无法连接验证接口，没有覆盖配置，请稍后重试。")
        return 1
    payload = json.dumps({"siliconflow_api_key": key}, ensure_ascii=False)
    fd, name = tempfile.mkstemp(prefix=".credentials-", dir=ROOT)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(payload + "\n")
        os.chmod(name, 0o600)
        os.replace(name, ROOT / "credentials.json")
    finally:
        if os.path.exists(name):
            os.unlink(name)
    print("硅基流动密钥已保存。请回到 Codex 回复：密钥已保存。")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
