# Third-Party Notices

绘声Studio 是 [wannengya-audio](https://github.com/shangcheng8/wannengya-audio) 的修改版，
自身以 **AGPL-3.0** 发布，全文见 [LICENSE](LICENSE)。

本文件列出随二进制一起分发的第三方组件及其许可条件。源码构建时，JUCE 及其内嵌的
组件由 CMake 在配置阶段取得，完整许可原文也都在 `third_party/JUCE` 源码树内，
括号里给出路径。

## 1. JUCE 8.0.15

- 许可：AGPLv3，或 [JUCE 商业许可](https://juce.com/legal/juce-8-licence) 二选一
- 本项目选择 **AGPLv3** 选项，因此本仓库整体以 AGPL-3.0 发布
- 上游：https://github.com/juce-framework/JUCE
- 原文：`third_party/JUCE/LICENSE.md`

JUCE 自身还聚合了下列组件。AGPL 选项下它们各自的许可仍然有效。

## 2. 编译进 macOS 二进制的组件

| 组件 | 版本 / 说明 | 许可 | 版权 | 源码树路径 |
|---|---|---|---|---|
| Steinberg VST3 SDK | 与 JUCE 8.0.15 同步 | MIT | Copyright (c) 2025, Steinberg Media Technologies GmbH | `modules/juce_audio_processors_headless/format_types/VST3_SDK/` |
| zlib | 1.2.3 | zlib License | Copyright (C) 1995-2010 Jean-loup Gailly and Mark Adler | `modules/juce_core/zip/zlib/` |
| libpng | PNG Reference Library License v2 | libpng License | Copyright (c) 1995-2022 the PNG Reference Library Authors | `modules/juce_graphics/image_formats/pnglib/` |
| libjpeg | IJG release 9f | IJG License | Copyright (C) 1991-2024, Thomas G. Lane, Guido Vollbeding | `modules/juce_graphics/image_formats/jpglib/` |
| HarfBuzz | — | Old MIT | Copyright © 2010-2022 Google, Inc. | `modules/juce_graphics/fonts/harfbuzz/` |
| SheenBidi | — | Apache-2.0 | Copyright 2015-2020 TAHOE | `modules/juce_graphics/unicode/sheenbidi/` |
| GLEW / Mesa / Khronos 头文件 | 用于 OpenGL 兼容层 | BSD / MIT | Milan Ikits, Marcelo Magallon 等 | `modules/juce_graphics/opengl/juce_gl.h` |
| LV2 SDK | 仅头文件 | ISC | Copyright 2006-2012 Steve Harris, David Robillard | `modules/juce_audio_processors_headless/format_types/LV2_SDK/` |
| pslextensions | 仅头文件 | Public domain | — | `modules/juce_audio_processors_headless/format_types/pslextensions/` |

## 3. 在 JUCE 源码树内、但未编入 macOS 二进制

这些组件只在别的平台或别的插件格式下才会被编译，不属于本项目的分发包内容，
列出以免误解：

- **FLAC、Ogg Vorbis**（BSD）— 属 `juce_audio_formats`，本项目未链接该模块
- **Oboe**（Apache-2.0）— 仅 Android
- **Steinberg ASIO SDK**（Steinberg ASIO License 或 GPLv3）— 仅 Windows
- **Avid AAX SDK**（Avid 专有许可或 GPLv3）— 仅构建 AAX 时
- **CHOC / QuickJS**、**Box2D** — 属未链接的模块

## 4. 许可全文

### MIT（Steinberg VST3 SDK）

```
MIT License

Copyright (c) 2025, Steinberg Media Technologies GmbH

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### zlib License

```
Copyright (C) 1995-2010 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### ISC（LV2 SDK 头文件）

```
Copyright 2006-2012 Steve Harris, David Robillard.
Based on LADSPA, Copyright 2000-2002 Richard W.E. Furse, Paul Barton-Davis,
Stefan Westerfeld.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```

Apache-2.0（SheenBidi）、BSD（GLEW / Mesa）、PNG Reference Library License v2
（libpng）与 IJG License（libjpeg）的全文较长，随 JUCE 源码树提供，路径见上表，
也可在 https://www.apache.org/licenses/LICENSE-2.0 、
`third_party/JUCE/modules/juce_graphics/image_formats/pnglib/LICENSE` 等处查阅。

## 5. 商标

VST is a trademark of Steinberg Media Technologies GmbH。
本项目与 Steinberg Media Technologies GmbH 无隶属关系，也未获其背书。

「万能鸭」「wannengya」是上游作者的名称与标识，本修改版已按上游要求把名称、图标与
bundle identifier 全部更换。本版本使用的「绘声Studio」名称、图标与 `com.huishengstudio.*`
bundle identifier 同样不在 AGPL-3.0 的授权范围内，详见 [README.md](README.md) 的
「商标与来源」一节。
