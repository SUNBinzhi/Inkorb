#!/usr/bin/env python3
"""Prepare an ignored local build header. Never print credential values."""
import json
import os
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent

def main():
    credentials = json.loads((ROOT / 'credentials.json').read_text())
    key = credentials['siliconflow_api_key'].strip()
    request = urllib.request.Request('https://api.siliconflow.cn/v1/models',
                                     headers={'Authorization': 'Bearer ' + key})
    with urllib.request.urlopen(request, timeout=20) as response:
        json.load(response)
    deepseek = os.getenv('DEEPSEEK_API_KEY', '')
    source = Path.home() / '.dsh/.env'
    if not deepseek and source.exists():
        for line in source.read_text().splitlines():
            match = re.match(r'^\s*(?:export\s+)?DEEPSEEK_API_KEY\s*=\s*(.*)$', line)
            if match:
                deepseek = match[1].strip().strip('\"\'')
    if not deepseek:
        raise RuntimeError('未找到本机 DeepSeek 密钥')
    text = '#pragma once\n#define INKORB_DEEPSEEK_KEY ' + json.dumps(deepseek)
    text += '\n#define INKORB_SILICONFLOW_KEY ' + json.dumps(key) + '\n'
    if credentials.get('volcengine_api_key'):
        text += '#define INKORB_VOLCENGINE_KEY ' + json.dumps(credentials['volcengine_api_key'].strip()) + '\n'
    target = ROOT.parent / 'main/voice_cloud_config.h'
    if not target.exists() or target.read_text() != text:
        target.touch(mode=0o600, exist_ok=True)
        target.write_text(text)
    target.chmod(0o600)
    print('云端鉴权通过，私有编译配置已就绪。请重新构建烧录，不要公开固件。')

if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('配置未完成：', type(exc).__name__, 'HTTP', getattr(exc, 'code', ''))
        raise SystemExit(1)
