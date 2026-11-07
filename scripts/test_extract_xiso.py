"""Check malformed disc entries and exclusive extraction staging."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def make_image(path, name, content, size):
    sector = 2048
    image = bytearray(35 * sector)
    struct.pack_into('<20sII', image, 32 * sector, b'MICROSOFT*XBOX*MEDIA', 33, 14 + len(name))
    struct.pack_into('<HHIIBB', image, 33 * sector, 0, 0, 34, size, 0, len(name))
    name_start = 33 * sector + 14
    image[name_start:name_start + len(name)] = name
    image[34 * sector:34 * sector + len(content)] = content
    path.write_bytes(image)


def main():
    extractor = Path(__file__).with_name('extract_xiso.py').resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        iso = root / 'test.iso'
        output = root / 'output'
        output.mkdir()

        def extract():
            # Python startup and antivirus scans can take tens of seconds on a
            # loaded Windows/CI machine; the extractor itself is milliseconds.
            return subprocess.run([sys.executable, str(extractor), str(iso), str(output)],
                                  capture_output=True, text=True, timeout=120)

        make_image(iso, b'../escaped.txt', b'proof', 5)
        result = extract()
        assert result.returncode != 0 and 'unsafe name' in result.stderr, result
        assert not (root / 'escaped.txt').exists()

        make_image(iso, b'truncated.bin', b'', 2049)
        result = extract()
        assert result.returncode != 0 and 'outside the image' in result.stderr, result
        assert not list(output.iterdir())

        external = root / 'external.txt'
        external.write_bytes(b'original')
        os.link(external, output / 'victim.part')
        make_image(iso, b'victim', b'changed', 7)
        result = extract()
        assert result.returncode == 0, result
        assert external.read_bytes() == b'original'
        assert (output / 'victim').read_bytes() == b'changed'
        assert sorted(entry.name for entry in output.iterdir()) == ['victim', 'victim.part']
    print('PASS: traversal and truncated extents rejected, staging leaves existing hard links intact')


if __name__ == '__main__':
    main()
