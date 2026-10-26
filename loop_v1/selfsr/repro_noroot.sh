#!/bin/sh
# repro_noroot.sh <serial> [轮数] [输出目录] — 不 root 的手机上一键复现插帧, 出报告和对比截图。
#   1) refbench frag_alu (可调试, 钉 60 帧): 原生 / 插帧交替  → 期望 60 → 120
#   2) 可调试版 Megacity Metro (若已装):     原生 / 插帧交替  → 期望 30 → 60
# 前提: refbench (REFBENCH_DEBUGGABLE=1 编) 已装; 层用 knobs/gray/build/build_layer.sh 现编。
# 每组结束 (含 Ctrl-C) 删回三项 GPU 调试设置、清 debug.knobs.*。输出: <目录>/report.md + *_cmp.png
set -eu
S=${1:?用法: sh loop_v1/selfsr/repro_noroot.sh <serial> [轮数] [输出目录]}
N=${2:-3}; OUTD=${3:-repro_noroot_${S}_$(date +%m%d%H%M)}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
SO=$ROOT/knobs/gray/build/out/libVkLayer_refknobs.so
[ -f "$SO" ] || { echo "先编层: bash knobs/gray/build/build_layer.sh"; exit 1; }
mkdir -p "$OUTD"; OUTD=$(cd "$OUTD" && pwd)
adb -s "$S" shell 'svc power stayon usb; input keyevent KEYCODE_WAKEUP'
trap 'adb -s "$S" shell svc power stayon false' EXIT
adb -s "$S" push "$SO" /data/local/tmp/libVkLayer_refknobs.so >/dev/null
ARMS="native ip" COOL_MAX=${COOL_MAX:-36} INTENSITY=4 FPS_CAP=60 OUT="$OUTD/refbench" \
  sh "$HERE/noroot_smoke.sh" "$S" /data/local/tmp/libVkLayer_refknobs.so "$N"
if adb -s "$S" shell pm path com.unity.megacity.metro >/dev/null 2>&1 \
   && adb -s "$S" shell run-as com.unity.megacity.metro true 2>/dev/null; then
  OUT="$OUTD/megacity" sh "$HERE/megacity_noroot.sh" "$S" "$SO" "$N"
else echo "没装可调试版 Megacity, 跳过第 2 组"; fi
python3 - "$OUTD" "$S" <<'PY'
import sys, os, re, json, statistics as st
from PIL import Image
d, serial = sys.argv[1], sys.argv[2]
rows = []
for g, fps_key in (("refbench", "app_fps"), ("megacity", "game_fps")):
    f = os.path.join(d, g, "summary.txt")
    if not os.path.exists(f): continue
    nat, ipa, ips = [], [], []
    for l in open(f):
        m = dict(re.findall(r"(\w+)=([\d.]+)", l)); arm = l.split()[1]
        if arm == "native" and fps_key in m: nat.append(float(m[fps_key]))
        if arm == "ip" and "sc_fps" in m: ips.append(float(m["sc_fps"])); ipa.append(float(m.get(fps_key, "nan")))
    delays = []
    for mf in sorted(x for x in os.listdir(os.path.join(d, g)) if x.startswith("ip_") and x.endswith(".marker.json")):
        try: delays.append(json.load(open(os.path.join(d, g, mf)))["interp"]["sc_real_sched_delay_ns"]["p50"] / 1e6)
        except Exception: pass
    if nat and ips:
        rows.append((g, len(ips), st.mean(nat), st.mean(ipa), st.mean(ips), min(ips), max(ips), st.mean(nat) and st.mean(ips) / st.mean(nat), st.mean(delays) if delays else float("nan")))
        a = Image.open(os.path.join(d, g, "native_1.png")).convert("RGB"); b = Image.open(os.path.join(d, g, "ip_1.png")).convert("RGB")
        w, h = a.size; c = Image.new("RGB", (w * 2 + 20, h), "white"); c.paste(a, (0, 0)); c.paste(b, (w + 20, 0)); c.thumbnail((2000, 2000))
        c.save(os.path.join(d, f"{g}_cmp.png"))
with open(os.path.join(d, "report.md"), "w") as o:
    o.write(f"# 不 root 插帧复现 · {serial}\n\n| 负载 | 对数 | 原生上屏 | 插帧时游戏自身 | 插帧上屏 (范围) | 倍数 | 真实帧加的延迟 p50 |\n|---|---|---|---|---|---|---|\n")
    for g, n, nat, ipa, ips, lo, hi, x, dl in rows:
        o.write(f"| {g} | {n} | {nat:.1f} | {ipa:.1f} | {ips:.1f} ({lo:.1f}–{hi:.1f}) | ×{x:.2f} | +{dl:.1f} ms |\n")
    o.write("\n对比截图 (左原生, 右插帧): " + ", ".join(f"`{g}_cmp.png`" for g, *_ in rows) + "\n")
print(open(os.path.join(d, "report.md")).read())
PY
