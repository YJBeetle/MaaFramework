#!/usr/bin/env python3
"""在真机 iPhone 的无边记里画一个苹果 logo —— iOS 控制器的端到端样例。

    python sample/python/ios_freeform_apple.py
    python sample/python/ios_freeform_apple.py --wifi 10.24.24.7

流水线（`sample/resource/pipeline/ios_freeform_apple.json`）负责"到画布里去、并把笔拿起来"：
起无边记 -> 认到看板列表就点新建 -> 认到底部工具条就点笔图标 -> 认到右上角蓝色对勾才动手画。

中间那一步不是多余的装饰：**画板默认不在绘画模式**，这时单指拖动是"平移画布"，一笔画都
不留（实测过，四条不同节奏的折线全部只挪动了点阵网格）。点了笔图标之后右上角才会冒出那个
蓝色对勾，看到它才代表可以下笔。

"画"这件事交给自定义动作：pipeline 的 `Swipe` 只能走直线，而苹果轮廓要的是一条**连续**的
笔画，抬笔重按会留下接缝。所以用 touch_down / touch_move / touch_up 描完整条。
"""
import argparse
import math
import time
from pathlib import Path

import cv2
import numpy as np

from maa.controller import IOSController
from maa.custom_action import CustomAction
from maa.resource import Resource
from maa.tasker import Tasker

SAMPLE_ROOT = Path(__file__).resolve().parents[1]

resource = Resource()

# 苹果轮廓：两条闭合笔画（带咬口的果身 + 一片斜叶子），坐标在 logo 自己的 0..1 里、y 向下。
#
# 点表不是手描的：真实轮廓每一段都是圆弧，所以直接拿圆来搭（左右两个大圆出顶部两瓣、
# 下面一个圆补满底部、右侧减一个圆当咬口；叶子是两个圆相交的透镜形），取 mask 外轮廓再
# approxPolyDP 抽稀。手描折点的版本我们也试过，顶部凹口会画成一个尖 V，一眼就不对。
BODY = [
    (0.132, 0.362), (0.081, 0.401), (0.036, 0.458), (0.008, 0.525),
    (0.000, 0.591), (0.006, 0.648), (0.025, 0.703), (0.053, 0.750),
    (0.095, 0.795), (0.115, 0.838), (0.145, 0.883), (0.185, 0.924),
    (0.237, 0.961), (0.296, 0.986), (0.358, 0.999), (0.416, 1.000),
    (0.481, 0.988), (0.541, 0.964), (0.590, 0.931), (0.645, 0.874),
    (0.671, 0.833), (0.688, 0.794), (0.718, 0.765), (0.747, 0.724),
    (0.770, 0.670), (0.782, 0.607), (0.777, 0.539), (0.737, 0.549),
    (0.691, 0.545), (0.651, 0.525), (0.620, 0.490), (0.606, 0.459),
    (0.601, 0.424), (0.606, 0.388), (0.625, 0.349), (0.567, 0.331),
    (0.502, 0.326), (0.446, 0.336), (0.391, 0.358), (0.331, 0.334),
    (0.265, 0.326), (0.199, 0.334),
]
LEAF = [
    (0.465, 0.001), (0.457, 0.027), (0.453, 0.047), (0.452, 0.070),
    (0.453, 0.093), (0.459, 0.122), (0.467, 0.146), (0.477, 0.166),
    (0.491, 0.187), (0.502, 0.200), (0.526, 0.223), (0.542, 0.235),
    (0.568, 0.249), (0.593, 0.257), (0.618, 0.263), (0.648, 0.265),
    (0.668, 0.264), (0.676, 0.238), (0.679, 0.219), (0.680, 0.196),
    (0.679, 0.172), (0.673, 0.144), (0.665, 0.120), (0.655, 0.100),
    (0.641, 0.078), (0.630, 0.065), (0.606, 0.042), (0.590, 0.031),
    (0.564, 0.017), (0.540, 0.008), (0.514, 0.002), (0.485, 0.000),
]

# 相邻两个触摸报告之间的间隔。设备靠帧间差值算笔速，太密会被当成抖动，实测 12~20ms
# 是可用区间，取中间值。
kStepMs = 18


