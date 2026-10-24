# xv6 filesystem checker

`xcheck` reads an xv6 filesystem image and reports the first consistency error.
It does not modify the image. A clean image produces no output and returns 0;
a detected error is written to standard error and returns 1.

Build with `make`, run with `./xcheck fs.img`, and run the tests with
`make test`. The tests use only Python's standard library. To include a clean
image built by xv6's `mkfs`, run `make test XV6_FS_IMAGE=/path/to/fs.img`.

The supported image layout is the 512-byte-block x86 xv6 format in
[`mit-pdos/xv6-public`](https://github.com/mit-pdos/xv6-public), pinned at commit
`eeb7b415dbcb12cc362d0783e41c3d1f44066b17`. Its `fs.h` defines the superblock,
64-byte disk inode, 16-byte directory entry, 12 direct addresses, and one
indirect block. Empty and file-containing images from that revision's `mkfs`
pass the checker.

The required checks and exact diagnostics come from the
[OSTEP filesystem-checker assignment](https://github.com/remzi-arpacidusseau/ostep-projects/tree/master/filesystems-checker).
`xcheck` checks inode types and addresses, the root and directory entries,
bitmap allocation, duplicate block references, inode references, file link
counts, and repeated directory links. It exits on the first error.

The OSTEP assignment and the pinned xv6 source are used as requirements and
format references; no existing checker implementation was copied.
