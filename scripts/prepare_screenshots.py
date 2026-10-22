"""Re-encode lossless device-capture previews for the catalog's HID exception."""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OLD_BACKGROUND = (247, 162, 48)
OLD_FOREGROUND = (24, 21, 13)
CATALOG_BACKGROUND = (0, 0, 0)
CATALOG_FOREGROUND = (254, 138, 44)


def prepare(source: Path, target: Path) -> None:
    image = Image.open(source).convert("RGB")
    assert image.size == (512, 256), f"Unexpected resolution: {source}"
    colors = image.getcolors(maxcolors=3)
    assert colors and {color for _, color in colors} == {OLD_BACKGROUND, OLD_FOREGROUND}, source
    # Recover the original 128x64 framebuffer exactly; reject interpolated pixels.
    frame = image.resize((128, 64), Image.Resampling.NEAREST)
    expanded = frame.resize((512, 256), Image.Resampling.NEAREST)
    assert expanded.tobytes() == image.tobytes(), "Source is not a lossless 4x capture"
    converted = Image.new("RGB", image.size)
    pixels = image.load()
    converted.putdata([
        CATALOG_FOREGROUND if pixels[x, y] == OLD_FOREGROUND else CATALOG_BACKGROUND
        for y in range(image.height) for x in range(image.width)
    ])
    target.parent.mkdir(parents=True, exist_ok=True)
    converted.save(target)
    saved = Image.open(target)
    assert saved.size == (512, 256)
    colors = saved.getcolors(maxcolors=3)
    assert colors and {color for _, color in colors} == {CATALOG_BACKGROUND, CATALOG_FOREGROUND}
    print(f"Prepared {target.relative_to(ROOT)}")


if __name__ == "__main__":
    for source, name in [("preview.png", "paused.png"), ("preview-infinite.png", "infinite.png")]:
        prepare(ROOT / "docs" / source, ROOT / "ham/catalog/screenshots" / name)