def _densify(pts, per_seg=2):
    """每段再插点：抽稀后的折线在 500px 画布上仍有 15~30px 的直边，不插会看出一圈多边形。"""
    out = []
    n = len(pts)
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        for k in range(per_seg):
            t = k / per_seg
            out.append((ax + (bx - ax) * t, ay + (by - ay) * t))
    out.append(pts[0])
    return out


def _strokes():
    """密化后按整体外接框归一到 0..1，保持宽高比（x、y 用同一个分母）。"""
    all_pts = [_densify(p) for p in (BODY, LEAF)]
    xs = [x for pts in all_pts for x, _ in pts]
    ys = [y for pts in all_pts for _, y in pts]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    span = max(x1 - x0, y1 - y0)
    return [[((x - x0) / span, (y - y0) / span) for x, y in pts] for pts in all_pts]


def _canvas(image_shape):
    """画布上能下笔的那块正方形。

    下边界停在 0.83h：绘画模式右下角那枚笔尖指示（当前笔/颜色）就在那儿，压在它上面
    会被当成点按钮，换支笔之后半张图都对不上。
    """
    height, width = image_shape[:2]
    top = int(height * 0.18)
    bottom = int(height * 0.83)
    side = min(int(width * 0.72), bottom - top)
    return (width - side) // 2, top, side


@resource.custom_action("DrawAppleLogo")
class DrawAppleLogo(CustomAction):
    def run(self, context, argv: CustomAction.RunArg) -> bool:
        controller = context.tasker.controller
        image = controller.cached_image
        if image is None or not image.size:
            print("no cached screenshot; cannot map the logo")
            return False

        left, top, side = _canvas(image.shape)
        before = image.copy()
        box = (slice(top, top + side), slice(left, left + side))

        perimeter = 0
        for stroke in _strokes():
            points = [(int(left + x * side), int(top + y * side)) for x, y in stroke]
            perimeter += sum(
                math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:])
            )
            if not controller.post_touch_down(*points[0]).wait():
                return False
            for x, y in points[1:]:
                if not controller.post_touch_move(x, y).wait():
                    return False
                time.sleep(kStepMs / 1000.0)
            if not controller.post_touch_up().wait():
                return False
            time.sleep(0.2)

        # 判"画出来了"不能拿整幅差分：画布自己会动（选中态、点阵网格跟着平移）。只数下笔
        # 那块矩形里变掉的像素，下限按实际笔迹长度算：周长 x 笔宽，再留一半余量。
        # 笔宽是在 720 缩放的截图上量出来的（一条直线 406px 长、变化 2436px => 6px 宽）。
        controller.post_screencap().wait()
        after = controller.cached_image
        if after is None or not after.size:
            return False
        changed = int((np.abs(after[box].astype(int) - before[box].astype(int)).sum(axis=2) > 40).sum())
        stroke_px = max(4, image.shape[1] // 120)
        expect = int(perimeter * stroke_px * 0.5)
        print(f"drawn: canvas box changed {changed} px, need >= {expect}")
        cv2.imwrite("/tmp/ios_apple_after.png", after)
        return changed >= expect


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--udid", default="", help="设备 UDID，留空用唯一在连的那台")
    ap.add_argument("--wifi", default="", help="设备的局域网地址；留空走 USB")
    args = ap.parse_args()

    if not resource.post_bundle(SAMPLE_ROOT / "resource").wait():
        raise RuntimeError("failed to load sample resource")

    controller = IOSController(udid=args.udid, wifi_address=args.wifi)
    if not controller.post_connection().wait():
        raise RuntimeError(
            "failed to connect the iPhone. Over Wi-Fi this usually means there is no "
            "RemotePairing record on this machine; see docs/zh_cn/2.4-控制方式说明.md"
        )
    print("connected:", controller.info)

    tasker = Tasker()
    if not tasker.bind(resource, controller):
        raise RuntimeError("failed to bind resource and controller")

    job = tasker.post_task("StartFreeform")
    job.wait()
    detail = job.get()
    if detail is None:
        raise RuntimeError("task disappeared")
    # bool(detail) 只说明"拿到了一个对象"，跟成没成没关系，别看它。
    print("task succeeded:", detail.status.succeeded)
    for node in detail.nodes:
        print(f"  {node.name:14s} completed={node.completed}")


if __name__ == "__main__":
    main()
