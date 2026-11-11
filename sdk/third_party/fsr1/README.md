# AMD FidelityFX Super Resolution 1

[FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) by Advanced Micro Devices, MIT licence
(`LICENSE.txt`). Files from commit `a21ffb8f6c13233ba336352bdff293894c706575`, unmodified: `ffx-fsr/ffx_a.h` and
`ffx-fsr/ffx_fsr1.h` (here without the folder). `FsrShader.h` holds both as raw strings for
`runtime/gpu/post_fx.h`, which compiles EASU (upscaling) and RCAS (sharpening) as full-screen pixel shaders on
both renderers.
