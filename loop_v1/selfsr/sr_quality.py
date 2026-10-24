#!/usr/bin/env python3
"""sr_quality.py — 超分算子的离线画质: 真机原生截图当真值, 缩小再放大回来, 与真值比 SSIM 与锐度。

    python3 -I loop_v1/selfsr/sr_quality.py --scale 0.7 --sharp 0.8 --out q.json <原生截图.png>...

跨冷启动的整屏 SSIM 被云、时辰、角色待机动作主导 (同为原生的相邻两臂只有 0.49–0.73), 判不了超分画质;
这里在同一帧上比, 结果是确定性的。截图是屏幕分辨率 (HWC 放大后的 2688x1216), 先缩回游戏送显尺寸
(--native, 缺省 2141x969) 当真值; 低分辨率输入用盒式缩小近似游戏按低分辨率渲染。
对比: bilinear (≈ HWC 现状), lanczos (selfsr_up.comp), selfsr (Lanczos2 + RCAS), selfsr_cr (Catmull-Rom + RCAS, algo=cr)。
Python 实现与两个着色器逐项对应 (Lanczos2 4x4 + 最近 2x2 去振铃; RCAS 十字邻域、负瓣上限 0.1875 × 强度)。
"""
import argparse, json, os, sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from selfsr_ab import ssim  # noqa: E402


def lanczos2(x):
    x = np.abs(x)
    out = np.where(x < 1e-5, 1.0, 0.0)
    m = (x >= 1e-5) & (x < 2)
    px = np.pi * x[m]
    out[m] = 2 * np.sin(px) * np.sin(px / 2) / (px * px)
    return out


def up_lanczos(src, W, H):
    s = np.asarray(src, dtype=np.float64) / 255
    h, w = s.shape[:2]
    oy, ox = np.mgrid[0:H, 0:W]
    py = (oy + 0.5) * h / H - 0.5
    px = (ox + 0.5) * w / W - 0.5
    by, bx = np.floor(py).astype(int), np.floor(px).astype(int)
    fy, fx = py - by, px - bx
    acc = np.zeros((H, W, 3)); ws = np.zeros((H, W)); lo = np.ones((H, W, 3)); hi = np.zeros((H, W, 3))
    for j in range(4):
        for i in range(4):
            c = s[np.clip(by + j - 1, 0, h - 1), np.clip(bx + i - 1, 0, w - 1)]
            wgt = lanczos2(fx - (i - 1)) * lanczos2(fy - (j - 1))
            acc += c * wgt[..., None]; ws += wgt
            if i in (1, 2) and j in (1, 2):
                lo = np.minimum(lo, c); hi = np.maximum(hi, c)
    return np.clip(acc / ws[..., None], lo, hi)


def up_catmull_rom(src, W, H):
    """Catmull-Rom 双三次 (selfsr_fused.comp algo=1 的数学等价: 9 次双线性取样 = 4x4 Catmull-Rom 权重)。"""
    s = np.asarray(src, dtype=np.float64) / 255
    h, w = s.shape[:2]
    oy, ox = np.mgrid[0:H, 0:W]
    py = (oy + 0.5) * h / H - 0.5
    px = (ox + 0.5) * w / W - 0.5
    by, bx = np.floor(py).astype(int), np.floor(px).astype(int)
    fy, fx = py - by, px - bx

    def cr(f):
        return [f * (-0.5 + f * (1 - 0.5 * f)), 1 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2 - 1.5 * f)), f * f * (-0.5 + 0.5 * f)]
    wx, wy = cr(fx), cr(fy)
    acc = np.zeros((H, W, 3))
    for j in range(4):
        for i in range(4):
            c = s[np.clip(by + j - 1, 0, h - 1), np.clip(bx + i - 1, 0, w - 1)]
            acc += c * (wx[i] * wy[j])[..., None]
    return np.clip(acc, 0, 1)


def rcas(s, sharp):
    p = np.pad(s, ((1, 1), (1, 1), (0, 0)), mode="edge")
    c = p[1:-1, 1:-1]; n = p[:-2, 1:-1]; so = p[2:, 1:-1]; w = p[1:-1, :-2]; e = p[1:-1, 2:]
    mn = np.minimum.reduce([n, so, w, e, c]); mx = np.maximum.reduce([n, so, w, e, c])
    hit_min = mn / (4 * mx + 1e-5); hit_max = (1 - mx) / (4 * mn - 4 + 1e-5)
    lobe = np.clip(np.maximum(-hit_min, hit_max).max(axis=2, keepdims=True), -0.1875, 0) * sharp
    return np.clip((lobe * (n + so + w + e) + c) / (4 * lobe + 1), 0, 1)


def to_img(a):
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8))


def lap_var(img):
    g = np.asarray(img.convert("L"), dtype=np.float64)
    l = g[1:-1, 1:-1] * 4 - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:]
    return float(l.var())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shots", nargs="+")
    ap.add_argument("--scale", type=float, default=0.7)
    ap.add_argument("--sharp", type=float, default=0.8)
    ap.add_argument("--native", default="2141x969")
    ap.add_argument("--out", default=None)
    ap.add_argument("--save-dir", default=None, help="把每张的真值/各放大结果存成 png (目检用)")
    a = ap.parse_args()
    W, H = (int(v) for v in a.native.split("x"))
    tmp = a.save_dir or os.path.join(os.path.dirname(os.path.abspath(a.out or ".")), ".sr_quality_tmp")
    os.makedirs(tmp, exist_ok=True)
    rows = []
    for k, shot in enumerate(a.shots):
        gt = Image.open(shot).convert("RGB").resize((W, H), Image.LANCZOS)
        low = gt.resize((max(1, round(W * a.scale)), max(1, round(H * a.scale))), Image.BOX)
        lz = up_lanczos(low, W, H)
        cr = up_catmull_rom(low, W, H)
        outs = {"bilinear": low.resize((W, H), Image.BILINEAR), "lanczos": to_img(lz), "selfsr": to_img(rcas(lz, a.sharp)),
                "selfsr_cr": to_img(rcas(cr, a.sharp))}
        p_gt = os.path.join(tmp, f"{k}_gt.png"); gt.save(p_gt)
        row = {"shot": shot, "gt_sharp": lap_var(gt)}
        for name, img in outs.items():
            p = os.path.join(tmp, f"{k}_{name}.png"); img.save(p)
            row[name] = {"ssim": ssim(p_gt, p), "sharp": lap_var(img)}
        rows.append(row)
        print(f"{os.path.basename(shot)}: 真值锐度 {row['gt_sharp']:.1f} | "
              + " | ".join(f"{n} SSIM {row[n]['ssim']:.4f} 锐度 {row[n]['sharp']:.1f}" for n in outs))
    summ = {n: {"ssim_mean": float(np.mean([r[n]["ssim"] for r in rows])),
                "sharp_ratio_mean": float(np.mean([r[n]["sharp"] / r["gt_sharp"] for r in rows]))}
            for n in ("bilinear", "lanczos", "selfsr", "selfsr_cr")}
    res = {"scale": a.scale, "sharp": a.sharp, "native": [W, H], "n": len(rows), "summary": summ, "rows": rows}
    print(json.dumps(summ, ensure_ascii=False))
    if a.out:
        json.dump(res, open(a.out, "w"), ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
