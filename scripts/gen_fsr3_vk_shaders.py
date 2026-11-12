"""Generate the FSR 3.1 Vulkan shader permutation headers for the Linux build.

AMD's FidelityFX SDK 1.1.4 builds these with its Windows-only FidelityFX_SC tool and glslang. The
headers hold SPIR-V and binding tables and are platform neutral, so they are generated here once on
Windows and committed under sdk/third_party/fsr3_vk/generated.

    python scripts/gen_fsr3_vk_shaders.py <sdk-1.1.4 checkout> <dir with FidelityFX_SC.exe and glslangValidator.exe>
"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "sdk" / "third_party" / "fsr3_vk" / "generated"

BASE = [
    "-reflection", "-deps=gcc", "-DFFX_GPU=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "-compiler=glslang", "-e", "CS", "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-DFFX_GLSL=1",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}",
]
# (name suffix, extra args): GLSL has no wave size or 16-bit type switches, only FFX_HALF
VARIANTS = [("", ["-DFFX_HALF=0"]), ("_wave64", ["-DFFX_HALF=0"]),
            ("_16bit", ["-DFFX_HALF=1"]), ("_wave64_16bit", ["-DFFX_HALF=1"])]


def main():
    sdk = Path(sys.argv[1]).resolve() / "sdk"
    tools = Path(sys.argv[2]).resolve()
    gpu = sdk / "include" / "FidelityFX" / "gpu"
    shaders = sorted((sdk / "src" / "backends" / "vk" / "shaders" / "fsr3upscaler").glob("*.glsl"))
    OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
    for shader in shaders:
        for suffix, extra in VARIANTS:
            cmd = [str(tools / "FidelityFX_SC.exe"), *BASE, f"-name={shader.stem}{suffix}", *extra,
                   f"-I{gpu}", f"-I{gpu / 'fsr3upscaler'}", f"-output={OUT}", str(shader)]
            r = subprocess.run(cmd, cwd=sdk, env=env, capture_output=True, text=True)
            if r.returncode != 0 or not (OUT / f"{shader.stem}{suffix}_permutations.h").exists():
                sys.exit(f"{shader.name}{suffix} failed:\n{r.stdout}{r.stderr}")
            print(f"{shader.stem}{suffix}: ok")
    for f in OUT.glob("*.d"):
        f.unlink()
    for d in OUT.glob("*_temp"):
        if d.is_dir():
            for f in d.iterdir():
                f.unlink()
            d.rmdir()


if __name__ == "__main__":
    main()
