#!/bin/sh
# megacity_noroot.sh <serial> <层.so> [轮数] — 不 root 的手机上给可调试版 Megacity Metro 挂层, 原生 / 插帧交替。
# 可调试版: apktool 解包加 android:debuggable="true" 重打包 (Unity 开源示例)。麦克风权限弹窗一律点「禁止」。
# 结束 (含 Ctrl-C) 删掉三项 GPU 调试层设置、清 debug.knobs.*、删 app 私有目录里的层。
set -u
S=$1; SO=$2; N=${3:-3}; P=com.unity.megacity.metro; L=VK_LAYER_refknobs_readonly
OUT=${OUT:-megacity_noroot_$S}; mkdir -p "$OUT"
a() { adb -s "$S" shell "$@"; }
PROPS="debug.knobs.interp debug.knobs.interp_mode debug.knobs.interp_fps debug.knobs.interp_backend debug.knobs.interp_stamp debug.knobs.interp_lead_ns"
restore() {
  for k in enable_gpu_debug_layers gpu_debug_app gpu_debug_layers; do a settings delete global $k >/dev/null; done
  for p in $PROPS; do a setprop $p "''"; done
  a am force-stop $P; a run-as $P rm -f libVkLayer_refknobs.so
  echo "restored: $(a settings get global enable_gpu_debug_layers) $(a settings get global gpu_debug_app) $(a settings get global gpu_debug_layers)"
}
trap 'restore; exit 130' INT TERM
adb -s "$S" push "$SO" /data/local/tmp/libVkLayer_refknobs.so >/dev/null
a run-as $P cp /data/local/tmp/libVkLayer_refknobs.so . || { echo "run-as 失败"; restore; exit 1; }
a settings put global gpu_debug_app $P; a settings put global gpu_debug_layers $L
WH=$(a wm size | sed -n 's/.*size: //p' | tail -1); W=${WH%x*}; H=${WH#*x}; [ "$W" -lt "$H" ] && { t=$W; W=$H; H=$t; }
lat() { a "dumpsys SurfaceFlinger --latency '$1'" | awk 'NR>1&&$2>0&&$2<9e18{n++;if(!f)f=$2;l=$2}END{if(n>1)printf "%.1f",(n-1)/((l-f)/1e9)}'; }
deny_mic() { a 'uiautomator dump /sdcard/ui.xml >/dev/null; cat /sdcard/ui.xml' | tr '>' '\n' | grep -E 'text="禁止[^"]*"' \
  | sed -E 's/.*bounds="\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]".*/\1 \2 \3 \4/' | head -1 | { read x1 y1 x2 y2 && a input tap $(((x1+x2)/2)) $(((y1+y2)/2)); }; }
tap() { a input tap $(python3 -c "print(int($1*$W), int($2*$H))"); }
cool() { i=0; while [ $i -lt 90 ]; do t=$(a dumpsys thermalservice | sed -n -E 's/.*mValue=([0-9.]+), mType=3, mName=skin.*/\1/p' | head -1)
  awk "BEGIN{exit !(${t:-99} < ${COOL_MAX:-36})}" && { echo "$1 skin=$t" >> "$OUT/temps.txt"; return; }; sleep 10; i=$((i+1)); done; }
arm() {
  cool "$r $1"; for p in $PROPS; do a setprop $p "''"; done
  case $1 in
    native) a settings put global enable_gpu_debug_layers 0;;
    ip) a settings put global enable_gpu_debug_layers 1; a setprop debug.knobs.interp 1; a setprop debug.knobs.interp_mode bm
        a setprop debug.knobs.interp_fps ${IP_FPS:-60}; a setprop debug.knobs.interp_backend sc; a setprop debug.knobs.interp_stamp 0
        a setprop debug.knobs.interp_lead_ns ${LEAD_NS:-50000000};;
  esac
  a am force-stop $P; a rm -f /storage/emulated/0/Android/data/$P/files/knobs_layer_out.json
  a monkey -p $P -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1; sleep 10; deny_mic; sleep 2
  tap 0.207 0.484; sleep 4; tap 0.5 0.558; sleep 3; tap 0.207 0.484; sleep 12; tap 0.505 0.884; sleep 10
  AL=$(a dumpsys SurfaceFlinger --list | sed -n -E 's/^RequestedLayerState\{([0-9a-f]+ SurfaceView\[com\.unity\.megacity\.metro[^ ]*\]\(BLAST\)#[0-9]+) .*/\1/p' | tail -1)
  SL=$(a dumpsys SurfaceFlinger --list | sed -n -E 's/^RequestedLayerState\{(selfsr_interp#[0-9]+) .*/\1/p' | tail -1)
  a dumpsys SurfaceFlinger --latency-clear >/dev/null; sleep 10
  afps=$(lat "$AL"); sfps=$([ -n "$SL" ] && lat "$SL")
  a screencap -p /data/local/tmp/mc_shot.png; adb -s "$S" pull /data/local/tmp/mc_shot.png "$OUT/$1_$r.png" >/dev/null
  a am force-stop $P; sleep 2
  a cat /storage/emulated/0/Android/data/$P/files/knobs_layer_out.json > "$OUT/$1_$r.marker.json" 2>/dev/null
  echo "$r $1 game_fps=${afps:--} sc_fps=${sfps:--}" | tee -a "$OUT/summary.txt"
}
r=1; while [ $r -le $N ]; do for x in native ip; do arm $x; done; r=$((r+1)); done
restore
