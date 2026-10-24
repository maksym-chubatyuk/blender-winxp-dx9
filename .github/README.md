Blender for Windows XP
======================

An unofficial port of [Blender](https://www.blender.org) 5.2.2 LTS to 32-bit Windows XP and
Direct3D 9 graphics hardware. It is not affiliated with or endorsed by the Blender Foundation, so
please don't report its problems to them.

What the port adds
------------------

- **A Direct3D 9 GPU backend** (`source/blender/gpu/d3d9`). The interface and the viewport run on
  Shader Model 2.0 graphics cards.
- **MECHAZYIU** (`source/blender/draw/engines/mechazyiu`), a render engine for that hardware in the
  spirit of EEVEE Legacy: deferred shading, sample accumulation, soft shadows, HDRI lighting and
  reflections, ambient occlusion, bounce light, bloom, depth of field and AgX. Final renders are
  split into tiles automatically to fit in video memory. EEVEE itself needs far newer hardware and
  is not included.
- **Cycles** on the CPU (SSE2).

Requirements
------------

- Windows XP SP3, 32-bit, with [One-Core-API](https://github.com/Skulltrail192/One-Core-API-Binaries)
  installed. Plain XP lacks Windows functions that Blender and Python 3.13 use, so the program
  doesn't start without it.
- Visual C++ 2015-2019 Redistributable (x86), for the Universal C Runtime (tested with 14.22).
- A CPU with SSE2 (Pentium 4 or newer).
- A Direct3D 9 GPU with Shader Model 2.0 and floating point render targets (Radeon 9600 or newer).

Tested on a Pentium 4 2.8 GHz with 1 GB of RAM and a Radeon 9600 XT, where MECHAZYIU renders a
960x540 frame with 64 samples in about 19 seconds.

Building
--------

Cross-compiled on Linux with MinGW-w64 GCC (i686, UCRT), using the scripts in `build_files/xp`:

```sh
build_files/xp/deps.sh       # libraries, static
build_files/xp/python.sh     # CPython 3.13
build_files/xp/configure.sh  # CMake, into build_xp/blender
ninja -C build_xp/blender
build_files/xp/stage.sh      # installs and strips into build_xp/stage/blender
```

The HLSL shaders are compiled during the build by `d3d9_shader_compiler.exe` under Wine, with
Microsoft's `d3dcompiler_43.dll` installed (`winetricks d3dcompiler_43`) in the Wine prefix that
`build_files/xp/wine.sh` uses.

Large binary files (assets, test data) stay on Blender's own Git LFS server; `.lfsconfig` points
Git LFS there, so a normal clone works.

License
-------

GPL, like Blender. See [blender.org/about/license](https://www.blender.org/about/license) for
details. Upstream Blender's README is in [`README.md`](../README.md).
