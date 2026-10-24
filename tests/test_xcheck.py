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
ROOT_INODE_OFFSET = INODE_START * BLOCK_SIZE + 64


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

    def edited_image(self, name, edit):
        with open(self.image_path, "rb") as source:
            image = bytearray(source.read())
        edit(image)
        path = os.path.join(self.temp_dir.name, name)
        with open(path, "wb") as output:
            output.write(image)
        return path

    def assert_error(self, path, message):
        result = self.run_xcheck(path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, message + "\n")

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
        self.assert_error(bad_path, "ERROR: invalid file system image.")

    def test_bad_inode_type(self):
        path = self.edited_image(
            "bad-inode.img", lambda image: struct.pack_into("<H", image, ROOT_INODE_OFFSET, 4)
        )
        self.assert_error(path, "ERROR: bad inode.")

    def test_bad_direct_address(self):
        path = self.edited_image(
            "bad-direct.img",
            lambda image: struct.pack_into("<I", image, ROOT_INODE_OFFSET + 12, IMAGE_BLOCKS),
        )
        self.assert_error(path, "ERROR: bad direct address in inode.")

    def test_direct_address_to_metadata_is_invalid(self):
        path = self.edited_image(
            "metadata-direct.img",
            lambda image: struct.pack_into("<I", image, ROOT_INODE_OFFSET + 12, BITMAP_START),
        )
        self.assert_error(path, "ERROR: bad direct address in inode.")

    def test_bad_indirect_block_address(self):
        path = self.edited_image(
            "bad-indirect-block.img",
            lambda image: struct.pack_into("<I", image, ROOT_INODE_OFFSET + 12 + 12 * 4, IMAGE_BLOCKS),
        )
        self.assert_error(path, "ERROR: bad indirect address in inode.")

    def test_bad_address_in_indirect_block(self):
        def edit(image):
            struct.pack_into("<I", image, ROOT_INODE_OFFSET + 12 + 12 * 4, 61)
            struct.pack_into("<I", image, 61 * BLOCK_SIZE, IMAGE_BLOCKS)

        path = self.edited_image("bad-indirect-data.img", edit)
        self.assert_error(path, "ERROR: bad indirect address in inode.")

    def test_missing_root_inode(self):
        path = self.edited_image(
            "missing-root.img", lambda image: struct.pack_into("<H", image, ROOT_INODE_OFFSET, 0)
        )
        self.assert_error(path, "ERROR: root directory does not exist.")

    def test_root_parent_must_be_root(self):
        path = self.edited_image(
            "bad-root-parent.img",
            lambda image: struct.pack_into("<H", image, ROOT_DATA_BLOCK * BLOCK_SIZE + 16, 2),
        )
        self.assert_error(path, "ERROR: root directory does not exist.")


if __name__ == "__main__":
    unittest.main()
