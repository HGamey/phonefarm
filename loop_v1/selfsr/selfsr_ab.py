#!/usr/bin/env python3
"""selfsr_ab.py — 自研超分/插帧的两臂 ABBA (冷启动、进大世界、同一套测量)。不改游戏内任何设置。

    python3 -I loop_v1/selfsr/selfsr_ab.py run --serial <S> --out <目录> --a native --b layer [--rounds 6]

臂的种类 (--a / --b):
  native              原生: 不挂层, Game Mode 无干预
  layer               只挂透传层 (gray-layer session --mode probe), 第 2 步用
  hwc:<F>             Android Game Mode 降分辨率 F (cmd game mode custom + cmd game set --downscale F), 放大交给 HWC
  ip:<合成方式>[:<帧率>[:键=值,...]]  挂层并开层内插帧 (interp_mode=dup/dupcur/bm/real, interp_fps=给窗口投的帧率,
                      可选 pacing/stamp/acquire_ms/extra_images/lead_ns → debug.knobs.interp_<键>)
  srip:<F>:<宽>x<高>:<锐化>:<合成方式>[:<帧率>]  超分 + 插帧叠加 (第 5 步)
  <挂层的臂>+gfrc:<三位>  自研层与 G-FRC+ 同开 (例 sr:1:2688x1216:0.8+gfrc:001)
  gfrc:<三位>          厂商 G-FRC+ (game_gfrc_mode 本包位, 例 101 = 只超分, 111 = 超分+插帧), 对标用
  sr:<F>:<宽>x<高>[:<锐化>]  同上降分辨率 F, 再挂层并设 debug.knobs.selfsr=<宽>x<高>[:<锐化>]: 层在送显前把游戏的小图超分到 <宽>x<高>
每臂: (devlock 下) 设好 Game Mode 与 selfsr 属性 → 冷启动进大世界 → 静置 --settle 秒 → --window 秒窗口并行采
  - SurfaceFlinger timestats: 上屏帧率 / 刷新率 / 丢帧
  - ftrace + phonefarm parse-trace: 游戏 GPU 提交 → 游戏渲染帧率 / p95 帧时
  - 截图: 锐度 (拉普拉斯方差), 与本轮第一张 native 截图的 SSIM (跨冷启动场景会有云/时辰差异, 只作参考)
  - 游戏图层送显缓冲尺寸 (HWC 源); 挂层臂另读层 marker (层加载、窗口内帧数、selfsr 实际尺寸与计数)
  不挂层的臂整臂在 tools/devlock 下跑; 挂层的臂由 gray-layer session 持设备租约, 挂层并冷启动游戏,
  onboard --enter-only --no-cold 只导航不重启 (重启会丢层), 采完 stop 会话 (会话负责摘层)。
  会话自带的 sample 在本机 ftrace 子进程起不来 (worker exit 76), 所以测量不用它。
收尾 (含 Ctrl-C): Game Mode 恢复原模式并 reset 干预、清 debug.knobs.selfsr、screen_off_timeout 还原, 逐项回读核对。
统计: 每对 (B - A) 配对差, 精确符号翻转检验 + bootstrap 95% 区间。
"""
import argparse, json, os, re, shlex, signal, statistics, subprocess, sys, time, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "loop_v1", "gfrc"))
from gfrc_ab import Dev, parse_timestats, layer_src, sharpness, signflip, boot_ci, set_pkg_bits  # noqa: E402

PKG_DEFAULT = "com.miHoYo.Yuanshen"
DEV_TRACE = "/data/local/tmp/selfsr_ftrace_capture.sh"
SR_PROP = "debug.knobs.selfsr"
SR_OPTS = ("algo", "direct", "unlimit")   # → debug.knobs.selfsr_<键>; unlimit → debug.knobs.unlimit
UNLIMIT_PROP = "debug.knobs.unlimit"
# 截图 (2688x1216) 右上角那排固定图标: 整屏锐度被场景主导 (冷启动后角色/镜头可能不同), 这块内容各臂相同
HUD_BOX = (1760, 10, 2600, 130)
IP_PROPS = ("debug.knobs.interp", "debug.knobs.interp_mode", "debug.knobs.interp_fps")
IP_TUNE = ("pacing", "stamp", "acquire_ms", "extra_images", "lead_ns", "mult", "backend", "sc_dst", "panel", "framepacing",
           "unlimit")
FP_PROP = "vendor.perf.framepacing.enable"   # 高通 perf HAL 的游戏帧节奏; framepacing=<值> 进世界后设, 收尾写回原值   # panel 不是层属性, 见 prepare   # ip 臂第 4 段可设的 debug.knobs.interp_<键>


