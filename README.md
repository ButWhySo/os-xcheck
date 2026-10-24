# xv6 filesystem checker

`xcheck` reads an xv6 filesystem image and reports the first consistency error. It does not modify the image. A clean image produces no output and returns 0; a detected error is written to standard error and returns 1.

Build with `make`, run with `./xcheck fs.img`, and run the tests with `make test`. The tests use only Python's standard library.

The supported image layout is the 512-byte-block x86 xv6 format in [`mit-pdos/xv6-public`](https://github.com/mit-pdos/xv6-public), pinned for development at commit `eeb7b415dbcb12cc362d0783e41c3d1f44066b17`. Its `fs.h` defines the superblock, 64-byte disk inode, 16-byte directory entry, 12 direct addresses, and one indirect block. The test suite also checks a clean image created by that revision's `mkfs`.

The required checks and exact diagnostics come from the [OSTEP filesystem-checker assignment](https://github.com/remzi-arpacidusseau/ostep-projects/tree/master/filesystems-checker). `xcheck` checks inode types and addresses, the root and directory entries, bitmap allocation, duplicate block references, inode references, file link counts, and repeated directory links. It exits on the first error.
