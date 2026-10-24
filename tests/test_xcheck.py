import os
import struct
import subprocess
import tempfile
import unittest


BLOCK_SIZE = 512
IMAGE_BLOCKS = 1000
IMAGE_BYTES = IMAGE_BLOCKS * BLOCK_SIZE
INODE_COUNT = 200
INODE_START = 32
BITMAP_START = 58
DATA_START = 59
ROOT_DATA_BLOCK = 60


def make_clean_image(path):
    image = bytearray(IMAGE_BYTES)
    struct.pack_into(
        "<7I", image, BLOCK_SIZE,
        IMAGE_BLOCKS, IMAGE_BLOCKS - DATA_START, INODE_COUNT, 30, 2,
        INODE_START, BITMAP_START,
    )

    inode_offset = INODE_START * BLOCK_SIZE + 64
    struct.pack_into("<HHHHI", image, inode_offset, 1, 0, 0, 1, 32)
    struct.pack_into("<13I", image, inode_offset + 12, ROOT_DATA_BLOCK, *([0] * 12))

    directory_offset = ROOT_DATA_BLOCK * BLOCK_SIZE
    for entry, name in enumerate((b".", b"..")):
        struct.pack_into("<H", image, directory_offset + entry * 16, 1)
        image[directory_offset + entry * 16 + 2:directory_offset + entry * 16 + 2 + len(name)] = name

    for block in range(ROOT_DATA_BLOCK + 1):
        image[BITMAP_START * BLOCK_SIZE + block // 8] |= 1 << (block % 8)

    with open(path, "wb") as output:
        output.write(image)


class XcheckTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.image_path = os.path.join(self.temp_dir.name, "clean.img")
        make_clean_image(self.image_path)

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_xcheck(self, *args):
        return subprocess.run(
            ["./xcheck", *args], capture_output=True, text=True, check=False
        )

    def test_clean_image_is_silent(self):
        result = self.run_xcheck(self.image_path)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")

    def test_missing_argument(self):
        result = self.run_xcheck()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, "Usage: xcheck <file_system_image>\n")

    def test_missing_image(self):
        result = self.run_xcheck(os.path.join(self.temp_dir.name, "missing.img"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, "image not found.\n")

    def test_invalid_image(self):
        bad_path = os.path.join(self.temp_dir.name, "invalid.img")
        with open(bad_path, "wb") as output:
            output.write(b"not an xv6 image")
        result = self.run_xcheck(bad_path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, "ERROR: invalid file system image.\n")


if __name__ == "__main__":
    unittest.main()
