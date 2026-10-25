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
#   7. native  vs srip:1:$SR_FULL:$IP_MODE:...       超分 + 插帧同时开 (第 5 步)
#   8. layer+gfrc:101 vs ip:...+gfrc:101             插帧走子图层 (G-FRC+ 101 放开限帧到 60, 子图层插到 ~90), 第 4 步, 只在有 G-FRC+ 的机器上跑
#   9. gfrc:111 vs srip:1:$SR_FULL:...+gfrc:101       自研超分 + 插帧 vs G-FRC+ 超分 + 插帧, 第 5 步, 同上
# 环境变量: ENTER_SCRIPT (onboard 认不出时的进门脚本) KEEP_LAYER_PROP (1 = 全程保留层属性)
#           SR_SHARP (满分辨率超分锐化, 缺省 1.0; Megacity 0) SR_FULL (本机屏幕:SR_SHARP) SR_SCALE (0.56) SR_OUT (2141x969:0.8) IP_MODE (bm) IP_FPS (120) GFRC_BUF (G-FRC+ 放大后的缓冲, 缺省 3214x1456)
#           IP_TUNE (stamp=0,panel=120,backend=sc,lead_ns=25000000) COOL_MAX (42) SETS ("1 2 3 4 5 6 7 8 9")
# 前提: 手机已 root、游戏已装好并登录过 (onboard 能进大世界)。层用 knobs/gray/build/build_layer.sh 现编。
# 每组结束 (含 Ctrl-C) 都把 Game Mode、G-FRC+ 位、debug.knobs.*、刷新率、风扇、屏幕超时写回并回读核对,
# 层由 gray-layer session 摘除。超分算子用 Catmull-Rom + RCAS (层缺省)。
set -eu
SERIAL="${1:?用法: sh loop_v1/selfsr/repro.sh <serial> [配对数] [输出目录] [包名]}"
ROUNDS="${2:-6}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${3:-$ROOT/_runs/selfsr_$(date +%Y%m%d-%H%M%S)}"
PKG="${4:-com.miHoYo.Yuanshen}"
# 满分辨率超分目标 = 本机屏幕横屏尺寸 (换机不用改)
PANEL=$(adb -s "$SERIAL" shell wm size | tr -d '\r' | sed -n 's/.*: *\([0-9]*\)x\([0-9]*\)$/\1 \2/p' | tail -1 | awk '{print ($1>$2)?$1"x"$2:$2"x"$1}')
# 已知游戏的进门方式: 原神走 onboard; Megacity Metro 的 onboard 认不出已进游戏, 用固定脚本, 且要全程保留层属性
case "$PKG" in
  com.unity.megacity.metro)
    ENTER_SCRIPT="${ENTER_SCRIPT:-s:10;t:0.207,0.484;s:4;t:0.5,0.558;s:3;t:0.207,0.484;s:12;t:0.505,0.884;s:5}"
    KEEP_LAYER_PROP="${KEEP_LAYER_PROP:-1}"
    SR_SHARP="${SR_SHARP:-0}" ;;   # 有胶片颗粒: 锐化会放大成噪点 (STEP6)
esac
SR_FULL="${SR_FULL:-$PANEL:${SR_SHARP:-1.0}}"
ENTER_SCRIPT="${ENTER_SCRIPT:-}"; KEEP_LAYER_PROP="${KEEP_LAYER_PROP:-}"
SR_SCALE="${SR_SCALE:-0.56}"; SR_OUT="${SR_OUT:-2141x969:0.8}"
IP_MODE="${IP_MODE:-bm}"; IP_FPS="${IP_FPS:-120}"; IP_TUNE="${IP_TUNE:-stamp=0,panel=120,backend=sc,lead_ns=25000000}"   # sc: 插帧走子图层, 绕开应用缓冲队列的限帧 (STEP4)
GFRC_BUF="${GFRC_BUF:-3214x1456}"
COOL_MAX="${COOL_MAX:-42}"
SETS="${SETS:-1 2 3 4 5 6 7 8 9}"
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
    --out "$OUT/$1" --a "$2" --b "$3" --rounds "$ROUNDS" --so "$SO" --phonefarm "$BIN" --fan --cool-max "$COOL_MAX" \
    ${ENTER_SCRIPT:+--enter-script "$ENTER_SCRIPT"} ${KEEP_LAYER_PROP:+--keep-layer-prop}
}
for g in $SETS; do
  case $g in
    1) run 1_layer native layer ;;
    2) run 2_sr_full native "sr:1:$SR_FULL" ;;
    3) run 3_sr_downscale native "sr:$SR_SCALE:$SR_OUT" ;;
    4) run 4_interp layer "ip:$IP_MODE:$IP_FPS:$IP_TUNE" ;;
    5) if [ "$HAS_GFRC" = true ]; then run 5_gfrc native gfrc:111; else echo "这台机器没有 G-FRC+, 跳过第 5 组"; fi ;;
    6) if [ "$HAS_GFRC" = true ]; then run 6_gfrc_vs_sr gfrc:101 "sr:1:$SR_FULL+gfrc:101"; else echo "这台机器没有 G-FRC+, 跳过第 6 组"; fi ;;
    7) run 7_sr_interp native "srip:1:$SR_FULL:$IP_MODE:$IP_FPS:$IP_TUNE" ;;
    8) if [ "$HAS_GFRC" = true ]; then run 8_interp_gfrc "layer+gfrc:101" "ip:$IP_MODE:$IP_FPS:$IP_TUNE,sc_dst=$GFRC_BUF+gfrc:101"; else echo "这台机器没有 G-FRC+, 跳过第 8 组"; fi ;;
    9) if [ "$HAS_GFRC" = true ]; then run 9_gfrc111_vs_srip gfrc:111 "srip:1:$SR_FULL:$IP_MODE:$IP_FPS:$IP_TUNE+gfrc:101"; else echo "这台机器没有 G-FRC+, 跳过第 9 组"; fi ;;
  esac
done
{
  echo "# 自研超分 + 插帧复现: $PKG ($SERIAL)"; echo
  for d in "$OUT"/*/; do [ -f "$d/report.md" ] && { sed 's/^# /## /' "$d/report.md"; echo; }; done
} > "$OUT/summary.md"
echo "报告: $OUT/summary.md"
