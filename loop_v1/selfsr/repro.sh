#!/bin/sh
# 一条命令复现自研超分 + 插帧 (灰档 Vulkan 层) 对游戏的画质/帧率影响, 不改游戏内任何设置:
#   sh loop_v1/selfsr/repro.sh <serial> [配对数, 缺省 6] [输出目录] [包名, 缺省 com.miHoYo.Yuanshen]
# 依次跑 (每组都是冷启动 ABBA, 精确符号翻转检验; 整轮开机身风扇, 每臂开跑前等电池 ≤ COOL_MAX °C):
#   1. native  vs layer                              透传层本身的开销
#   2. native  vs sr:1:$SR_FULL                      满分辨率超分 (游戏照原生渲染, 层直接超分到屏幕分辨率)
#   3. native  vs sr:$SR_SCALE:$SR_OUT               Game Mode 降分辨率 + 层内超分 (省 GPU)
#   4. layer   vs ip:$IP_MODE:$IP_FPS:...            层内插帧
#   5. native  vs gfrc:111                           厂商 G-FRC+ (超分+插帧) 对标, 只在有 G-FRC+ 的机器上跑
#   6. gfrc:101 vs sr:1:$SR_FULL+gfrc:101            G-FRC+ 只超分 vs 自研超分 (G-FRC+ 开着放开厂商限帧, 自研出图), 同上
# 环境变量: SR_FULL (2688x1216:1.0) SR_SCALE (0.56) SR_OUT (2141x969:0.8) IP_MODE (bm) IP_FPS (120)
#           IP_TUNE (stamp=0,panel=120) COOL_MAX (42) SETS ("1 2 3 4 5 6")
# 前提: 手机已 root、游戏已装好并登录过 (onboard 能进大世界)。层用 knobs/gray/build/build_layer.sh 现编。
# 每组结束 (含 Ctrl-C) 都把 Game Mode、G-FRC+ 位、debug.knobs.*、刷新率、风扇、屏幕超时写回并回读核对,
# 层由 gray-layer session 摘除。超分算子用 Catmull-Rom + RCAS (层缺省)。
set -eu
SERIAL="${1:?用法: sh loop_v1/selfsr/repro.sh <serial> [配对数] [输出目录] [包名]}"
ROUNDS="${2:-6}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${3:-$ROOT/_runs/selfsr_$(date +%Y%m%d-%H%M%S)}"
PKG="${4:-com.miHoYo.Yuanshen}"
SR_FULL="${SR_FULL:-2688x1216:1.0}"
SR_SCALE="${SR_SCALE:-0.56}"; SR_OUT="${SR_OUT:-2141x969:0.8}"
IP_MODE="${IP_MODE:-bm}"; IP_FPS="${IP_FPS:-120}"; IP_TUNE="${IP_TUNE:-stamp=0,panel=120}"
COOL_MAX="${COOL_MAX:-42}"
SETS="${SETS:-1 2 3 4 5 6}"
BIN="${PHONEFARM_BIN:-}"
if [ -z "$BIN" ]; then
  BIN="$ROOT/src/target/release/phonefarm"
  [ -x "$BIN" ] || (cd "$ROOT/src" && cargo build --release)
  codesign --force --sign - "$BIN" 2>/dev/null || true
fi
[ "$(adb -s "$SERIAL" shell su -c id 2>/dev/null | grep -c 'uid=0')" = 1 ] || { echo "这台手机没有 root, 层装不进游戏"; exit 2; }
bash "$ROOT/knobs/gray/build/build_layer.sh" >/dev/null
mkdir -p "$OUT"
SO="$OUT/libVkLayer_refknobs.so"
cp "$ROOT/knobs/gray/build/out/libVkLayer_refknobs.so" "$SO"
HAS_GFRC=$(adb -s "$SERIAL" shell getprop ro.vendor.feature.zte_feature_gfrc | tr -d '\r')
run() {  # run <组名> <A> <B>
  PHONEFARM_BIN="$BIN" python3 -I "$ROOT/loop_v1/selfsr/selfsr_ab.py" run --serial "$SERIAL" --pkg "$PKG" \
    --out "$OUT/$1" --a "$2" --b "$3" --rounds "$ROUNDS" --so "$SO" --phonefarm "$BIN" --fan --cool-max "$COOL_MAX"
}
for g in $SETS; do
  case $g in
    1) run 1_layer native layer ;;
    2) run 2_sr_full native "sr:1:$SR_FULL" ;;
    3) run 3_sr_downscale native "sr:$SR_SCALE:$SR_OUT" ;;
    4) run 4_interp layer "ip:$IP_MODE:$IP_FPS:$IP_TUNE" ;;
    5) if [ "$HAS_GFRC" = true ]; then run 5_gfrc native gfrc:111; else echo "这台机器没有 G-FRC+, 跳过第 5 组"; fi ;;
    6) if [ "$HAS_GFRC" = true ]; then run 6_gfrc_vs_sr gfrc:101 "sr:1:$SR_FULL+gfrc:101"; else echo "这台机器没有 G-FRC+, 跳过第 6 组"; fi ;;
  esac
done
{
  echo "# 自研超分 + 插帧复现: $PKG ($SERIAL)"; echo
  for d in "$OUT"/*/; do [ -f "$d/report.md" ] && { sed 's/^# /## /' "$d/report.md"; echo; }; done
} > "$OUT/summary.md"
echo "报告: $OUT/summary.md"