def parse_kind(s):
    if "+gfrc:" in s:   # <挂层的臂>+gfrc:<三位>  自研层 + 厂商 G-FRC+ 同时开 (G-FRC+ 开着时系统放开限帧)
        base, bits = s.rsplit("+gfrc:", 1)
        k = parse_kind(base)
        if not uses_layer(k) or len(bits) != 3 or not bits.isdigit():
            raise argparse.ArgumentTypeError(f"+gfrc 只接在挂层的臂后面, 三位数字: {s}")
        return {**k, "gfrc": bits}
    p = s.split(":")
    if p[0] in ("native", "layer") and len(p) == 1:
        return {"kind": p[0], "downscale": None, "selfsr": None}
    if p[0] == "layer" and len(p) == 2 and p[1] == "unlimit=1":   # 透传层 + 解开红魔 libgui 进程内限帧
        return {"kind": "layer", "downscale": None, "selfsr": None, "unlimit": True}
    if p[0] == "gfrc" and len(p) == 2 and len(p[1]) == 3:   # gfrc:<画质档><帧率档><总开关>  厂商 G-FRC+ (对标用)
        return {"kind": "gfrc", "downscale": None, "selfsr": None, "gfrc": p[1]}
    if p[0] == "ip" and len(p) in (2, 3, 4):   # ip:<合成方式>[:<投帧率>[:键=值,...]]  层内插帧 (interp.cpp)
        tune = dict(kv.split("=", 1) for kv in p[3].split(",")) if len(p) == 4 and p[3] else {}
        if any(k not in IP_TUNE for k in tune) or ("panel" in tune and tune["panel"] not in PANEL_MODE):
            raise argparse.ArgumentTypeError(f"ip 臂只认这些键: {IP_TUNE}")
        return {"kind": "ip", "downscale": None, "selfsr": None, "interp": p[1], "fps": p[2] if len(p) >= 3 else "",
                "tune": tune}
    if p[0] == "hwc" and len(p) == 2:
        return {"kind": "hwc", "downscale": float(p[1]), "selfsr": None}
    if p[0] == "srip" and len(p) in (5, 6, 7) and "x" in p[2]:   # srip:<F>:<宽>x<高>:<锐化>:<合成方式>[:<投帧率>[:键=值,...]]
        tune = dict(kv.split("=", 1) for kv in p[6].split(",")) if len(p) == 7 and p[6] else {}
        if any(k not in IP_TUNE for k in tune):
            raise argparse.ArgumentTypeError(f"srip 臂的插帧参数只认: {IP_TUNE}")
        return {"kind": "srip", "downscale": float(p[1]), "selfsr": f"{p[2]}:{p[3]}", "interp": p[4],
                "fps": p[5] if len(p) >= 6 else "", "tune": tune}
    if p[0] == "sr" and len(p) in (3, 4, 5) and "x" in p[2]:   # sr:<F>:<宽>x<高>[:<锐化>[:algo=cr,direct=1]]
        opts = dict(kv.split("=", 1) for kv in p[4].split(",")) if len(p) == 5 and p[4] else {}
        if any(k not in SR_OPTS for k in opts):
            raise argparse.ArgumentTypeError(f"sr 臂只认这些键: {SR_OPTS}")
        return {"kind": "sr", "downscale": float(p[1]), "selfsr": ":".join(p[2:4]), "sr_opts": opts}
    raise argparse.ArgumentTypeError(f"不认识的臂: {s}")


def uses_layer(k):
    return k["kind"] in ("layer", "sr", "ip", "srip")


def pf_json(pf, *args, inp=None, timeout=600):
    r = subprocess.run([pf, *args], input=inp, capture_output=True, text=True, timeout=timeout)
    try:
        return r.returncode, json.loads(r.stdout)
    except ValueError:
        return r.returncode, {"raw": r.stdout[-2000:], "err": r.stderr[-2000:]}


def session_action(pf, out, action, params=None):
    _, d = pf_json(pf, "gray-layer", "session-status", "--out", out, "--json")
    c = d.get("controller") or {}
    rid = f"selfsr-{action}-{uuid.uuid4().hex[:8]}"
    req = {"schema": "phonefarm/gray-control-action@1", "request_id": rid, "session_id": d.get("session_id"),
           "actor_id": c.get("actor_id"), "control_epoch": c.get("epoch"), "action": action, "params": params or {}}
    code, rec = pf_json(pf, "gray-layer", "session-action", "--out", out, "--request", "-", "--json",
                        inp=json.dumps(req))
    for _ in range(300):
        if code != 0 or rec.get("status") in ("completed", "failed", "rejected", "unknown"):
            break
        time.sleep(1)
        code, rec = pf_json(pf, "gray-layer", "session-status", "--out", out, "--request-id", rid, "--json")
    return rec


