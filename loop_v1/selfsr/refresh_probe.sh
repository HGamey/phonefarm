#!/bin/sh
# 游戏运行中每秒记一次: 厂商刷新率控制器结果、system_server 下发的刷新率范围、当前刷新率, 查「窗口后屏幕被降到 60 Hz」(STEP4 待查)。
#   sh loop_v1/selfsr/refresh_probe.sh <serial> [秒数, 缺省 120] [点屏间隔秒, 缺省 0 = 不点] > probe.tsv
# 点屏点在屏幕左上角 (5%, 5%) 附近的空白处; 游戏要先进到大世界 (这个脚本不启动游戏)。
# 输出列: 秒 控制器fps 刷新率模式 当前Hz 范围 点了没有
set -eu
S="${1:?用法: sh refresh_probe.sh <serial> [秒数] [点屏间隔秒]}"
DUR="${2:-120}"
TAP="${3:-0}"
SIZE=$(adb -s "$S" shell wm size | tr -d '\r' | sed -n 's/.*: *\([0-9]*\)x\([0-9]*\)$/\1 \2/p' | tail -1)
X=$(echo "$SIZE" | awk '{print int(($1>$2?$1:$2)*0.05)}')
Y=$(echo "$SIZE" | awk '{print int(($1<$2?$1:$2)*0.05)}')
printf 't\tctrl_fps\tmode\thz\trange\ttap\n'
i=0
while [ "$i" -lt "$DUR" ]; do
  tapped=0
  if [ "$TAP" -gt 0 ] && [ $((i % TAP)) -eq 0 ] && [ "$i" -gt 0 ]; then adb -s "$S" shell input tap "$X" "$Y"; tapped=1; fi
  adb -s "$S" shell '
    c=$(settings get system ScreenRefreshRateController_resultFps)
    m=$(settings get system refresh_rate_mode)
    hz=$(dumpsys SurfaceFlinger | grep -m1 -o "activeMode={[^}]*vsyncRate=[0-9.]*" | grep -o "[0-9.]*$")
    r=$(dumpsys display | grep -m1 -o "primary=\[[^]]*\]" )
    printf "%s\t%s\t%s\t%s" "$c" "$m" "$hz" "$r"' | tr -d '\r' | awk -v t="$i" -v k="$tapped" '{print t "\t" $0 "\t" k}'
  i=$((i + 1))
  sleep 1
done
