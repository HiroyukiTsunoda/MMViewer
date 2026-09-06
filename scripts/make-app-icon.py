"""Convert the selected MM ribbon artwork into the embedded Windows icon.

Development-only dependency: Pillow. Normal C++ builds use the checked-in ICO.
"""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "design/icon-candidates/2026-09-05/02-mm-ribbon.png"
OUTPUT = ROOT / "src/native/app.ico"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def main() -> None:
    with Image.open(SOURCE) as source:
        artwork = source.convert("RGBA")
        if artwork.width != artwork.height:
            raise ValueError("The selected icon source must be square")
        artwork.save(OUTPUT, format="ICO", sizes=[(size, size) for size in SIZES])
    with Image.open(OUTPUT) as icon:
        expected = {(size, size) for size in SIZES}
        if icon.ico.sizes() != expected:
            raise RuntimeError("The ICO is missing an expected resolution")
        for size in SIZES:
            frame = icon.ico.getimage((size, size)).convert("RGBA")
            if frame.getchannel("A").getextrema() != (0, 255):
                raise RuntimeError(f"Transparency was not preserved at {size}px")
    print(f"{OUTPUT}: {OUTPUT.stat().st_size:,} bytes; {SIZES}")


if __name__ == "__main__":
    main()
