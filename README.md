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

## End-to-end runtime demonstration

The commands below use Windows PowerShell to enter Ubuntu through Windows
Subsystem for Linux (WSL), then use Bash for compilation and execution. The
Windows project directory is `D:\Keval\IITGnVistingStud\OS\xcheck`; Ubuntu sees
the same files at `/mnt/d/Keval/IITGnVistingStud/OS/xcheck`. This program uses
Linux/POSIX facilities, so run the Makefile inside Ubuntu. No Python virtual
environment or pip packages are needed. QEMU and a running xv6 kernel are also
unnecessary: the checker reads a filesystem image directly.

### Enter and verify the environment

Run these commands in **Windows PowerShell**. Ubuntu is already installed on
the demonstrated machine.

```powershell
wsl --list --verbose
wsl -d Ubuntu
```

On a new Windows machine without Ubuntu, run `wsl --install -d Ubuntu` from
an administrator PowerShell first, restart if prompted, and finish Ubuntu's
initial user setup. This installation step was not repeated during the demo.

Run all remaining commands in **Ubuntu Bash**, including a VS Code terminal
opened using WSL. Verify the tools before building.

```bash
cd /mnt/d/Keval/IITGnVistingStud/OS/xcheck
pwd
cat /etc/os-release
gcc --version
make --version
python3 --version
git --version
```

If tools are missing on a fresh Ubuntu installation, install them using the
following commands. The existing machine already had these tools, so package
installation was not repeated.

```bash
sudo apt update
sudo apt install -y build-essential python3 git
```

### Obtain the matching image generator

`mkfs` means “make filesystem”: this xv6 program creates a binary file containing
the superblock, inodes, directories, bitmap, and file data that `xcheck` reads.
Use the pinned x86 xv6 revision below, rather than an xv6 RISC-V image. The
existing checkout at `/tmp/os-xv6-public` was verified at this exact commit.
Run the clone command only if that directory does not already exist.

```bash
git clone https://github.com/mit-pdos/xv6-public.git /tmp/os-xv6-public
cd /tmp/os-xv6-public
git checkout --detach eeb7b415dbcb12cc362d0783e41c3d1f44066b17
git rev-parse HEAD
make -B mkfs
```

For the existing pinned checkout, start at `cd /tmp/os-xv6-public`, verify
`git rev-parse HEAD`, and run `make -B mkfs`. The `-B` option forces a rebuild;
the compiler command is `gcc -Werror -Wall -o mkfs mkfs.c`. The `/tmp` directory
is temporary, so repeat the clone and build if Ubuntu removes it later.

### Build the checker and generate real images

Return to the project directory before building. `make clean` removes the
previous checker executable, and `make` builds the current `xcheck.c`.

```bash
cd /mnt/d/Keval/IITGnVistingStud/OS/xcheck
make clean
make
```

The default compiler command is
`cc -std=c11 -Wall -Wextra -Werror -O2 xcheck.c -o xcheck`.
Here `cc` selects the system C compiler, C11 selects the language standard,
the warning options reject warnings, and `-O2` enables optimization.

Generate a root-only image, then an image containing a text file. Each image
contains 1,000 blocks of 512 bytes, for a total of 512,000 bytes. The input file
argument to xv6's `mkfs` must be a basename without `/`, so create the sample
in `/tmp` and run the second image-generation command from there. These commands
replace the demonstration images when repeated.

```bash
cd /mnt/d/Keval/IITGnVistingStud/OS/xcheck
/tmp/os-xv6-public/mkfs tests/demo-clean.img
printf 'Hello from the xcheck runtime demonstration.\n' > /tmp/xcheck-demo-sample.txt
cd /tmp
/tmp/os-xv6-public/mkfs /mnt/d/Keval/IITGnVistingStud/OS/xcheck/tests/demo-file.img xcheck-demo-sample.txt
cd /mnt/d/Keval/IITGnVistingStud/OS/xcheck
./xcheck tests/demo-clean.img
echo "clean image exit status: $?"
./xcheck tests/demo-file.img
echo "file-containing image exit status: $?"
```

Both checker commands should be silent, and both `echo` commands should show
status `0`. Bash's `$?` is the exit status of the immediately preceding command,
so print it immediately after the checker. The `./` means the executable in
the current directory. An `.img` here is a binary filesystem image, not a picture.

### Demonstrate an actual consistency error

Make a separate corrupted copy, preserving the clean image. Python reads the
inode-table starting block from the superblock and changes root inode 1's type
to the invalid value 99. An inode is the record describing a file or directory;
changing its type demonstrates the checker's invalid-inode diagnostic.

```bash
python3 - <<'PY'
from pathlib import Path
import struct
image = bytearray(Path('tests/demo-clean.img').read_bytes())
inodestart = struct.unpack_from('<I', image, 512 + 20)[0]
struct.pack_into('<H', image, inodestart * 512 + 64, 99)
Path('tests/demo-bad-inode.img').write_bytes(image)
PY
./xcheck tests/demo-bad-inode.img
echo "corrupt image exit status: $?"
./xcheck
echo "missing argument exit status: $?"
./xcheck tests/does-not-exist.img
echo "missing image exit status: $?"
```

The corrupted image prints `ERROR: bad inode.`; the missing argument prints
`Usage: xcheck <file_system_image>`; the missing file prints `image not found.`.
Each returns status `1`. Run these interactively without `set -e`, since that
shell option stops a script on these deliberately induced failures.

### Run the complete tests

Run these commands from the project directory. `make test` builds the checker
if necessary and runs Python's standard-library unittest suite. Supplying
`XV6_FS_IMAGE` includes the real `mkfs` compatibility test; omitting it skips
that one test. The other tests generate their own temporary images.

```bash
make test XV6_FS_IMAGE="$PWD/tests/demo-clean.img"
make test XV6_FS_IMAGE="$PWD/tests/demo-file.img"
XV6_FS_IMAGE="$PWD/tests/demo-file.img" python3 tests/test_xcheck.py -v
```

Each command should report `Ran 39 tests` and `OK`, without skipped tests.
The verbose command shows every individual test name and result.

For the additional checks used in the previous setup, rebuild with runtime
memory and undefined-behavior checks, then GCC's static analyzer. Restore the
normal build afterward. Always use `make clean` when changing compiler flags,
because Make does not automatically rebuild just because `CFLAGS` changed.

```bash
make clean test CFLAGS='-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' XV6_FS_IMAGE="$PWD/tests/demo-file.img"
make clean test CFLAGS='-std=c11 -Wall -Wextra -Werror -O2 -fanalyzer' XV6_FS_IMAGE="$PWD/tests/demo-file.img"
make clean test XV6_FS_IMAGE="$PWD/tests/demo-file.img"
```

### Live verification on 25 November 2026

The current locally edited `xcheck.c` was rebuilt in WSL Ubuntu 26.04.1 LTS
using GCC 15.2.0, GNU Make 4.4.1, Python 3.14.4, and Git 2.53.0. The pinned
`mkfs` was forcibly rebuilt and generated both demonstration images anew.
Both clean images returned 0, the corrupted image returned 1 with the expected
diagnostic, and both command-line error examples returned 1. All 39 tests passed
with each real image, in verbose mode, with both sanitizers enabled, with
`-fanalyzer`, and after restoring the normal optimized build. No tests were
skipped in those complete runs. The demonstration images remain in `tests/`
and are ignored by Git. The checker source was not changed during this run.
