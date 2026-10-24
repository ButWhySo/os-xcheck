# Project tasks

## Milestone 1 - Environment and xv6 setup

- [x] **1.1** Verify Ubuntu, GCC, Make, and Git.
- [x] **1.2** Pin xv6 source and confirm its on-disk structures.
- [x] **1.3** Build `mkfs` and create a clean image.
- [x] **1.4** Parse the image header and implement the required command-line errors.
- [x] **1.5** Run milestone checks and commit the working setup.

## Milestone 2 - Inode and root checks

- [x] **2.1** Reject inode types outside the xv6 type values.
- [x] **2.2** Validate direct and indirect block addresses.
- [x] **2.3** Check that root inode 1 exists and names itself as parent.
- [x] **2.4** Test each diagnostic and commit the checks.

## Milestone 3 - Block allocation checks

- [x] **3.1** Check inode block references against the allocation bitmap.
- [x] **3.2** Find allocated data blocks unused by any inode.
- [x] **3.3** Detect repeated direct and indirect block references.
- [x] **3.4** Test bitmap and duplicate-address errors; commit the checks.

## Milestone 4 - Directory and reference checks

- [x] **4.1** Validate `.` and `..` entries in every directory.
- [x] **4.2** Check allocated and referenced inode consistency.
- [x] **4.3** Compare regular-file link counts with directory references.
- [x] **4.4** Reject directories linked from multiple parents.
- [x] **4.5** Test each diagnostic and commit the checks.

## Milestone 5 - Complete test coverage and documentation

- [x] **5.1** Cover all required diagnostics and command-line cases.
- [x] **5.2** Check clean and file-containing images from the pinned xv6 `mkfs`.
- [x] **5.3** Record build, test, and compatibility instructions.
- [x] **5.4** Run the complete suite and commit the verified project.

## Milestone 6 - Final review

- [x] **6.1** Review the checker for correctness and student-level clarity.
- [x] **6.2** Remove unused code and fix any review findings.
- [x] **6.3** Run the complete suite after the review.
- [x] **6.4** Commit the final reviewed state.
