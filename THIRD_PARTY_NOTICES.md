# Third-party notices

## In the source code

`src/phaser.c` translates parts of Phaser 3.55.2 and of phaser3-rex-plugins to C.
Both are MIT licensed:

    Copyright (c) 2020 Richard Davey, Photon Storm Ltd.
    Copyright (c) 2018 Rex

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

- delaunay-fast (ironwallaby), also translated in `src/phaser.c`: released into
  the public domain by its author.
- cJSON (`src/third_party/`): MIT, Dave Gamble and cJSON contributors. The
  notice is in its files.

## In the VPK

- raylib 5.5: zlib license, Ramon Santamaria. Rinnegatamante's Vita port with
  `patches/raylib-5.5-vita.patch`.
- SDL2, Northfear's Vita port: zlib license, Sam Lantinga.
- vitaGL: GNU LGPL v3, Rinnegatamante. Linked statically: this repository has
  everything needed to rebuild the VPK against a modified vitaGL.

## In the setup tools

- FFmpeg: the bundled binary is a GPL v3 build including x264, run as a separate
  program. Source: https://ffmpeg.org/download.html
- imageio-ffmpeg: BSD 2-Clause, imageio contributors.
- QuickJS: MIT, Fabrice Bellard and Charlie Gordon. Python bindings by PetterS.
- PyInstaller: GPL with an exception that allows bundling applications.
