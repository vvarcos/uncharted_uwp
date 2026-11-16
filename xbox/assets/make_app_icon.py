#!/usr/bin/env python3
"""Draws the UWP app artwork (xbox/assets/AppIcon.png) from scratch.

Original artwork: procedural parchment, faint antique chart lines, a generic
explorer bust silhouette and a compass rose, with a heavy wordmark. It is not
the game's cover: no official key art, screenshot, logo, character likeness or
official typeface is used or traced. Regenerate with:

    python xbox/assets/make_app_icon.py

Needs Pillow. xbox/scripts/assets.ps1 derives the UWP logos from the result.
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

WIDTH, HEIGHT = 900, 1350
ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / "gpu/third_party/fonts/DejaVuSans.ttf"
OUT = Path(__file__).resolve().parent / "AppIcon.png"

PARCHMENT_TOP = (245, 237, 219)
PARCHMENT_BOTTOM = (223, 207, 176)
INK = (33, 27, 20)
SEPIA = (122, 98, 66)
BROWN = (150, 122, 84)


def gradient(top, bottom):
    image = Image.new("RGB", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(image)
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        draw.line([(0, y), (WIDTH, y)],
                  fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return image


def parchment():
    base = gradient(PARCHMENT_TOP, PARCHMENT_BOTTOM).convert("RGBA")
    random.seed(7)

    stains = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(stains)
    for _ in range(46):
        cx, cy = random.uniform(0, WIDTH), random.uniform(0, HEIGHT)
        rx, ry = random.uniform(60, 260), random.uniform(60, 260)
        shade = random.choice([(198, 174, 132), (240, 230, 206), (180, 152, 108)])
        draw.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=shade + (random.randint(10, 26),))
    base = Image.alpha_composite(base, stains.filter(ImageFilter.GaussianBlur(46)))

    noise = Image.effect_noise((WIDTH, HEIGHT), 42).convert("L").filter(ImageFilter.GaussianBlur(1.2))
    noise = noise.point(lambda v: 128 + (v - 128) // 3)
    tint = Image.new("RGBA", (WIDTH, HEIGHT), (210, 190, 156, 0))
    tint.putalpha(noise.point(lambda v: 26 if v < 128 else 14))
    return Image.alpha_composite(base, tint)


def antique_map(layer):
    draw = ImageDraw.Draw(layer)
    random.seed(23)
    for _ in range(24):
        x, y = random.uniform(-40, WIDTH + 40), random.uniform(-40, HEIGHT + 40)
        points = [(x, y)]
        heading = random.uniform(0, math.tau)
        for _ in range(random.randint(9, 18)):
            heading += random.uniform(-0.7, 0.7)
            step = random.uniform(40, 90)
            x += math.cos(heading) * step
            y += math.sin(heading) * step
            points.append((x, y))
        draw.line(points, fill=BROWN + (44,), width=random.choice((1, 2)), joint="curve")

    for offset in (-120, 60, 240, 420, 600):
        draw.arc([offset, -520, offset + 980, 1480], start=200, end=340, fill=BROWN + (34,), width=2)
    for y in range(120, HEIGHT, 210):
        draw.arc([-260, y, WIDTH + 260, y + 260], start=200, end=340, fill=BROWN + (26,), width=1)

    route = [(70, 1210), (210, 1150), (310, 1180), (430, 1105), (560, 1140), (690, 1070), (850, 1110)]
    for (x1, y1), (x2, y2) in zip(route, route[1:]):
        steps = max(2, int(math.dist((x1, y1), (x2, y2)) / 16))
        for i in range(steps):
            if i % 2:
                continue
            t = i / steps
            draw.ellipse([x1 + (x2 - x1) * t - 2, y1 + (y2 - y1) * t - 2,
                          x1 + (x2 - x1) * t + 2, y1 + (y2 - y1) * t + 2], fill=BROWN + (90,))
    for x, y in route[1:-1]:
        draw.ellipse([x - 6, y - 6, x + 6, y + 6], outline=BROWN + (100,), width=2)


def compass(layer, cx, cy, radius):
    draw = ImageDraw.Draw(layer)
    draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], outline=SEPIA + (105,), width=3)
    draw.ellipse([cx - radius * 0.78, cy - radius * 0.78, cx + radius * 0.78, cy + radius * 0.78],
                 outline=SEPIA + (70,), width=1)
    for i in range(8):
        angle = i * math.tau / 8 - math.pi / 2
        inner = radius * (0.22 if i % 2 else 0.32)
        half = math.tau / 32
        tip = (cx + math.cos(angle) * radius * 0.9, cy + math.sin(angle) * radius * 0.9)
        left = (cx + math.cos(angle - half) * inner, cy + math.sin(angle - half) * inner)
        right = (cx + math.cos(angle + half) * inner, cy + math.sin(angle + half) * inner)
        fill = SEPIA + (115,) if i % 2 == 0 else SEPIA + (70,)
        draw.polygon([tip, left, (cx, cy), right], fill=fill)


def fade_bottom(layer, solid_until, floor=0.0):
    mask = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(mask)
    for y in range(solid_until, HEIGHT):
        t = (y - solid_until) / (HEIGHT - solid_until)
        draw.line([(0, y), (WIDTH, y)], fill=int(255 * (1 - (1 - floor) * t)))
    mask = mask.filter(ImageFilter.GaussianBlur(8))
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), mask))


def figure(layer):
    """A generic explorer bust: head, short neck, shoulders and upper chest."""
    body = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(body)
    tone = (54, 45, 33, 246)

    # Shoulders and chest, fading into the parchment below the neckline.
    draw.polygon([(232, 830), (286, 566), (382, 486), (518, 486), (614, 566), (668, 830)], fill=tone)
    # Neck.
    draw.polygon([(430, 372), (470, 372), (482, 502), (418, 502)], fill=tone)
    # Head and short hair.
    draw.ellipse([394, 240, 506, 404], fill=tone)
    draw.ellipse([386, 226, 514, 344], fill=(38, 31, 23, 248))

    # Open collar.
    draw.polygon([(432, 500), (468, 500), (450, 586)], fill=(66, 55, 42, 250))
    draw.line([(398, 520), (450, 622)], fill=(26, 21, 15, 255), width=7)
    draw.line([(502, 520), (450, 622)], fill=(26, 21, 15, 255), width=7)
    draw.line([(414, 540), (450, 630)], fill=(232, 220, 196, 100), width=2)
    draw.line([(486, 540), (450, 630)], fill=(232, 220, 196, 100), width=2)

    # Chain and pendant.
    chain = []
    for i in range(41):
        t = i / 40
        chain.append((402 + (498 - 402) * t, 528 + math.sin(t * math.pi) * 72))
    draw.line(chain, fill=(32, 26, 19, 235), width=3, joint="curve")
    for i in range(0, 41, 3):
        x, y = chain[i]
        draw.ellipse([x - 3, y - 3, x + 3, y + 3], outline=(32, 26, 19, 240), width=1)
    draw.ellipse([428, 618, 472, 662], outline=(32, 26, 19, 240), width=4)
    draw.ellipse([436, 626, 464, 654], outline=(32, 26, 19, 175), width=2)

    fade_bottom(body, 700, 0.0)
    layer.alpha_composite(body)


def fit(size, text, max_width, tracking):
    while size > 40:
        font = ImageFont.truetype(str(FONT), size)
        measure = ImageDraw.Draw(Image.new("RGBA", (10, 10)))
        width = sum(measure.textlength(ch, font=font) for ch in text) + tracking * (len(text) - 1)
        if width <= max_width:
            return font
        size -= 4
    return ImageFont.truetype(str(FONT), size)


def spaced_text(draw, center_x, y, text, font, tracking, fill, stroke=0, stroke_fill=None):
    widths = [draw.textlength(ch, font=font) for ch in text]
    total = sum(widths) + tracking * (len(text) - 1)
    x = center_x - total / 2
    for ch, width in zip(text, widths):
        draw.text((x, y), ch, font=font, fill=fill, stroke_width=stroke, stroke_fill=stroke_fill)
        x += width + tracking
    return total


def title(layer):
    draw = ImageDraw.Draw(layer)
    font = fit(124, "UNCHARTED", 780, -1)
    spaced_text(draw, WIDTH / 2, 884, "UNCHARTED", font, -1, INK, 2, INK)

    sub = fit(31, "THE NATHAN DRAKE COLLECTION", 700, 7)
    spaced_text(draw, WIDTH / 2, 1024, "THE NATHAN DRAKE COLLECTION", sub, 7, (44, 35, 25, 255))

    draw.line([(330, 1092), (570, 1092)], fill=(44, 35, 25, 170), width=2)


def finish(image):
    vignette = Image.new("L", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(vignette)
    draw.ellipse([-380, -360, WIDTH + 380, HEIGHT + 360], fill=255)
    vignette = vignette.filter(ImageFilter.GaussianBlur(160)).point(lambda v: int(50 * (1 - v / 255)))
    dark = Image.new("RGBA", (WIDTH, HEIGHT), (58, 44, 27, 0))
    dark.putalpha(vignette)
    image = Image.alpha_composite(image, dark)

    border = Image.new("L", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(border)
    draw.rectangle([0, 0, WIDTH - 1, HEIGHT - 1], outline=255, width=20)
    border = border.filter(ImageFilter.GaussianBlur(16)).point(lambda v: int(v * 0.32))
    edge = Image.new("RGBA", (WIDTH, HEIGHT), (96, 74, 46, 0))
    edge.putalpha(border)
    return Image.alpha_composite(image, edge)


def main():
    image = parchment()
    drawn = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    antique_map(drawn)
    compass(drawn, 168, 300, 80)
    figure(drawn)
    title(drawn)
    image = Image.alpha_composite(image, drawn)
    image = finish(image)
    image = ImageChops.multiply(image.convert("RGB"),
                                Image.new("RGB", (WIDTH, HEIGHT), (251, 248, 241))).convert("RGBA")
    image.save(OUT)
    print(f"wrote {OUT} ({image.width}x{image.height})")


if __name__ == "__main__":
    main()
