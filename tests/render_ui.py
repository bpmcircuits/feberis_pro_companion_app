#!/usr/bin/env python3
"""Render the production dashboard with fonts/decoder from a firmware checkout.

python3 tests/render_ui.py --u8g2 /path/to/flipperzero-firmware/lib/u8g2
No network access or hardware required. Needs a C compiler and Pillow.
"""
import argparse
import platform
import re
import subprocess
import tempfile
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--u8g2', required=True, type=Path)
parser.add_argument('--output', type=Path, default=root / 'dist/ui')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
font_source = (args.u8g2 / 'u8g2_fonts.c').read_text(encoding='latin1')
with tempfile.TemporaryDirectory(prefix='feberis-ui-') as build:
    build = Path(build)
    fonts = '#include "u8g2.h"\n'
    for name in ('u8g2_font_helvB08_tr', 'u8g2_font_haxrcorp4089_tr'):
        match = re.search(r'const uint8_t ' + name + r'\[.*?";\n', font_source, re.S)
        if not match:
            raise SystemExit(f'Firmware font missing: {name}')
        fonts += match.group() + '\n'
    (build / 'fonts.c').write_text(fonts)
    binary = build / 'render'
    linker = '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'
    subprocess.run(['cc', '-std=c11', '-DFBR_UI_RENDER', '-I' + str(args.u8g2),
                    '-ffunction-sections', '-fdata-sections',
                    str(root / 'dashboard.c'), str(root / 'ui_model.c'),
                    str(root / 'tests/render_dashboard.c'),
                    str(args.u8g2 / 'u8g2_font.c'), str(args.u8g2 / 'u8x8_8x8.c'),
                    str(build / 'fonts.c'), linker, '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(args.output)], check=True)

names = ['offline', 'connecting', 'ready', 'recording', 'stopping', 'error',
         'large_counts', 'wifi_error']
sheet = Image.new('RGB', (1104, 1192), '#ededed')
draw = ImageDraw.Draw(sheet)
for i, name in enumerate(names):
    x, y = 16 + (i % 2) * 552, 12 + (i // 2) * 296
    draw.text((x, y), name.replace('_', ' '), fill='black')
    tokens = (args.output / (name + '.pbm')).read_text().split()
    assert tokens[:3] == ['P1', '128', '64']
    image = Image.new('L', (128, 64))
    image.putdata([0 if int(pixel) else 255 for pixel in tokens[3:]])
    image.save(args.output / (name + '.png'))
    sheet.paste(image.convert('RGB').resize((512, 256), Image.NEAREST), (x, y + 24))
sheet.save(args.output / 'dashboard-preview.png')
print(args.output / 'dashboard-preview.png')
