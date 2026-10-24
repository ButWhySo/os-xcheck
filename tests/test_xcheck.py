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
ROOT_DATA_BLOCK = DATA_START
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


def inode_offset(number):
    return INODE_START * BLOCK_SIZE + number * 64


def set_inode_type(image, number, inode_type):
    struct.pack_into("<H", image, inode_offset(number), inode_type)


def set_inode_address(image, number, slot, block):
    struct.pack_into("<I", image, inode_offset(number) + 12 + slot * 4, block)


def set_inode_size(image, number, size):
    struct.pack_into("<I", image, inode_offset(number) + 8, size)


def set_inode_links(image, number, links):
    struct.pack_into("<H", image, inode_offset(number) + 6, links)


def write_dirent(image, block, index, number, name):
    offset = block * BLOCK_SIZE + index * 16
    struct.pack_into("<H", image, offset, number)
    image[offset + 2:offset + 16] = name.ljust(14, b"\0")


def set_bitmap(image, block, allocated):
    offset = BITMAP_START * BLOCK_SIZE + block // 8
    mask = 1 << (block % 8)
    if allocated:
        image[offset] |= mask
    else:
        image[offset] &= ~mask


def add_file(image, number=2, links=1):
    set_inode_type(image, number, 2)
    set_inode_links(image, number, links)
    write_dirent(image, ROOT_DATA_BLOCK, 2, number, b"file")
    set_inode_size(image, 1, 48)


