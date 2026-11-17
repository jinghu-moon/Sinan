"""Render FTXUI test cell dumps with their actual RGB colors (requires Pillow)."""
import argparse
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def render(source: Path) -> Path:
    rows = json.loads(source.read_text(encoding="utf-8"))
    cell_width, cell_height, margin = 11, 24, 16
    image = Image.new("RGB", (len(rows[0]) * cell_width + margin * 2,
                              len(rows) * cell_height + margin * 2), "#2E3440")
    draw = ImageDraw.Draw(image)
    mono = ImageFont.truetype("C:/Windows/Fonts/CascadiaMono.ttf", 18)
    chinese = ImageFont.truetype("C:/Windows/Fonts/simhei.ttf", 18)

    def rgb(escape: str, fallback: str):
        match = re.search(r"(?:38|48);2;(\d+);(\d+);(\d+)", escape)
        return tuple(map(int, match.groups())) if match else fallback

    for y, row in enumerate(rows):
        for x, cell in enumerate(row):
            left, top = margin + x * cell_width, margin + y * cell_height
            draw.rectangle((left, top, left + cell_width - 1, top + cell_height - 1),
                           fill=rgb(cell["bg"], "#2E3440"))
        for x, cell in enumerate(row):
            text = cell["text"]
            if not text:
                continue
            left, top = margin + x * cell_width, margin + y * cell_height
            foreground = rgb(cell["fg"], "#D8DEE9")
            font = chinese if any(ord(c) >= 0x2E80 for c in text) else mono
            draw.text((left, top + 3), text, font=font, fill=foreground, anchor="lt")
            if cell["underline"]:
                draw.line((left, top + cell_height - 3, left + cell_width - 1,
                           top + cell_height - 3), fill=foreground)
    target = source.with_suffix("").with_suffix(".png")
    image.save(target)
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshots", type=Path, nargs="+")
    for snapshot in parser.parse_args().snapshots:
        print(render(snapshot))
