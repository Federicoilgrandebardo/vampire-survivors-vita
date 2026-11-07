#!/usr/bin/env python3
"""Icone 16x16 delle reliquie del port (arte originale): occhiali 3D e vinili, in assets/relics/.
uso: python3 tools/make_relic_icons.py   (serve Pillow)"""
import os
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'relics')
os.makedirs(OUT, exist_ok=True)
K, W, G = (24, 16, 28, 255), (236, 232, 220, 255), (150, 146, 140, 255)  # contorno, montatura, ombra della montatura

def put(im, rows, pal):
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c in pal: im.putpixel((x, y), pal[c])

# occhiali da cinema 3D: lente sinistra rossa, destra ciano, montatura chiara col bordo scuro
GLASSES = [
    "................",
    "................",
    "................",
    "................",
    "KKKKKKKKKKKKKKKK",
    "KWWWWWWWWWWWWWWK",
    "KWKKKKKWWKKKKKWK",
    "KWKRRrRWWKCCcCWK",
    "KWKRRRRWWKCCCCWK",
    "KWKRRRRKKKCCCCWK",
    "KWKKKKKKKKKKKKWK",
    "KGWWWWK..KWWWWGK",
    ".KKKKK....KKKKK.",
    "................",
    "................",
    "................",
]
im = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
put(im, GLASSES, {'K': K, 'W': W, 'G': G, 'R': (220, 40, 52, 255), 'r': (255, 150, 150, 255), 'C': (40, 210, 230, 255), 'c': (180, 250, 255, 255)})
im.save(os.path.join(OUT, 'glasses3d.png'))

# vinile: disco nero coi solchi, etichetta colorata, foro; un colore per gruppo di stage
VINYL = [
    ".....KKKKKK.....",
    "...KKDDDDDDKK...",
    "..KDDdDDDDdDDK..",
    ".KDdDDDDDDDDdDK.",
    ".KDDDDLLLLDDDDK.",
    "KDDDDLLLLLLDDDDK",
    "KDdDLLLLLLLLDdDK",
    "KDDDLLLKKLLLDDDK",
    "KDDDLLLKKLLLDDDK",
    "KDdDLLLLLLLLDdDK",
    "KDDDDLLLLLLDDDDK",
    ".KDDDDLLLLDDDDK.",
    ".KDdDDDDDDDDdDK.",
    "..KDDdDDDDdDDK..",
    "...KKDDDDDDKK...",
    ".....KKKKKK.....",
]
COLORS = [(230, 60, 60), (240, 180, 40), (70, 190, 90), (60, 140, 240), (170, 80, 220), (240, 120, 200), (80, 220, 220), (240, 240, 240)]
for i, c in enumerate(COLORS):
    im = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    put(im, VINYL, {'K': K, 'D': (40, 38, 46, 255), 'd': (90, 88, 104, 255), 'L': c + (255,)})
    im.save(os.path.join(OUT, f'vinyl{i}.png'))
print('ok', sorted(os.listdir(OUT)))
