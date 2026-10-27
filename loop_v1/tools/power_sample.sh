#!/system/bin/sh
# 整机功耗采样 (高通机, 要 root): 停充 → 等 PRE 秒电流稳定 → 每秒读一次电池电流/电压/温度, 共 N 次 → 恢复充电。
#   sh power_sample.sh <N> [PRE, 缺省 5]
# 每行输出 "<电流 uA> <电压 uV> <温度 0.1°C>"; 放电电流为负。停充后 USB 不再给手机供电 (usb current_now = 0),
# 整机耗电全部来自电池, 所以 -电流 × 电压 就是整机功耗。中途被杀也会在 N+PRE+30 秒后自己恢复充电。
N=${1:?N}; PRE=${2:-5}
B=/sys/class/qcom-battery/charging_enabled; P=/sys/class/power_supply/battery
[ -w $B ] || { echo "no $B"; exit 3; }
(sleep $((N + PRE + 30)); echo 1 > $B) >/dev/null 2>&1 &
echo 0 > $B
sleep "$PRE"
i=0
while [ $i -lt "$N" ]; do
  echo "$(cat $P/current_now) $(cat $P/voltage_now) $(cat $P/temp)"
  sleep 1
  i=$((i + 1))
done
echo 1 > $B
