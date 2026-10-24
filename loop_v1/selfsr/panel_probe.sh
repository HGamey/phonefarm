#!/bin/sh
# panel_probe.sh — 原神在前台时, 哪种可还原的办法能让屏幕跑到高刷 (插帧要屏幕刷新率高于游戏帧率)。
#   tools/devlock run panel-probe -- sh loop_v1/selfsr/panel_probe.sh <serial> <输出目录> [包名]
# 先冷启动进大世界 (phonefarm onboard --enter-only), 再逐个试:
#   A. system min_refresh_rate / peak_refresh_rate = 144 (AOSP 标准设置)
#   B. system refresh_rate_mode = 1..4 (红魔自带, 取值含义未知, 逐个试)
# 每种施加后等 6 s, 记 SurfaceFlinger 当前刷新周期与 dumpsys display 的活动模式; 试完写回原值并回读核对。
set -u
S="${1:?serial}"; OUT="${2:?outdir}"; PKG="${3:-com.miHoYo.Yuanshen}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PF="${PHONEFARM_BIN:-$ROOT/phonefarm}"
mkdir -p "$OUT"
a() { adb -s "$S" shell "$@" 2>&1 | tr -d '\r'; }
rate() {  # 当前刷新周期 (SF) 与活动模式 (display)
  printf '%s | %s' "$(a dumpsys SurfaceFlinger | grep -m1 -E 'mPeriod|refresh-rate' | tr -s ' ')" \
                   "$(a dumpsys display | grep -m1 -oE 'mActiveSfDisplayMode=DisplayMode\{id=[0-9]+, [^,]*, [^,]*, [^,]*, peakRefreshRate=[0-9.]+')"
}
o_min=$(a settings get system min_refresh_rate); o_peak=$(a settings get system peak_refresh_rate)
o_mode=$(a settings get system refresh_rate_mode); o_to=$(a settings get system screen_off_timeout)
echo "orig min=$o_min peak=$o_peak refresh_rate_mode=$o_mode" | tee "$OUT/probe.txt"
put() { if [ "$2" = null ]; then a settings delete system "$1" >/dev/null; else a settings put system "$1" "$2"; fi; }
restore() {
  put min_refresh_rate "$o_min"; put peak_refresh_rate "$o_peak"; put refresh_rate_mode "$o_mode"
  a settings put system screen_off_timeout "$o_to"
  echo "restored min=$(a settings get system min_refresh_rate) peak=$(a settings get system peak_refresh_rate) refresh_rate_mode=$(a settings get system refresh_rate_mode)" | tee -a "$OUT/probe.txt"
}
trap restore EXIT INT TERM
a settings put system screen_off_timeout 1800000
a am force-stop "$PKG"
"$PF" onboard --package "$PKG" --serial "$S" --enter-only --no-guest --no-devlock --out "$OUT/onboard" >/dev/null 2>&1 || echo "onboard 失败" | tee -a "$OUT/probe.txt"
sleep 15
echo "base: $(rate)" | tee -a "$OUT/probe.txt"
a settings put system peak_refresh_rate 144; a settings put system min_refresh_rate 144; sleep 6
echo "A min/peak=144: $(rate)" | tee -a "$OUT/probe.txt"
put min_refresh_rate "$o_min"; put peak_refresh_rate "$o_peak"; sleep 4
for m in 1 2 3 4; do
  a settings put system refresh_rate_mode $m; sleep 6
  echo "B refresh_rate_mode=$m: $(rate)" | tee -a "$OUT/probe.txt"
done
put refresh_rate_mode "$o_mode"; sleep 4
echo "after: $(rate)" | tee -a "$OUT/probe.txt"
a am force-stop "$PKG"