def ssim(png_a, png_b):
    """灰度 SSIM (8x8 不重叠块的均值), 两图尺寸不同时把 b 缩放到 a。"""
    from PIL import Image
    import numpy as np
    a = Image.open(png_a).convert("L")
    b = Image.open(png_b).convert("L")
    if b.size != a.size:
        b = b.resize(a.size, Image.BICUBIC)
    x = np.asarray(a, dtype=np.float64)
    y = np.asarray(b, dtype=np.float64)
    h, w = (x.shape[0] // 8) * 8, (x.shape[1] // 8) * 8
    x = x[:h, :w].reshape(h // 8, 8, w // 8, 8)
    y = y[:h, :w].reshape(h // 8, 8, w // 8, 8)
    mx, my = x.mean(axis=(1, 3)), y.mean(axis=(1, 3))
    vx, vy = x.var(axis=(1, 3)), y.var(axis=(1, 3))
    cxy = ((x - mx[:, None, :, None]) * (y - my[:, None, :, None])).mean(axis=(1, 3))
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    s = ((2 * mx * my + c1) * (2 * cxy + c2)) / ((mx ** 2 + my ** 2 + c1) * (vx + vy + c2))
    return float(s.mean())


def game_mode(dev, pkg, downscale, orig_mode):
    """设 Game Mode 干预; downscale=None 表示无干预。返回核对信息。"""
    if downscale is None or downscale >= 1:   # >=1: 不降分辨率 (满分辨率超分只换放大)
        dev.shell(f"cmd game reset {pkg}")
        if orig_mode and orig_mode not in ("unknown", ""):
            dev.shell(f"cmd game mode {orig_mode} {pkg}")
        cfg = dev.shell(f"cmd game list-configs {pkg}")
        return {"downscale": downscale, "ok": "downscale" not in cfg.lower() or "no intervention" in cfg.lower(), "configs": cfg[-300:]}
    o1 = dev.shell(f"cmd game mode custom {pkg}")
    o2 = dev.shell(f"cmd game set --downscale {downscale} {pkg}")
    cfg = dev.shell(f"cmd game list-configs {pkg}")
    return {"downscale": downscale, "ok": f"{downscale}" in cfg, "configs": cfg[-300:], "out": (o1 + o2)[-200:]}


def current_mode(dev, pkg):
    t = dev.shell(f"cmd game list-modes {pkg}")
    return t.split("current mode:")[1].split(",")[0].strip() if "current mode:" in t else "unknown"


def timestats_layer(text, needle):
    """timestats 里名字含 needle 的图层的 totalFrames (取帧数最多的那个) 与全局 displayOnTime。"""
    g = {"displayOnTime": None, "layer_frames": None}
    m = re.search(r"^displayOnTime = (\d+)", text, re.M)
    g["displayOnTime"] = int(m.group(1)) if m else None
    for blk in text.split("layerName = ")[1:]:
        if needle in blk.splitlines()[0]:
            f = re.search(r"^totalFrames = (\d+)", blk, re.M)
            if f and int(f.group(1)) > (g["layer_frames"] or 0):
                g["layer_frames"] = int(f.group(1))
    return g


def measure(dev, a, name, out):
    dev_trace = f"/data/local/tmp/selfsr_trace_{name}.txt"
    ts_cmd = ("dumpsys SurfaceFlinger --timestats -disable -clear >/dev/null; "
              "dumpsys SurfaceFlinger --timestats -enable >/dev/null; "
              f"sleep {a.window}; dumpsys SurfaceFlinger --timestats -dump -maxlayers 8; "
              "dumpsys SurfaceFlinger --timestats -disable >/dev/null")
    tr = subprocess.Popen(f"adb -s {shlex.quote(a.serial)} shell su -c "
                          + shlex.quote(shlex.quote(f"sh {DEV_TRACE} {a.window} {dev_trace} {a.pkg}")),
                          shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ts_text = dev.shell(ts_cmd, timeout=a.window + 60)
    ts = parse_timestats(ts_text, a.pkg)
    ts_sc = timestats_layer(ts_text, "selfsr_interp")   # 插帧子图层后端的图层
    tr.wait(timeout=a.window + 90)
    local_trace = os.path.join(out, "traces", f"{name}.txt")
    dev.adb(f"exec-out su -c {shlex.quote('cat ' + dev_trace)} > {shlex.quote(local_trace)}")
    dev.su(f"rm -f {dev_trace}")
    pt = subprocess.run([a.phonefarm, "parse-trace", local_trace], capture_output=True, text=True)
    try:
        game = json.loads(pt.stdout)
    except ValueError:
        game = {}
    fg = dev.shell("dumpsys window | grep mCurrentFocus")
    sf = dev.shell("dumpsys SurfaceFlinger")
    src = layer_src(sf, a.pkg)
    # 游戏图层在 SF 里的帧率票 / HWC 合成方式: 插帧时上屏率被谁限住要看这里
    keep = [ln for i, ln in enumerate(sf.splitlines()) if a.pkg[:12] in ln or "Frame Rate" in ln or "frameRate" in ln
            or "FrameRateOverride" in ln or "refresh" in ln.lower()][:80]
    open(os.path.join(out, "traces", f"{name}_sf.txt"), "w").write("\n".join(keep) + "\n")
    shot = os.path.join(out, "shots", f"{name}.png")
    dev.adb(f"exec-out screencap -p > {shlex.quote(shot)}")
    span = (ts.get("displayOnTime") or a.window * 1000) / 1000
    return {"in_game": a.pkg in fg,
            "display_fps": round(ts["layer_frames"] / span, 2) if ts.get("layer_frames") else None,
            "display_hz": ts.get("display_hz"), "missed_frames": ts.get("missedFrames"),
            "display_fps_sc": round(ts_sc["layer_frames"] / ((ts_sc.get("displayOnTime") or a.window * 1000) / 1000), 2)
                              if ts_sc.get("layer_frames") else None,
            "game_fps": game.get("fps_mean"), "game_frame_p95_ms": game.get("frame_p95"),
            "gpu_active_ms": game.get("gpu_active_mean"),
            "out_res": src, "sharp_full": sharpness(shot), "sharp_hud": sharpness(shot, HUD_BOX), "shot": shot}


def uptime_ns(dev):
    """手机的 CLOCK_MONOTONIC (≈ /proc/uptime, 不含休眠), 与层 CSV 里的时刻同一个钟。"""
    try:
        return int(float(dev.shell("cat /proc/uptime").split()[0]) * 1e9)
    except (ValueError, IndexError):
        return None


def interp_window(dev, a, name, out, t0, t1):
    """层的 knobs_interp_frames.csv 里 [t0, t1] 内实际上屏的帧: 每秒几张真实帧 / 合成帧, 上屏间隔。"""
    import statistics as st
    path = f"/storage/emulated/0/Android/data/{a.pkg}/files/knobs_interp_frames.csv"
    local = os.path.join(out, "traces", f"{name}_interp.csv")
    dev.adb(f"exec-out su -c {shlex.quote('cat ' + path)} > {shlex.quote(local)}")
    rows = []
    for ln in open(local, errors="replace"):
        f = ln.strip().split(",")
        if len(f) >= 8 and f[0] == "timing":
            try:
                rows.append((int(f[7]), f[2]))
            except ValueError:
                pass
    # /proc/uptime 是 BOOTTIME, 层的时刻是 CLOCK_MONOTONIC (差了累计休眠), 对不齐;
    # 改为以 CSV 里最后一条上屏时刻为终点往前取一个测量窗口 (CSV 在测量结束后才拉, 每 64 行落一次盘)
    if not rows:
        return {"status": "no_rows"}
    end = max(r[0] for r in rows)
    span = (t1 - t0) / 1e9 if (t0 and t1 and t1 > t0) else float(a.window)
    win = sorted(r for r in rows if end - span * 1e9 <= r[0] <= end)
    if len(win) < 2:
        return {"status": "no_samples", "rows": len(rows)}
    iv = [(b[0] - a_[0]) / 1e6 for a_, b in zip(win, win[1:])]
    return {"span_s": round(span, 2), "displayed_per_s": round(len(win) / span, 2),
            "real_per_s": round(sum(1 for r in win if r[1] == "real") / span, 2),
            "synth_per_s": round(sum(1 for r in win if r[1] == "synth") / span, 2),
            "interval_p50_ms": round(st.median(iv), 2), "interval_max_ms": round(max(iv), 2)}


def layer_absent(dev, pkg):
    lib = dev.su(f"ls -d /data/app/*/{pkg}-*/lib/arm64").strip().splitlines()
    has_so = any(dev.su(f"ls {shlex.quote(d)} | grep -c libVkLayer_refknobs").strip() not in ("0", "") for d in lib)
    prop = dev.prop("debug.vulkan.layers")
    return (not has_so) and prop == "", {"so_present": has_so, "debug.vulkan.layers": prop}


def enter_by_script(a, out, name, cold):
    """--enter-script: 给 onboard 认不出「已在游戏里」的游戏用的固定进门脚本。
    步骤用 ; 分隔: s:<秒> 等待, t:<x>,<y> 点屏幕 (横屏下的比例坐标)。cold 时先冷启动游戏。"""
    dev = Dev(a.serial)
    log = []
    if cold:
        dev.shell(f"am force-stop {a.pkg}")
        dev.shell(f"monkey -p {a.pkg} -c android.intent.category.LAUNCHER 1")
    for step in a.enter_script.split(";"):
        op, _, arg = step.strip().partition(":")
        if op == "s":
            time.sleep(float(arg))
        elif op == "t":
            fx, fy = (float(v) for v in arg.split(","))
            m = re.search(r" cur=(\d+)x(\d+)", dev.shell("dumpsys window displays | grep -m1 ' cur='"))
            w, h = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
            if not w:
                return 1, "读不到屏幕尺寸"
            dev.shell(f"input tap {int(fx * w)} {int(fy * h)}")
        log.append(step)
    fg = dev.shell("dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity'")
    os.makedirs(os.path.join(out, "onboard"), exist_ok=True)
    json.dump({"steps": log, "foreground": fg.strip()}, open(os.path.join(out, "onboard", f"{name}.json"), "w"), ensure_ascii=False)
    return (0, "") if a.pkg in fg else (1, f"脚本走完但前台不是 {a.pkg}: {fg.strip()[:200]}")


def onboard(a, out, name, cold):
    if getattr(a, "enter_script", None):
        return enter_by_script(a, out, name, cold)
    args = [a.phonefarm, "onboard", "--package", a.pkg, "--serial", a.serial, "--enter-only", "--no-guest",
            "--out", os.path.join(out, "onboard", name)]
    if not cold:
        # 挂层臂: 设备租约在 gray-layer session 手里, onboard 自己再拿锁会一直排队; 在会话的租约内导航
        args += ["--no-cold", "--no-devlock"]
    r = subprocess.run(args, capture_output=True, text=True, timeout=900)
    return r.returncode, r.stderr[-600:]


PANEL_KEYS = ("refresh_rate_mode",)
# 红魔 system refresh_rate_mode: 1/2/3/4 = 60/90/120/144 Hz, 0 = 自动。2026-10-24 panel_probe.sh 实测原神在前台时
# 设 4 屏幕跑 144.5 Hz; AOSP 的 min/peak_refresh_rate 设 144 无效 (仍 60 Hz)。
PANEL_MODE = {"60": 1, "90": 2, "120": 3, "144": 4}
PANEL_ORIG = {"path": None}   # <输出目录>/panel_orig.json: 开跑前的原值 (run 写, prepare/restore 读)


def restore_panel(dev):
    try:
        orig = json.load(open(PANEL_ORIG["path"]))
    except (OSError, ValueError, TypeError):
        return
    for key in PANEL_KEYS:
        v = orig.get(key)
        dev.shell(f"settings delete system {key}" if v in (None, "null") else f"settings put system {key} {v}")
    if orig.get(FP_PROP) not in (None, "") and dev.prop(FP_PROP) != orig[FP_PROP]:
        dev.su(f"setprop {FP_PROP} {shlex.quote(orig[FP_PROP])}")


def gfrc_after_enter(dev, a, k):
    """进世界后写本包 G-FRC+ 位并回读厂商属性核对。游戏助手会记住上次开过 G-FRC+, 启动时自己再打开
    (全局设置只是镜像, 启动前写 000 管不住), 所以每一臂都在进世界后明确写一次: G-FRC+ 臂写目标位, 其它臂写 000。
    返回 (属性, 错误或 None)。本机没有 G-FRC+ (属性不存在) 时什么都不做。"""
    if dev.prop("vendor.gpp.frc.enable") in ("", None):
        return {}, None
    bits = k.get("gfrc") or "000"
    cur = dev.shell("settings get global game_gfrc_mode").strip()
    if cur in ("", "null"):
        return {}, None
    dev.shell(f"settings put global game_gfrc_mode {shlex.quote(set_pkg_bits(cur, a.pkg, bits))}")
    time.sleep(8)
    props = {p: dev.prop(f"vendor.gpp.{p}") for p in ("gfrc.interp.rate", "gfrc.upscale.ratio", "frc.enable")}
    want_up = bits[2] == "1" and bits[0] != "0"
    want_ip = bits[2] == "1" and bits[1] != "0"
    if ((props["gfrc.upscale.ratio"] not in ("", "0")) != want_up) or ((props["gfrc.interp.rate"] not in ("", "0")) != want_ip):
        return props, f"G-FRC+ 没按 {bits} 生效: {props}"
    return props, None


def prepare(a, k, orig_mode):
    """在设备锁下调用: Game Mode 与 selfsr 属性。"""
    dev = Dev(a.serial)
    dev.shell(f"am force-stop {a.pkg}")
    gm = game_mode(dev, a.pkg, k["downscale"], orig_mode)
    dev.su(f"setprop {SR_PROP} {shlex.quote(k['selfsr'] or '')}")
    # 厂商 G-FRC+: 只改 settings global game_gfrc_mode 里本包那一项; 收尾写回开跑前原值 (orig.json)
    cur = dev.shell("settings get global game_gfrc_mode").strip()
    if cur not in ("", "null"):
        # 先一律关; G-FRC+ 要在进世界之后再开才生效 (见 arm_plain)
        dev.shell(f"settings put global game_gfrc_mode {shlex.quote(set_pkg_bits(cur, a.pkg, '000'))}")
    so = k.get("sr_opts") or {}
    dev.su("; ".join(f"setprop debug.knobs.selfsr_{o} {shlex.quote(so.get(o, ''))}" for o in SR_OPTS if o != "unlimit"))
    unl = k.get("unlimit") or so.get("unlimit") == "1" or (k.get("tune") or {}).get("unlimit") == "1"
    dev.su(f"setprop {UNLIMIT_PROP} {'1' if unl else shlex.quote('')}")
    mode = k.get("interp")
    dev.su(f"setprop {IP_PROPS[0]} {'1' if mode else shlex.quote('')}; setprop {IP_PROPS[1]} {shlex.quote(mode or '')}; "
           f"setprop {IP_PROPS[2]} {shlex.quote(k.get('fps') or '')}")
    tune = k.get("tune") or {}
    dev.su("; ".join(f"setprop debug.knobs.interp_{t} {shlex.quote(str(tune.get(t, '')))}" for t in IP_TUNE if t not in ("panel", "framepacing", "unlimit")))
    # panel=<Hz>: 屏幕刷新率 (红魔 refresh_rate_mode), 收尾 (restore) 按开跑前记下的原值写回
    # 刷新率在进世界之后才设 (arm_layer): 原神启动时游戏助手会把它压回 60 Hz
    restore_panel(dev)
    return {"game_mode": gm, "selfsr_prop": dev.prop(SR_PROP), "interp_props": [dev.prop(p) for p in IP_PROPS]}


def arm_plain(a, out, name, k, orig_mode):
    """不挂层的臂 (整臂在 devlock 下被调用)。"""
    dev = Dev(a.serial)
    rec = {"prep": prepare(a, k, orig_mode)}
    code, err = onboard(a, out, name, cold=True)
    if code != 0:
        rec["error"] = f"onboard: {err}"
        return rec
    rec["gfrc_props"], err = gfrc_after_enter(dev, a, k)
    if err:
        rec["error"] = err
        return rec
    time.sleep(a.settle)
    rec.update(measure(dev, a, name, out))
    dev.shell(f"am force-stop {a.pkg}")
    return rec


def arm_layer(a, out, name, k, note, locked):
    dev = Dev(a.serial)
    rec = {}
    r = locked("prep", name, k)
    rec["prep"] = r.get("prep")
    if r.get("error"):
        rec["error"] = r["error"]
        return rec
    sdir = os.path.join(out, "sessions", name)
    log = open(sdir + ".log", "w")
    cmd = [a.phonefarm, "gray-layer", "session", "--mode", "probe", "--package", a.pkg, "--serial", a.serial,
           "--out", sdir, "--max-seconds", str(a.session_seconds)]
    if a.so:
        cmd += ["--so", a.so]
    if a.keep_layer_prop:
        cmd += ["--keep-prop"]
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    st = None
    try:
        phase = None
        for _ in range(450):
            time.sleep(2)
            if proc.poll() is not None:
                break
            code, s = pf_json(a.phonefarm, "gray-layer", "session-status", "--out", sdir, "--json")
            phase = s.get("phase") if code == 0 else None
            if phase == "ready":
                break
        if phase != "ready":
            rec["error"] = f"session not ready (phase={phase}, exit={proc.poll()})"
            return rec
        code, err = onboard(a, out, name, cold=False)
        if code != 0:
            rec["error"] = f"onboard --no-cold: {err}"
            return rec
        rec["gfrc_props"], err = gfrc_after_enter(dev, a, k)
        if err:
            rec["error"] = err
            return rec
        panel = (k.get("tune") or {}).get("panel")
        if panel:   # 进世界后再设刷新率 (红魔 refresh_rate_mode), 回读 SF 刷新周期确认
            dev.shell(f"settings put system refresh_rate_mode {PANEL_MODE[str(panel)]}")
            time.sleep(4)
            per = dev.shell("dumpsys SurfaceFlinger | grep -m1 mPeriod")
            rec["panel_period"] = per.strip()
            m = re.search(r"\(([0-9.]+) fps\)", per)
            if not m or abs(float(m.group(1)) - float(panel)) > 3:
                rec["error"] = f"屏幕没切到 {panel} Hz: {per.strip()}"
                return rec
        fpv = (k.get("tune") or {}).get("framepacing")
        if fpv is not None:   # 进世界后设高通帧节奏开关, 回读核对
            dev.su(f"setprop {FP_PROP} {shlex.quote(str(fpv))}")
            rec["framepacing_now"] = dev.prop(FP_PROP)
        time.sleep(a.settle)
        code, s = pf_json(a.phonefarm, "gray-layer", "session-status", "--out", sdir, "--json")
        if proc.poll() is not None or s.get("phase") != "ready":
            rec["error"] = f"会话在测量前已结束 (phase={s.get('phase')}, stop={s.get('stop_reason')})"
            return rec

        def marker():
            m = session_action(a.phonefarm, sdir, "marker")
            try:
                return json.load(open(os.path.join(sdir, m["result"]["path"])))
            except Exception:
                return {}
        m0 = marker()
        t0 = uptime_ns(dev)
        rec.update(measure(dev, a, name, out))
        t1 = uptime_ns(dev)
        m1 = marker()
        if k["kind"] in ("ip", "srip"):   # 窗口内层回读的实际上屏 (会话累计的环形统计混着加载期, 不可用)
            rec["interp_window"] = interp_window(dev, a, name, out, t0, t1)
            iw = rec["interp_window"]
            rec["displayed_per_s"] = iw.get("displayed_per_s")
        rec["layer_loaded"] = bool(m0.get("layer_loaded") and m1.get("layer_loaded"))
        f0 = (m0.get("readonly_stats") or {}).get("frames")
        f1 = (m1.get("readonly_stats") or {}).get("frames")
        rec["layer_frames_in_window"] = (f1 - f0) if (f0 is not None and f1 is not None) else None
        if k["kind"] in ("sr", "srip"):
            s0, s1 = m0.get("selfsr") or {}, m1.get("selfsr") or {}
            rec["selfsr"] = s1
            up = (s1.get("upscaled") or 0) - (s0.get("upscaled") or 0)
            rec["selfsr_upscaled_in_window"] = up
            # 层的 marker 每 300 帧落一次盘, 游戏帧率低时窗口内可能一次没落: 判「在超分」看累计次数
            if (s1.get("upscaled") or 0) <= 0:
                rec["error"] = f"层没在超分 (selfsr={s1})"
        rec["unlimit"] = m1.get("unlimit")
        if k["kind"] in ("ip", "srip"):
            ipj = m1.get("interp") or {}
            rec["interp"] = ipj
            lat = ipj.get("real_present_to_display_ns") or {}
            # 真实帧 present→上屏 (整个会话累计); 插帧带来的延迟 = 与 ip:real 对照臂之差
            rec["display_latency_p50_ms"] = round(lat["p50"] / 1e6, 3) if lat.get("p50") else None
        if not rec["layer_loaded"]:
            rec["error"] = "层没加载 (marker layer_loaded 不为真)"
    finally:
        if proc.poll() is None:
            st = session_action(a.phonefarm, sdir, "stop")
        try:
            proc.wait(timeout=300)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=60)
        rec["session_exit"] = proc.returncode
        rec["session_stop"] = (st or {}).get("status")
        dev.shell(f"am force-stop {a.pkg}")
        ok, info = layer_absent(dev, a.pkg)
        rec["layer_removed"] = ok
        rec["layer_removed_info"] = info
        note(f"{name}: 会话退出 {proc.returncode}, 层已摘除 {ok} {info}")
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["run", "_locked"])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--pkg", default=PKG_DEFAULT)
    ap.add_argument("--out", required=True)
    ap.add_argument("--a", type=parse_kind, default=parse_kind("native"))
    ap.add_argument("--b", type=parse_kind, default=parse_kind("layer"))
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--so", default=None, help="层 .so (挂层臂); 缺省用 gray-layer session 自己的")
    ap.add_argument("--enter-script", default=None,
                    help="onboard 认不出进了游戏时用固定脚本进门: 's:<秒>;t:<x>,<y>;…' (横屏比例坐标), 冷启动由脚本自己做")
    ap.add_argument("--keep-layer-prop", action="store_true",
                    help="整个会话保留 debug.vulkan.layers (Unity 先建一个探测用 VkInstance、再建真正的那种游戏需要; 原神不需要)")
    ap.add_argument("--settle", type=float, default=20)
    ap.add_argument("--window", type=int, default=10)
    ap.add_argument("--session-seconds", type=int, default=1200)
    ap.add_argument("--cool-max", type=float, default=0, help="每臂开跑前电池温度要降到这个 °C 以下 (0 = 不等), 最多等 15 分钟")
    ap.add_argument("--fan", action="store_true", help="整轮开机身风扇最高档 (红魔 /sys/kernel/fan), 收尾写回原值; 没有该节点的机器跳过")
    ap.add_argument("--phonefarm", default=os.environ.get("PHONEFARM_BIN") or os.path.join(REPO, "phonefarm"))
    # _locked 内部用
    ap.add_argument("--what", default=None)
    ap.add_argument("--name", default=None)
    ap.add_argument("--kind", default=None)
    ap.add_argument("--orig-mode", default="standard")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    PANEL_ORIG["path"] = os.path.join(out, "panel_orig.json")
    for d in ("shots", "traces", "sessions", "onboard"):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    if a.cmd == "_locked":
        k = parse_kind(a.kind)
        if a.what == "plain":
            rec = arm_plain(a, out, a.name, k, a.orig_mode)
        elif a.what == "prep":
            rec = {"prep": prepare(a, k, a.orig_mode)}
            if not rec["prep"]["game_mode"]["ok"]:
                rec["error"] = f"Game Mode 没设上: {rec['prep']['game_mode']}"
        else:  # restore
            dev = Dev(a.serial)
            dev.shell(f"am force-stop {a.pkg}")
            gm = game_mode(dev, a.pkg, None, a.orig_mode)
            dev.su(f"setprop {SR_PROP} ''")
            dev.su("; ".join(f"setprop debug.knobs.selfsr_{o} ''" for o in SR_OPTS if o != "unlimit"))
            dev.su(f"setprop {UNLIMIT_PROP} ''")
            try:
                og = json.load(open(os.path.join(out, "orig.json"))).get("game_gfrc_mode")
            except (OSError, ValueError):
                og = None
            if og not in (None, "", "null"):
                dev.shell(f"settings put global game_gfrc_mode {shlex.quote(og)}")
            dev.su("; ".join(f"setprop {p} ''" for p in IP_PROPS))
            dev.su("; ".join(f"setprop debug.knobs.interp_{t} ''" for t in IP_TUNE if t not in ("panel", "framepacing", "unlimit")))
            restore_panel(dev)
            rec = {"game_mode": gm, "mode_now": current_mode(dev, a.pkg), "selfsr_prop": dev.prop(SR_PROP),
                   "game_gfrc_mode": dev.shell("settings get global game_gfrc_mode").strip(),
                   "interp_props": [dev.prop(p) for p in IP_PROPS]}
        print("ARM_JSON " + json.dumps(rec, ensure_ascii=False))
        return 0

    log = open(os.path.join(out, "run.log"), "a")

    def note(msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    raw = sys.argv
    ka = raw[raw.index("--a") + 1] if "--a" in raw else "native"
    kb = raw[raw.index("--b") + 1] if "--b" in raw else "layer"

    def locked(what, name, k_str):
        # 标签带序列号「<标签>@<序列号>」: 看板与远程调试据此只把这一台当作测量中 (harness-ops 的约定)
        cmd = [os.path.join(REPO, "tools", "devlock"), "run", f"selfsr-{what}-{name}@{a.serial}", "--", sys.executable, "-I",
               os.path.abspath(__file__), "_locked", "--what", what, "--name", name, "--kind", k_str,
               "--orig-mode", orig_mode, "--serial", a.serial, "--pkg", a.pkg, "--out", out,
               "--settle", str(a.settle), "--window", str(a.window), "--phonefarm", a.phonefarm]
        if a.enter_script:
            cmd += ["--enter-script", a.enter_script]
        r = None
        for _ in range(90):   # devlock run 轮不到时立即返回 1 (不等), 有界重试
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800,
                               env={**os.environ, "PHONEFARM_BIN": a.phonefarm})
            if r.returncode == 0 or not any(s in r.stderr for s in ("not your turn", "busy:")):
                break
            time.sleep(10)
        line = next((l for l in r.stdout.splitlines() if l.startswith("ARM_JSON ")), None)
        return json.loads(line[9:]) if line else {"error": f"{what} exit {r.returncode}: {r.stderr[-400:]}"}

    dev = Dev(a.serial)
    ok, info = layer_absent(dev, a.pkg)
    if not ok:
        note(f"开跑前设备上已有层残留, 拒绝开始: {info}")
        return 2
    orig_mode = current_mode(dev, a.pkg)
    orig_cfg = dev.shell(f"cmd game list-configs {a.pkg}")
    orig_sr = dev.prop(SR_PROP)
    orig_timeout = dev.shell("settings get system screen_off_timeout").strip()
    json.dump({**{key: dev.shell(f"settings get system {key}").strip() for key in PANEL_KEYS}, FP_PROP: dev.prop(FP_PROP)},
              open(PANEL_ORIG["path"], "w"))
    json.dump({"game_mode": orig_mode, "game_configs": orig_cfg, "selfsr_prop": orig_sr,
               "game_gfrc_mode": dev.shell("settings get global game_gfrc_mode").strip(),
               "screen_off_timeout": orig_timeout}, open(os.path.join(out, "orig.json"), "w"), ensure_ascii=False)
    if "no intervention" not in orig_cfg.lower() and "downscale" in orig_cfg.lower():
        note(f"开跑前本包已有 Game Mode 干预, 拒绝开始 (不覆盖别人的设置): {orig_cfg[-200:]}")
        return 2
    # 机身风扇: 红魔 /sys/kernel/fan (enable + 档位 1..5); 荣耀 hn_thermal/fan (目标转速, 0 = 关, 最高 fan_max_rpm)
    FAN = "/sys/kernel/fan"
    HFAN = "/sys/devices/virtual/hn_thermal/fan"
    fan_orig = None   # ("redmagic", enable, level) | ("honor", target_rpm)
    if a.fan:
        fo = dev.su(f"cat {FAN}/fan_enable {FAN}/fan_speed_level 2>/dev/null").split()
        ho = dev.su(f"cat {HFAN}/fan_target_rpm {HFAN}/fan_max_rpm 2>/dev/null").split()
        if len(fo) == 2:
            fan_orig = ("redmagic", fo[0], fo[1])
            dev.su(f"echo 1 > {FAN}/fan_enable; echo 5 > {FAN}/fan_speed_level")
            note(f"风扇 (红魔): 原值 enable={fo[0]} level={fo[1]}, 本轮开到 1/5")
        elif len(ho) == 2:
            fan_orig = ("honor", ho[0])
            dev.su(f"echo {ho[1]} > {HFAN}/fan_target_rpm")
            note(f"风扇 (荣耀): 原目标转速 {ho[0]}, 本轮 {ho[1]}")
        else:
            note("风扇: 本机没有已知的风扇节点, 跳过")
    dev.shell("settings put system screen_off_timeout 1800000")
    dev.adb(f"push {shlex.quote(os.path.join(REPO, 'loop_v1/tools/ftrace_capture.sh'))} {DEV_TRACE}", check=True)
    stop = {"flag": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(flag=True))
    signal.signal(signal.SIGTERM, lambda *_: stop.update(flag=True))
    note(f"A={ka} B={kb} rounds={a.rounds} 原 Game Mode={orig_mode}")

    order = []
    for i in range(a.rounds):
        order += [("A", i), ("B", i)] if i % 2 == 0 else [("B", i), ("A", i)]
    arms = []
    ref_shot = None
    try:
        for seq, (arm, pair) in enumerate(order):
            if stop["flag"]:
                note("收到中止信号, 停在这里")
                break
            k, k_str = (a.a, ka) if arm == "A" else (a.b, kb)
            name = f"{arm}{pair + 1}"
            cool_wait = 0
            if a.cool_max > 0:   # 等电池降温 (过热时系统高温保护会弹窗挡前台、限制应用运行)
                dev.shell(f"am force-stop {a.pkg}; input keyevent KEYCODE_SLEEP")
                for _ in range(90):
                    try:
                        t_c = int(re.search(r"temperature: (\d+)", dev.shell("dumpsys battery")).group(1)) / 10
                    except (AttributeError, ValueError):
                        break
                    if t_c <= a.cool_max:
                        break
                    time.sleep(10)
                    cool_wait += 10
                if cool_wait:
                    note(f"{name}: 等降温 {cool_wait}s (电池 ≤ {a.cool_max}°C)")
            dev.shell("input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard")   # 无密码锁屏的机器 (荣耀) 也要解锁
            if uses_layer(k):
                rec = arm_layer(a, out, name, k, note, lambda w, n, _k: locked(w, n, k_str))
            else:
                rec = locked("plain", name, k_str)
            rec = {"arm": name, "side": arm, "kind": k_str, "pair": pair + 1, "seq": seq + 1, **rec, "cool_wait_s": cool_wait,
                   "t": time.time()}
            if rec.get("shot") and os.path.exists(rec["shot"]):
                if k["kind"] == "native" and ref_shot is None and not rec.get("error"):
                    ref_shot = rec["shot"]
                rec["ssim_vs_native_ref"] = ssim(ref_shot, rec["shot"]) if ref_shot else None
            arms.append(rec)
            open(os.path.join(out, "arms.jsonl"), "a").write(json.dumps(rec, ensure_ascii=False) + "\n")
            if rec.get("error"):
                note(f"{name} ({k_str}): 作废 — {rec['error']}")
            else:
                note(f"{name} ({k_str}): 显示 {rec.get('display_fps')} fps @{rec.get('display_hz')}Hz 游戏 {rec.get('game_fps')} fps "
                     f"p95 {rec.get('game_frame_p95_ms')} ms 锐度 {rec.get('sharp_full', 0):.1f} 输出 {rec.get('out_res')}"
                     + (f" 层帧数 {rec.get('layer_frames_in_window')}" if uses_layer(k) else "")
                     + (f" 窗口上屏 {rec.get('interp_window')}" if rec.get('interp_window') else "")
                     + (f" 超分 {rec.get('selfsr_upscaled_in_window')} 次 {(rec.get('selfsr') or {}).get('game_extent')}→{(rec.get('selfsr') or {}).get('swap_extent')}" if k['kind'] in ('sr', 'srip') else ""))
            if uses_layer(k) and not rec.get("layer_removed", False):
                note(f"{name}: 层没摘干净, 中止整轮 (先人工核对再继续)")
                break
    finally:
        r = locked("restore", "end", "native")
        if fan_orig and fan_orig[0] == "redmagic":
            dev.su(f"echo {fan_orig[1]} > {FAN}/fan_enable; echo {fan_orig[2]} > {FAN}/fan_speed_level")
            r["fan_restored"] = dev.su(f"cat {FAN}/fan_enable {FAN}/fan_speed_level").split()
        elif fan_orig and fan_orig[0] == "honor":
            dev.su(f"echo {fan_orig[1]} > {HFAN}/fan_target_rpm")
            r["fan_restored"] = dev.su(f"cat {HFAN}/fan_target_rpm").split()
        dev.shell(f"settings put system screen_off_timeout {orig_timeout}")
        ok, info = layer_absent(dev, a.pkg)
        json.dump({**r, "layer_absent": ok, "layer_info": info}, open(os.path.join(out, "restore.json"), "w"), ensure_ascii=False)
        note(f"收尾核对: Game Mode {r.get('mode_now')} (原 {orig_mode}), selfsr 属性 '{r.get('selfsr_prop')}', "
             f"层已不在 {ok}, screen_off_timeout 还原为 {orig_timeout}")

    good = [r for r in arms if not r.get("error") and r.get("in_game")
            and (not uses_layer(parse_kind(r["kind"])) or (r.get("layer_loaded") and r.get("layer_removed")))]
    pairs = sorted({r["pair"] for r in good if sum(1 for x in good if x["pair"] == r["pair"]) == 2})
    metrics = ["display_fps", "display_fps_sc", "game_fps", "game_frame_p95_ms", "gpu_active_ms", "sharp_full", "sharp_hud", "ssim_vs_native_ref", "missed_frames",
               "display_latency_p50_ms", "displayed_per_s"]
    res = {}
    for m in metrics:
        av = [next(x.get(m) for x in good if x["pair"] == p and x["side"] == "A") for p in pairs]
        bv = [next(x.get(m) for x in good if x["pair"] == p and x["side"] == "B") for p in pairs]
        if not pairs or any(v is None for v in av + bv):
            res[m] = {"status": "not_measured"}
            continue
        d = [y - x for x, y in zip(av, bv)]
        ma = statistics.mean(av)
        res[m] = {"a_mean": round(ma, 4), "b_mean": round(statistics.mean(bv), 4), "diff_mean": round(statistics.mean(d), 4),
                  "diff_pct": round(100 * statistics.mean(d) / ma, 2) if ma else None,
                  "ci95": [round(x, 4) for x in boot_ci(d)] if len(d) > 1 else None, "p": signflip(d), "n_pairs": len(d)}
    report = {"pkg": a.pkg, "a": ka, "b": kb, "rounds": a.rounds, "valid_pairs": len(pairs),
              "dropped_arms": [r["arm"] for r in arms if r not in good],
              "out_res": {side: sorted({tuple(r["out_res"]) for r in good if r["side"] == side and r.get("out_res")}) for side in "AB"},
              "metrics": res, "min_p_possible": 2 / 2 ** len(pairs) if pairs else None}
    json.dump(report, open(os.path.join(out, "report.json"), "w"), ensure_ascii=False, indent=1)
    names = {"display_fps": "上屏帧率", "display_fps_sc": "子图层上屏帧率 (插帧 sc 后端)", "game_fps": "游戏渲染帧率 (GPU 提交)", "game_frame_p95_ms": "游戏渲染 p95 帧时 (ms)",
             "gpu_active_ms": "每帧 GPU 忙碌 (ms, 均值)",
             "display_latency_p50_ms": "真实帧 present→上屏 p50 (ms)",
             "displayed_per_s": "层回读: 窗口内每秒实际上屏 (真实+合成)",
             "sharp_full": "锐度 (拉普拉斯方差)", "sharp_hud": "右上角固定图标区锐度", "ssim_vs_native_ref": "与第一张原生截图的 SSIM", "missed_frames": "SF 丢帧 / 窗口"}
    md = [f"# {ka} (A) vs {kb} (B): {a.pkg}", "",
          f"每臂冷启动进大世界, 有效 {len(pairs)} 对, 精确符号翻转检验 (最小可能 p = {report['min_p_possible']})。"
          f"游戏内设置未改。作废臂: {report['dropped_arms'] or '无'}。送显缓冲: {report['out_res']}", "",
          "| 指标 | A | B | 差 (B-A) | 差% | 95% 区间 | p |", "|---|---|---|---|---|---|---|"]
    for m in metrics:
        r = res[m]
        if r.get("status"):
            md.append(f"| {names[m]} | - | - | - | - | - | 未测成 |")
            continue
        md.append(f"| {names[m]} | {r['a_mean']} | {r['b_mean']} | {r['diff_mean']:+} | {r['diff_pct']}% | {r['ci95']} | {r['p']:.4f} |")
    open(os.path.join(out, "report.md"), "w").write("\n".join(md) + "\n")
    print("\n".join(md))
    return 0


if __name__ == "__main__":
    sys.exit(main())