def add_child_directory(image, references=1):
    set_inode_type(image, 2, 1)
    set_inode_links(image, 2, 1)
    set_inode_size(image, 2, 32)
    set_inode_address(image, 2, 0, 61)
    write_dirent(image, 61, 0, 2, b".")
    write_dirent(image, 61, 1, 1, b"..")
    set_bitmap(image, 61, True)
    for index in range(references):
        write_dirent(image, ROOT_DATA_BLOCK, 2 + index, 2, b"sub" + str(index).encode())
    set_inode_size(image, 1, (2 + references) * 16)


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

    def test_inode_address_must_be_allocated(self):
        path = self.edited_image(
            "free-data-block.img", lambda image: set_bitmap(image, ROOT_DATA_BLOCK, False)
        )
        self.assert_error(path, "ERROR: address used by inode but marked free in bitmap.")

    def test_indirect_block_must_be_allocated(self):
        def edit(image):
            set_inode_type(image, 2, 2)
            set_inode_address(image, 2, 12, 61)

        path = self.edited_image("free-indirect-block.img", edit)
        self.assert_error(path, "ERROR: address used by inode but marked free in bitmap.")

    def test_indirect_data_block_must_be_allocated(self):
        def edit(image):
            set_inode_type(image, 2, 2)
            set_inode_address(image, 2, 12, 61)
            struct.pack_into("<I", image, 61 * BLOCK_SIZE, 62)
            set_bitmap(image, 61, True)

        path = self.edited_image("free-indirect-data.img", edit)
        self.assert_error(path, "ERROR: address used by inode but marked free in bitmap.")

    def test_allocated_data_block_must_be_used(self):
        path = self.edited_image(
            "unused-allocated-block.img", lambda image: set_bitmap(image, ROOT_DATA_BLOCK + 1, True)
        )
        self.assert_error(path, "ERROR: bitmap marks block in use but it is not in use.")

    def test_direct_address_cannot_repeat(self):
        def edit(image):
            set_inode_type(image, 2, 2)
            set_inode_address(image, 2, 0, ROOT_DATA_BLOCK)

        path = self.edited_image("duplicate-direct.img", edit)
        self.assert_error(path, "ERROR: direct address used more than once.")

    def test_indirect_addresses_cannot_repeat(self):
        def edit(image):
            set_inode_type(image, 2, 2)
            set_inode_address(image, 2, 12, 61)
            set_inode_type(image, 3, 2)
            set_inode_address(image, 3, 12, 63)
            struct.pack_into("<I", image, 61 * BLOCK_SIZE, 62)
            struct.pack_into("<I", image, 63 * BLOCK_SIZE, 62)
            for block in (61, 62, 63):
                set_bitmap(image, block, True)

        path = self.edited_image("duplicate-indirect-data.img", edit)
        self.assert_error(path, "ERROR: indirect address used more than once.")

    def test_indirect_block_cannot_repeat(self):
        def edit(image):
            for number in (2, 3):
                set_inode_type(image, number, 2)
                set_inode_address(image, number, 12, 61)
            set_bitmap(image, 61, True)

        path = self.edited_image("duplicate-indirect-block.img", edit)
        self.assert_error(path, "ERROR: indirect address used more than once.")

    def test_direct_and_indirect_addresses_cannot_alias(self):
        def edit(image):
            set_inode_type(image, 2, 2)
            set_inode_address(image, 2, 12, 61)
            struct.pack_into("<I", image, 61 * BLOCK_SIZE, ROOT_DATA_BLOCK)
            set_bitmap(image, 61, True)

        path = self.edited_image("duplicate-mixed-address.img", edit)
        self.assert_error(path, "ERROR: direct address used more than once.")

    def test_directory_must_contain_dot(self):
        path = self.edited_image(
            "missing-dot.img", lambda image: write_dirent(image, ROOT_DATA_BLOCK, 0, 1, b"x")
        )
        self.assert_error(path, "ERROR: directory not properly formatted.")

    def test_dot_must_point_to_its_directory(self):
        path = self.edited_image(
            "wrong-dot-target.img", lambda image: write_dirent(image, ROOT_DATA_BLOCK, 0, 2, b".")
        )
        self.assert_error(path, "ERROR: directory not properly formatted.")

    def test_subdirectory_must_contain_dotdot(self):
        def edit(image):
            set_inode_type(image, 2, 1)
            set_inode_size(image, 2, 16)
            set_inode_address(image, 2, 0, 61)
            write_dirent(image, 61, 0, 2, b".")
            set_bitmap(image, 61, True)
            write_dirent(image, ROOT_DATA_BLOCK, 2, 2, b"sub")
            set_inode_size(image, 1, 48)

        path = self.edited_image("missing-dotdot.img", edit)
        self.assert_error(path, "ERROR: directory not properly formatted.")

    def test_allocated_inode_must_be_referenced(self):
        path = self.edited_image("unreferenced-inode.img", lambda image: set_inode_type(image, 2, 2))
        self.assert_error(path, "ERROR: inode marked use but not found in a directory.")

    def test_directory_cannot_reference_free_inode(self):
        def edit(image):
            write_dirent(image, ROOT_DATA_BLOCK, 2, 2, b"free")
            set_inode_size(image, 1, 48)

        path = self.edited_image("reference-to-free-inode.img", edit)
        self.assert_error(path, "ERROR: inode referred to in directory but marked free.")

    def test_file_link_count_must_match_directory_entries(self):
        path = self.edited_image("bad-file-links.img", lambda image: add_file(image, links=2))
        self.assert_error(path, "ERROR: bad reference count for file.")

    def test_directory_cannot_have_multiple_parent_links(self):
        path = self.edited_image(
            "repeated-directory.img", lambda image: add_child_directory(image, references=2)
        )
        self.assert_error(path, "ERROR: directory appears more than once in file system.")

    def test_valid_file_and_subdirectory_pass(self):
        def edit(image):
            add_file(image)
            add_child_directory(image)
            write_dirent(image, ROOT_DATA_BLOCK, 3, 2, b"sub")
            write_dirent(image, ROOT_DATA_BLOCK, 2, 3, b"file")
            set_inode_type(image, 3, 2)
            set_inode_links(image, 3, 1)
            set_inode_size(image, 1, 64)

        path = self.edited_image("valid-entries.img", edit)
        result = self.run_xcheck(path)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
