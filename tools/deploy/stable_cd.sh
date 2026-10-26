#!/bin/bash
# stable 分支持续部署（节点直接从开发目录跑 phonefarm 的情形，例如 mbp1）。
#
# 发版：给 stable 提 PR（或把 main 的某个提交推到 stable）。节点每 5 分钟看一次 origin/stable，
# 有新提交且节点空闲（/queue 里 active+queued 为 0）时：切到该提交 → cargo build --release → 签名 →
# 换掉 phonefarm 二进制 → 重启 node_service（phonefarm serve 是它的子进程）→ /health 30 秒内回 ok 才算成功。
# 编译失败不动线上；重启后健康检查失败就回滚到上一个提交和上一个二进制再重启。
#
# 用法（从终端起，不要用 launchd：launchd 起的进程读不了 ~/Desktop 下的仓库）：
#   nohup tools/deploy/stable_cd.sh --loop >/dev/null 2>&1 &     # 常驻，每 INTERVAL 秒一次
#   tools/deploy/stable_cd.sh                                     # 只检查一次
# 环境变量（缺省值按 mbp1 的目录布局）：
#   PF_DIR      phonefarm 工作目录（node_service 用的那个）      缺省：本脚本所在仓库
#   HG_DIR      含 harness-ops/ 和 game_opt_loop/ 的目录         缺省：$PF_DIR/..
#   NODE_TOKEN_FILE  node_service 的 token                       缺省：$HG_DIR/../.hgamey/node.token
#   CD_LOG      部署日志                                        缺省：$HG_DIR/../.hgamey/phonefarm-cd.log
#   NODE_LOG    node_service 日志                               缺省：$HG_DIR/../.hgamey/node_service-mbp1.log
#   NODE_PORT=8765  CD_BRANCH=stable  INTERVAL=300
set -u
{  # 整段先读进内存再执行：部署会把本文件切到新版本，bash 边读边执行会读到改过的文件
export PATH=$HOME/.cargo/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
PF_DIR=${PF_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}
HG_DIR=${HG_DIR:-$(cd "$PF_DIR/.." && pwd)}
STATE=$(cd "$HG_DIR/.." && pwd)/.hgamey
NODE_TOKEN_FILE=${NODE_TOKEN_FILE:-$STATE/node.token}
CD_LOG=${CD_LOG:-$STATE/phonefarm-cd.log}
NODE_LOG=${NODE_LOG:-$STATE/node_service-mbp1.log}
NODE_PORT=${NODE_PORT:-8765}; CD_BRANCH=${CD_BRANCH:-stable}; INTERVAL=${INTERVAL:-300}

if [ "${1:-}" = --loop ]; then
  while true; do "$0"; sleep "$INTERVAL"; done
fi

log() { echo "$(date '+%F %T') $*" >> "$CD_LOG"; }
LOCK=$STATE/phonefarm-cd.lock
mkdir "$LOCK" 2>/dev/null || exit 0; trap 'rmdir "$LOCK"' EXIT
TOKEN=$(cat "$NODE_TOKEN_FILE")
cd "$PF_DIR" || exit 1
git fetch -q origin "$CD_BRANCH" 2>>"$CD_LOG" || { log "fetch 失败"; exit 1; }
TARGET=$(git rev-parse "origin/$CD_BRANCH"); PREV=$(git rev-parse HEAD)
[ "$TARGET" = "$PREV" ] && exit 0
busy=$(curl -s -m5 -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$NODE_PORT/queue" \
  | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["active_count"]+d["queued_count"])' 2>/dev/null)
[ "$busy" = 0 ] || { log "$CD_BRANCH=${TARGET:0:7} 待部署, 节点忙 (active+queued=${busy:-?}), 下次再试"; exit 0; }
git diff --quiet HEAD || { log "工作树有未提交改动, 不部署 ${TARGET:0:7}"; exit 1; }

restart_node() {
  pkill -f 'harness-ops/node/node_service.py'; pkill -f "$PF_DIR/phonefarm serve"; sleep 3
  (cd "$HG_DIR" && PHONEFARM_NODE_TOKEN=$TOKEN nohup python3 harness-ops/node/node_service.py --phonefarm "$PF_DIR" \
    --game-opt-loop "$HG_DIR/game_opt_loop" --port "$NODE_PORT" >> "$NODE_LOG" 2>&1 &)
  for _ in $(seq 30); do sleep 2; [ "$(curl -s -m3 "http://127.0.0.1:$NODE_PORT/health")" = ok ] && return 0; done
  return 1
}
build() {  # 先编到 phonefarm.new；签名后能起来才算好（被 macOS 杀掉时退出码 ≥ 128）
  (cd src && cargo build --release -q) >>"$CD_LOG" 2>&1 && cp src/target/release/phonefarm phonefarm.new \
    && codesign --force --sign - phonefarm.new 2>>"$CD_LOG" && { ./phonefarm.new help >/dev/null 2>&1; [ $? -lt 128 ]; }
}

log "部署 ${PREV:0:7} → ${TARGET:0:7}"
git checkout -q -B "$CD_BRANCH" "$TARGET" || { log "checkout 失败"; exit 1; }
if ! build; then log "编译失败, 回到 ${PREV:0:7}"; rm -f phonefarm.new; git checkout -q -B "$CD_BRANCH" "$PREV"; exit 1; fi
cp -p phonefarm phonefarm.prev; mv -f phonefarm.new phonefarm
if restart_node; then log "完成: 现在运行 ${TARGET:0:7}"; exit 0; fi
log "重启后健康检查失败, 回滚到 ${PREV:0:7}"
git checkout -q -B "$CD_BRANCH" "$PREV"; mv -f phonefarm.prev phonefarm
restart_node && log "已回滚, 运行 ${PREV:0:7}" || log "回滚后节点仍起不来, 需要人工处理"
exit 1
}
