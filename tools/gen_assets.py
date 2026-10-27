#!/usr/bin/env python3
"""
Remote PKG Installer NextGen — asset generator (icon0/pic0/pic1).

Generates the app icon (512x512) and splash screens (1920x1080) in the
dark-tech theme. Requires Pillow. Committed outputs live in sce_sys/.

Usage: python3 tools/gen_assets.py
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SCE_SYS = ROOT / "sce_sys"

BG = (13, 17, 23)        # #0D1117
PANEL = (22, 27, 34)     # #161B22
BORDER = (48, 54, 61)    # #30363D
TEXT = (230, 237, 243)   # #E6EDF3
DIM = (139, 148, 158)    # #8B949E
ACCENT = (57, 210, 192)  # #39D2C0
GREEN = (63, 185, 80)    # #3FB950

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def gen_icon() -> None:
    """512x512 app icon: rounded panel, stylized download arrow + progress bar."""
    img = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Rounded-square panel.
    d.rounded_rectangle((16, 16, 496, 496), radius=96, fill=BG, outline=BORDER, width=6)

    # Download arrow.
    ax, ay = 256, 208
    d.line((ax, 120, ax, 250), fill=TEXT, width=28)
    d.polygon(
        (
            (ax - 62, 212),
            (ax + 62, 212),
            (ax, 296),
        ),
        fill=ACCENT,
    )

    # Progress bar (accent fill, dim track).
    d.rounded_rectangle((120, 340, 392, 372), radius=16, fill=PANEL, outline=BORDER, width=4)
    d.rounded_rectangle((120, 340, 268, 372), radius=16, fill=ACCENT)

    # Speed ticks.
    for i in range(4):
        x = 176 + i * 56
        d.line((x, 400, x, 412), fill=DIM if i else GREEN, width=8)

    SCE_SYS.mkdir(parents=True, exist_ok=True)
    img.save(SCE_SYS / "icon0.png")


def gen_splash(path: str, title: str) -> None:
    """1920x1080 splash screen: dark bg, title, animated-look bar, footer."""
    img = Image.new("RGB", (1920, 1080), BG)
    d = ImageDraw.Draw(img)

    f_big = load_font(72)
    f_small = load_font(34)

    # Title block.
    d.text((960, 430), title, font=f_big, fill=TEXT, anchor="mm")

    # Accent bar with partial fill.
    bar_x0, bar_x1, bar_y = 660, 1260, 560
    d.rounded_rectangle((bar_x0, bar_y, bar_x1, bar_y + 12), radius=6, fill=PANEL)
    d.rounded_rectangle((bar_x0, bar_y, bar_x0 + 460, bar_y + 12), radius=6, fill=ACCENT)

    d.text((960, 640), "waiting for network...", font=f_small, fill=DIM, anchor="mm")
    d.text((960, 990), "listening on port 12800", font=f_small, fill=DIM, anchor="mm")

    # Corner brackets (HUD look).
    s, t = 60, 6
    for (cx, cy, dx, dy) in ((80, 80, 1, 1), (1840, 80, -1, 1), (80, 1000, 1, -1), (1840, 1000, -1, -1)):
        d.line((cx, cy, cx + dx * s, cy), fill=BORDER, width=t)
        d.line((cx, cy, cx, cy + dy * s), fill=BORDER, width=t)

    img.save(SCE_SYS / path)


def main() -> None:
    SCE_SYS.mkdir(parents=True, exist_ok=True)
    gen_icon()
    gen_splash("pic0.png", "Remote PKG Installer NextGen")
    gen_splash("pic1.png", "Remote PKG Installer NextGen")
    print("assets written to sce_sys/")


if __name__ == "__main__":
    main()
