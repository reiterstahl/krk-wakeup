"""Rebuild the small, independent KRK Wakeup application icon."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SIZE = 256
SCALE = 4
image = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
draw = ImageDraw.Draw(image)


def rect(values):
    return tuple(int(value * SCALE) for value in values)


draw.rounded_rectangle(rect((4, 4, 252, 252)), radius=54 * SCALE,
                       fill=(24, 28, 27, 255))
# A single bright waveform remains legible at Windows notification icon sizes.
points = [(31, 128), (65, 128), (88, 76), (114, 183),
          (142, 72), (169, 182), (190, 128), (225, 128)]
draw.line([(x * SCALE, y * SCALE) for x, y in points],
          fill=(241, 204, 73, 255), width=19 * SCALE, joint="curve")
for x, y in (points[0], points[-1]):
    radius = 9 * SCALE
    draw.ellipse((x * SCALE - radius, y * SCALE - radius,
                  x * SCALE + radius, y * SCALE + radius),
                 fill=(241, 204, 73, 255))

image = image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
image.save(ROOT / "assets" / "app.ico", sizes=[(16, 16), (24, 24),
                                           (32, 32), (48, 48), (64, 64),
                                           (256, 256)])
image.save("/tmp/krk-wakeup-icon-preview.png")
