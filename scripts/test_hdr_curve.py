"""Check the HDR highlight curve and that both shader copies contain it."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
D3D12 = ROOT / "sdk/runtime/gpu/d3d12.cpp"
VULKAN = ROOT / "platform/linux/backend_vk.cpp"

KNEE = 0.75


def hdr_highlights(luminance, peak):
    l2 = luminance
    if luminance > KNEE:
        t = (luminance - KNEE) / (1.0 - KNEE)
        l2 = luminance + t * t * (peak - 1.0)
    return l2


def normalized_function(path):
    text = path.read_text(encoding="utf-8")
    match = re.search(r"float3 HdrHighlights\(float3 lin.*?\n}\n", text, re.S)
    assert match, f"HdrHighlights not found in {path}"
    # Drop comments and collapse whitespace so the two copies can be compared.
    body = re.sub(r"//[^\n]*", "", match.group(0))
    return re.sub(r"\s+", " ", body).strip()


def main():
    # The curve is continuous at the knee and meets it with slope 1.
    for peak in (1.0, 1.5, 2.0, 2.275, 4.0):
        assert abs(hdr_highlights(KNEE, peak) - KNEE) < 1e-12
        eps = 1e-7
        slope = (hdr_highlights(KNEE + eps, peak) - hdr_highlights(KNEE, peak)) / eps
        assert abs(slope - 1.0) < 1e-4, (peak, slope)
        assert abs(hdr_highlights(1.0, peak) - peak) < 1e-12, (peak, hdr_highlights(1.0, peak))

    # 0% highlights is the identity; the curve never decreases for P >= 1.
    samples = [i / 2000.0 for i in range(2001)]
    for peak in (1.0, 1.5, 2.0, 2.275, 4.0):
        previous = -1.0
        for value in samples:
            out = hdr_highlights(value, peak)
            if peak == 1.0:
                assert abs(out - value) < 1e-12
            assert out >= previous - 1e-12, (peak, value, out, previous)
            assert out >= value - 1e-12
            previous = out

    d3d12 = normalized_function(D3D12)
    vulkan = normalized_function(VULKAN)
    assert d3d12 == vulkan, "the two HdrHighlights bodies differ"
    assert "const float knee = 0.75;" in d3d12
    assert "l2 = l + t * t * (P - 1.0);" in d3d12

    print("PASS: HDR curve is continuous, slope 1 at the knee, P at white, identity at 0% and "
          "monotonic; both backend shaders match")


if __name__ == "__main__":
    main()
