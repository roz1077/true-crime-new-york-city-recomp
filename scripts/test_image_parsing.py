"""Exercise the runtime disc and XBE parsers with malformed input."""
from pathlib import Path
import struct
import tempfile

from native_test import run_native


def main():
    root = Path(__file__).resolve().parents[1] / 'sdk/runtime'
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        sector = 2048
        image = bytearray(35 * sector)
        volume = 32 * sector
        magic = b'MICROSOFT*XBOX*MEDIA'
        struct.pack_into('<20sII', image, volume, magic, 33, 25)
        image[volume + 0x7EC:volume + 0x7EC + len(magic)] = magic
        struct.pack_into('<HHIIBB11s', image, 33 * sector, 0, 0, 34, 4, 0, 11, b'default.xbe')
        image[34 * sector:34 * sector + 4] = b'XBEH'
        (temp / 'valid.iso').write_bytes(image)
        struct.pack_into('<I', image, 33 * sector + 8, 0xFFFFFFFF)
        (temp / 'oversized.iso').write_bytes(image)
        struct.pack_into('<IIB', image, 33 * sector + 4, 33, 25, 0x10)
        (temp / 'cycle.iso').write_bytes(image)

        source = f'''
#include "{(root / 'kernel/disc.cpp').as_posix()}"
#include "{(root / 'xbe/xbe.h').as_posix()}"
extern "C" __declspec(dllexport) int run() {{
    std::filesystem::path temp = LR"({temp})";
    kernel::Disc valid, oversized, cycle;
    std::string error;
    std::vector<uint8_t> data;
    if (!valid.Open(temp / "valid.iso", error) || !valid.Load("default.xbe", data) || data.size() != 4)
        return 1;
    if (oversized.Open(temp / "oversized.iso", error)) return 2;
    if (cycle.Open(temp / "cycle.iso", error)) return 3;

    std::vector<uint8_t> file(0x200);
    std::memcpy(file.data(), "XBEH", 4);
    auto put = [&](size_t offset, uint32_t value) {{ std::memcpy(file.data() + offset, &value, 4); }};
    put(0x104, 0x10000);
    put(0x108, 0x200);
    put(0x10C, 0x1000);
    put(0x128, 0x10180 ^ xbe::kRetailEntryKey);
    put(0x158, 0x10190 ^ xbe::kRetailThunkKey);
    xbe::Image parsed;
    if (!parsed.Parse(file, error)) return 4;
    put(0x12C, 0x101F0);
    if (parsed.Parse(file, error) || error != "TLS record is outside the image") return 5;
    return 0;
}}
'''
        result = run_native(source, prefix='tcnyc-image-', needs_crt=True,
                            compiler_flags=('-I', str(root)))
        assert result == 0, f'runtime image parser check {result} failed'
    print('PASS: disc parser rejects oversized extents and cycles, XBE parser rejects a truncated TLS record')


if __name__ == '__main__':
    main()
