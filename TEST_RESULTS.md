# Verified test results

The checker was built and tested in Ubuntu 26.04.1 LTS with GCC 15.2.0 and GNU Make 4.4.1. The xv6 image format and `mkfs` were pinned to `mit-pdos/xv6-public` commit `eeb7b415dbcb12cc362d0783e41c3d1f44066b17`.

The 39-test suite passed with a clean root-only image and with an image containing a file created by the pinned `mkfs`. The tests include every required diagnostic, command-line errors, malformed images, direct and indirect block cases, directory entries, inode references, file link counts, and valid images that use indirect blocks.

The same 39 tests passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled. They also passed with GCC's `-fanalyzer` enabled. The final standard build used `-std=c11 -Wall -Wextra -Werror -O2` and passed all 39 tests after review.

The test commands, run from the project directory, were:

```sh
make clean test XV6_FS_IMAGE=/tmp/os-xv6-public/fs.img
make test XV6_FS_IMAGE=/tmp/os-xv6-public/fs-nonempty.img
make clean test CFLAGS='-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' XV6_FS_IMAGE=/tmp/os-xv6-public/fs-nonempty.img
make clean test CFLAGS='-std=c11 -Wall -Wextra -Werror -O2 -fanalyzer' XV6_FS_IMAGE=/tmp/os-xv6-public/fs-nonempty.img
```
