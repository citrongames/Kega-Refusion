# MMPX Advanced v3.5

The GLSL port comes from [Libretro's MMPX Advanced shader][shader] at commit
`26bd6a692f2df44cd049b33397a5ab6b3026ee01`. The upstream shader's SHA-256 is
`fedeb89e83e02eff302c83ed1b055ee90e7eb81d3fa7a121b5c645060dc44c3f`.

Morgan McGuire and Mara Gagiu developed [MMPX][paper]. hunterk adapted it to
Slang, and crashGG developed MMPX Advanced. The MIT license and copyright
notices are included in `assets/shaders/mmpx-advanced.glsl`. The base
implementation's copyright notice and license are in `supplement/code/README.md`
in the [authors' sample code][supplement].

The port uses the application's GLSL 1.20 shader bindings. It stores each
upstream packed color as the exact RGB24 integer in a float. The upstream
algorithm uses a constant packed alpha, so RGB24 preserves its color
comparisons. The preset runs once at 2× source resolution with nearest
sampling and transparent black texture borders, matching the [Libretro
preset][preset].

## MIT license

Copyright 2020 Morgan McGuire & Mara Gagiu

Copyright (c) 2025-2026 by crashGG.

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
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

[shader]: https://github.com/libretro/slang-shaders/blob/26bd6a692f2df44cd049b33397a5ab6b3026ee01/edge-smoothing/scalenx/shaders/mmpx-ADV.slang
[preset]: https://github.com/libretro/slang-shaders/blob/26bd6a692f2df44cd049b33397a5ab6b3026ee01/edge-smoothing/scalenx/mmpx-ADV.slangp
[paper]: https://jcgt.org/published/0010/02/04/
[supplement]: https://jcgt.org/published/0010/02/04/supplement.zip
