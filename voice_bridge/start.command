#!/bin/bash
cd "$(dirname "$0")"
if .venv/bin/python - <<'PY'
import json,urllib.request
from pathlib import Path
try:
    cfg=json.loads(Path('config.json').read_text())
    request=urllib.request.Request('http://127.0.0.1:'+str(cfg['port'])+'/health',
        headers={'Authorization':'Bearer '+cfg['device_token']})
    with urllib.request.urlopen(request,timeout=2) as response:
        ready=json.load(response)['ready']
    print('Inkorb 服务已经运行。' + ('语音模型已就绪。' if ready else '语音模型正在准备。'))
except Exception:
    raise SystemExit(1)
PY
then
    exit 0
fi
exec .venv/bin/python -u server.py
