from argparse import ArgumentParser
import sys
from pathlib import *
from PIL import Image

_SCRIPT_PATH = Path(__file__).resolve()
_WIDTH = 320
_HEIGHT = 240


def main():
    parser = ArgumentParser()
    parser.add_argument('-i', '--input', type=str, required=True)
    args = parser.parse_args()

    try:
        img = Image.open(args.input)
        img = img.resize((_WIDTH, _HEIGHT), Image.BILINEAR)
        img = img.convert('RGB')

        pixels = img.load()
        data = bytearray()
        for y in range(_HEIGHT):
            for x in range(_WIDTH):
                r, g, b = pixels[x, y]

                r = r * 31 // 255
                g = g * 63 // 255
                b = b * 31 // 255

                r5g6b5 = r << 11 | g << 5 | b

                data += r5g6b5.to_bytes(2, byteorder='big')

        open(_SCRIPT_PATH.parent / '../main/image.c', 'wt').write(f"""unsigned char image_bin[] = {{ { ", ".join(f"{b:#02x}" for b in data) } }};
unsigned int image_len = {len(data)};
""")
        sys.exit(0)
    except RuntimeError as e:
        print()
        print(e)
        sys.exit(-1)


if __name__ == "__main__":
    main()
