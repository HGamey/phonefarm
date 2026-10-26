#!/bin/sh
# noroot_smoke.sh <serial> [层.so] [轮数] — 不用 root 验证自研层: 可调试的 refbench + Android GPU 调试层设置。
# 臂: native / layer(透传) / sr(超分到屏幕分辨率) / ip(子图层插帧)。每臂冷启动 refbench frag_alu, 读 refbench_out.json
# 和 SurfaceFlinger 子图层 latency。结束 (含 Ctrl-C) 还原三项 settings、清 debug.knobs.*、删 app 私有目录里的层。
set -u
S=$1; SO=${2:-/data/local/tmp/libVkLayer_refknobs.so}; N=${3:-3}
P=io.github.hgamey.refbench; L=VK_LAYER_refknobs_readonly
OUT=${OUT:-noroot_${S}_$(date +%m%d%H%M)}; mkdir -p "$OUT"
a() { adb -s "$S" shell "$@"; }
PROPS="debug.knobs.selfsr debug.knobs.interp debug.knobs.interp_mode debug.knobs.interp_fps debug.knobs.interp_backend debug.knobs.interp_stamp debug.knobs.interp_lead_ns"
o1=$(a settings get global enable_gpu_debug_layers); o2=$(a settings get global gpu_debug_app); o3=$(a settings get global gpu_debug_layers)
restore() {
  for k in enable_gpu_debug_layers:$o1 gpu_debug_app:$o2 gpu_debug_layers:$o3; do
    n=${k%%:*}; v=${k#*:}; [ "$v" = null ] && a settings delete global $n >/dev/null || a settings put global $n "$v"; done
  for p in $PROPS; do a setprop $p "''"; done
  a am force-stop $P; a run-as $P rm -f libVkLayer_refknobs.so
  echo "restored: $(a settings get global enable_gpu_debug_layers) $(a settings get global gpu_debug_app) $(a settings get global gpu_debug_layers)"
}
trap 'restore; exit 130' INT TERM
case "$SO" in /data/*) ;; *) adb -s "$S" push "$SO" /data/local/tmp/libVkLayer_refknobs.so >/dev/null; SO=/data/local/tmp/libVkLayer_refknobs.so;; esac
a run-as $P cp "$SO" . || { echo "run-as 失败: refbench 没装或不可调试"; restore; exit 1; }
a settings put global gpu_debug_app $P; a settings put global gpu_debug_layers $L
WH=$(a wm size | sed -n 's/.*size: //p' | tail -1); W=${WH%x*}; H=${WH#*x}; [ "$W" -lt "$H" ] && WH=${H}x$W
echo "panel $WH"
lat() { a dumpsys SurfaceFlinger --latency "$1" | awk 'NR>1&&$2>0&&$2<9e18{n++;if(!f)f=$2;l=$2}END{if(n>1)printf "%.1f",(n-1)/((l-f)/1e9)}'; }
cool() {  # 等机身降到 COOL_MAX (°C) 以下, 最多 15 分钟
  i=0; while [ $i -lt 90 ]; do t=$(a dumpsys thermalservice | sed -n -E 's/.*mValue=([0-9.]+), mType=3, mName=skin.*/\1/p' | head -1)
    awk "BEGIN{exit !(${t:-99} < ${COOL_MAX:-36})}" && { echo "skin=$t" >> "$OUT/temps.txt"; return; }; sleep 10; i=$((i+1)); done
  echo "skin=$t (超时)" >> "$OUT/temps.txt"
}
arm() {  # $1=名字
  cool
  for p in $PROPS; do a setprop $p "''"; done
  case $1 in
    native) a settings put global enable_gpu_debug_layers 0;;
    layer)  a settings put global enable_gpu_debug_layers 1;;
    sr)     a settings put global enable_gpu_debug_layers 1; a setprop debug.knobs.selfsr "$WH:1.0";;
    ip)     a settings put global enable_gpu_debug_layers 1; a setprop debug.knobs.interp 1; a setprop debug.knobs.interp_mode bm
            a setprop debug.knobs.interp_fps 120; a setprop debug.knobs.interp_backend sc; a setprop debug.knobs.interp_stamp 0
            a setprop debug.knobs.interp_lead_ns 25000000;;
  esac
  a am force-stop $P; a run-as $P rm -f knobs_layer_out.json; a rm -f /storage/emulated/0/Android/data/$P/files/knobs_layer_out.json
  a am start -W -n $P/android.app.NativeActivity --es scene frag_alu --es run_id "$1" --es frames 1500 --es intensity ${INTENSITY:-16} --es fps_cap ${FPS_CAP:-0} >/dev/null
  sleep 5
  AL=$(a dumpsys SurfaceFlinger --list | sed -n -E 's/^RequestedLayerState\{(io\.github\.hgamey\.refbench\/android\.app\.NativeActivity#[0-9]+) .*/\1/p' | tail -1)
  SL=$(a dumpsys SurfaceFlinger --list | sed -n -E 's/^RequestedLayerState\{(selfsr_interp#[0-9]+) .*/\1/p' | tail -1)
  a dumpsys SurfaceFlinger --latency-clear >/dev/null; sleep 5
  afps=$(lat "$AL"); sfps=$([ -n "$SL" ] && lat "$SL")
  a screencap -p /data/local/tmp/ns_shot.png; adb -s "$S" pull /data/local/tmp/ns_shot.png "$OUT/$1_$r.png" >/dev/null
  i=0; while [ $i -lt 40 ] && [ "$(a pidof $P)" ]; do sleep 1; i=$((i+1)); done
  a cat /storage/emulated/0/Android/data/$P/files/knobs_layer_out.json > "$OUT/$1_$r.marker.json" 2>/dev/null
  a cat /storage/emulated/0/Android/data/$P/files/refbench_out.json > "$OUT/$1_$r.json"
  ld=$(python3 -c "import json;d=json.load(open('$OUT/$1_$r.marker.json'));print(d.get('layer_loaded'), (d.get('selfsr') or {}).get('frames', (d.get('selfsr') or {}).get('count','-')))" 2>/dev/null)
  echo "$r $1 app_fps=${afps:--} sc_fps=${sfps:--} layer=${ld:-none}" | tee -a "$OUT/summary.txt"
}
r=1; while [ $r -le $N ]; do for x in ${ARMS:-native layer sr ip}; do arm $x; done; r=$((r+1)); done
restore
