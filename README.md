# xcheck

A read-only consistency checker for x86 xv6 filesystem images, written in C.

`xcheck` validates inode types and block addresses, directory structure, allocation
bitmaps, duplicate block references, inode references, and file link counts. It
reports the first detected error and exits without modifying the image.

## Requirements

Build and run on Linux with a C11 compiler and GNU Make. Tests require Python 3
and use only its standard library.

On this Windows machine, Ubuntu is already configured through Windows Subsystem
for Linux (WSL), and the demonstration images are available in `tests/`. No
background service, Python virtual environment, or running xv6 kernel is required.

For a fresh Ubuntu environment, install the tools with:

```bash
sudo apt update
sudo apt install -y build-essential python3 git
```

## Quick start

From Windows PowerShell, open Ubuntu:

```powershell
wsl -d Ubuntu
```

Run the remaining commands in Ubuntu. Enter the project directory:

```bash
cd /mnt/d/Keval/IITGnVistingStud/OS/xcheck
```

Clean the previous build, then compile:

```bash
make clean
make
```

Run the existing clean demonstration image:

```bash
./xcheck tests/demo-clean.img
```

A successful check is silent. Immediately inspect the exit status; expect `0`:

```bash
echo $?
```

## Usage

```bash
./xcheck <file_system_image>
```

The image must use the supported x86 xv6 format described below. Errors are
written to standard error. Exit status `0` indicates a consistent image; status
`1` indicates a consistency error, invalid input, or another execution failure.

To demonstrate error detection, run the existing corrupted image:

```bash
./xcheck tests/demo-bad-inode.img
```

Expected diagnostic:

```text
ERROR: bad inode.
```

Immediately inspect the exit status; expect `1`:

```bash
echo $?
```

## Testing

From the project directory, run the complete suite against the existing
file-containing image:

```bash
make test XV6_FS_IMAGE="$PWD/tests/demo-file.img"
```

Expected result is `Ran 39 tests` followed by `OK`, with no skipped tests.
To display individual test names and results, use:

```bash
XV6_FS_IMAGE="$PWD/tests/demo-file.img" python3 tests/test_xcheck.py -v
```

Running `make test` without `XV6_FS_IMAGE` skips the real xv6 image compatibility
test. The remaining tests generate and remove their own temporary images.

## Project structure

| Path | Purpose |
| --- | --- |
| `xcheck.c` | Checker implementation. |
| `Makefile` | Build, test, and clean targets. |
| `tests/test_xcheck.py` | Automated consistency and command-line tests. |
| `tests/demo-clean.img` | Local clean root-only image. |
| `tests/demo-file.img` | Local clean image containing a file. |
| `tests/demo-bad-inode.img` | Local image with a deliberately invalid inode type. |
| `TEST_RESULTS.md` | Recorded build and verification details. |
| `TASKS.md` | Coursework milestones. |

The demonstration images are generated locally and ignored by Git; they are
available on this machine but are not included in a fresh clone.

## Image compatibility

The supported layout is the 512-byte-block x86 format from
[mit-pdos/xv6-public](https://github.com/mit-pdos/xv6-public), pinned at commit
`eeb7b415dbcb12cc362d0783e41c3d1f44066b17`. This format uses 64-byte disk inodes,
16-byte directory entries, 12 direct block addresses, and one indirect address.
Images from other xv6 variants, including xv6 RISC-V, are outside this format contract.

The local image generator was built from that revision at
`/tmp/os-xv6-public/mkfs`. If it is still available, regenerate the root-only
image from the project directory with:

```bash
/tmp/os-xv6-public/mkfs tests/demo-clean.img
```

The `/tmp` checkout may be removed by the operating system; the existing demo
images in the project do not depend on that checkout to run.

## References

Required checks and diagnostic messages follow the
[OSTEP filesystem-checker assignment](https://github.com/remzi-arpacidusseau/ostep-projects/tree/master/filesystems-checker).
The pinned xv6 source defines the image format and provides `mkfs`.
These sources serve as requirements and format references; no existing checker
implementation was copied.
