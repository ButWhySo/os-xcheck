/**
 * @file xcheck.c
 * @brief xcheck: a read-only consistency checker for xv6 file system images.
 *
 * Usage: xcheck <file_system_image>
 *
 * The image is mapped into memory as one byte array and checked in three
 * phases. Each check returns 0 when it passes. On failure it prints exactly one
 * error message to stderr and returns -1, and main() then exits with status 1.
 * The checks are chained with || in main(), so the first failure stops the run.
 *
 * Layout of the reference image (1000 blocks of 512 bytes = 512,000 bytes):
 *
 *   block 0        boot block
 *   block 1        superblock (7 little-endian uint32 values, byte 512 to 539)
 *   blocks 2-31    log (30 blocks)
 *   blocks 32-57   inode table (200 inodes, 8 per block; xcheck needs 25 blocks)
 *   block 58       bitmap (1 bit for every block of the disk, blocks 0 to 999)
 *   blocks 59-999  data region (941 blocks)
 *
 * Phase 1, checks 1-4, make the image safe to walk:
 *   1. inode types valid           2. inode addresses inside the data region
 *   3. root directory exists       4. directories are well formed
 * Phase 2, checks 5-8, compare the bitmap with the blocks inodes really use:
 *   5. used block marked free      6. block marked used but nobody uses it
 *   7. direct address repeated     8. indirect address repeated
 * Phase 3, checks 9-12, compare directory entries with inodes:
 *   9. inode in use but unnamed    10. name points to a free inode
 *   11. file link count wrong      12. directory named more than once
 */

/** Make off_t 64 bits wide so st_size is correct for images larger than 2 GiB. */
#define _FILE_OFFSET_BITS 64

#include <errno.h>     // errno, ENOENT, ENOTDIR: why open() failed
#include <fcntl.h>     // open(), O_RDONLY
#include <stdint.h>    // uint8_t, uint16_t, uint32_t, uint64_t: fixed-width integers
#include <stdio.h>     // fprintf(), perror()
#include <stdlib.h>    // calloc(), free()
#include <string.h>    // memcmp()
#include <sys/mman.h>  // mmap(), munmap()
#include <sys/stat.h>  // fstat(), struct stat
#include <unistd.h>    // close()

/** Size of one disk block in bytes (the xv6 block size). */
#define BLOCK_SIZE 512

/** Bits in one block: 512 bytes x 8 = 4096. One bitmap block describes 4096 disk blocks. */
#define BLOCK_BITS (BLOCK_SIZE * 8)

/** Size of one on-disk inode record in bytes. */
#define INODE_SIZE 64

/** Inodes stored in one block: 512 / 64 = 8. Inode n lives in block inode_start + n / 8. */
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)

/** Size of one directory entry: 2 bytes of inode number followed by a 14 byte name field. */
#define DIRECTORY_ENTRY_SIZE 16

/** Number of direct block addresses stored inside an inode. */
#define DIRECT_COUNT 12

/** Block numbers held by one indirect block: 512 / 4 = 128 (each number is a uint32_t). */
#define INDIRECT_COUNT (BLOCK_SIZE / sizeof(uint32_t))

/** Most blocks one file or directory can have: 12 direct + 128 indirect = 140 (71,680 bytes). */
#define MAX_FILE_BLOCKS (DIRECT_COUNT + INDIRECT_COUNT)

/** Byte offset of the superblock inside the image: it is block 1, so 1 x 512 = 512. */
#define SUPERBLOCK_OFFSET BLOCK_SIZE

/** Inode type value for a directory. A type of 0 means the inode is free. */
#define TYPE_DIRECTORY 1

/** Inode type value for a regular file. */
#define TYPE_FILE 2

/** Inode type value for a device file. */
#define TYPE_DEVICE 3

/** Printed by load_image() when the image or its superblock is not believable. */
#define INVALID_IMAGE "ERROR: invalid file system image."
/** Printed by check 1: an inode has a type other than 0, 1, 2 or 3. */
#define BAD_INODE "ERROR: bad inode."
/** Printed by check 2: a direct address is outside the data region. */
#define BAD_DIRECT_ADDRESS "ERROR: bad direct address in inode."
/** Printed by check 2: the indirect address, or a number inside the indirect block, is outside the data region. */
#define BAD_INDIRECT_ADDRESS "ERROR: bad indirect address in inode."
/** Printed by check 3: inode 1 is not a directory, or it has no entry ".." pointing to itself. */
#define ROOT_MISSING "ERROR: root directory does not exist."
/** Printed by check 4: a directory has a bad size, a missing block, or wrong "." and ".." entries. */
#define BAD_DIRECTORY "ERROR: directory not properly formatted."
/** Printed by check 9: an inode is in use but no directory entry names it. */
#define UNREFERENCED_INODE "ERROR: inode marked use but not found in a directory."
/** Printed by check 10: a directory entry names an inode that is out of range or free. */
#define FREE_INODE_REFERENCE "ERROR: inode referred to in directory but marked free."
/** Printed by check 11: a file's link count differs from the number of directory entries naming it. */
#define BAD_FILE_LINKS "ERROR: bad reference count for file."
/** Printed by check 12: a directory is named by more than one entry (besides "." and ".."). */
#define REPEATED_DIRECTORY "ERROR: directory appears more than once in file system."
/** Printed by check 5: an inode uses a block whose bitmap bit is 0. */
#define FREE_BLOCK_REFERENCE "ERROR: address used by inode but marked free in bitmap."
/** Printed by check 6: a bitmap bit is 1 but no inode uses that data block. */
#define UNUSED_BITMAP_BLOCK "ERROR: bitmap marks block in use but it is not in use."
/** Printed by check 7: a block is a direct address more than once, or both direct and indirect. */
#define REPEATED_DIRECT_ADDRESS "ERROR: direct address used more than once."
/** Printed by check 8: a block is listed more than once through indirect blocks. */
#define REPEATED_INDIRECT_ADDRESS "ERROR: indirect address used more than once."

/**
 * @brief The superblock: seven numbers that describe the layout of the image.
 *
 * On disk they are seven consecutive little-endian uint32 values starting at
 * byte 512, in the order declared below. load_image() copies them here.
 * Example values for the reference image: 1000, 941, 200, 30, 2, 32, 58.
 */
struct superblock {
    /** Total number of blocks in the image (1000). Must equal file length / 512. */
    uint32_t size;
    /** Number of blocks in the data region (941). The data region ends at block size - 1. */
    uint32_t data_blocks;
    /** Number of inodes (200). Inode 0 is never used and inode 1 is the root directory. */
    uint32_t inode_count;
    /** Number of log blocks (30). 0 means the image has no log. */
    uint32_t log_count;
    /** First block of the log (2). */
    uint32_t log_start;
    /** First block of the inode table (32). */
    uint32_t inode_start;
    /** First block of the bitmap (58). */
    uint32_t bitmap_start;
};

/**
 * @brief Handle for the whole image: the mapped file plus its superblock numbers.
 *
 * main() creates one of these, load_image() fills it, and every check receives
 * it as a const pointer. The image is never modified (it is mapped read-only).
 */
struct image {
    /** File descriptor (a small number the OS gives for the open file), or -1 when not open. */
    int fd;
    /** Address of byte 0 of the image in memory (from mmap), or NULL when not mapped. data[512] is the first superblock byte. */
    uint8_t *data;
    /** Length of the image in bytes (512000 for the reference image). */
    size_t length;
    /** Copy of the seven superblock numbers. */
    struct superblock sb;
    /** First data block: size - data_blocks = 1000 - 941 = 59. A valid data block b satisfies data_start <= b < size. */
    uint32_t data_start;
};

/**
 * @brief Block accounting built from the inodes (used by checks 6, 7 and 8).
 *
 * Each array has sb.size entries (1000 bytes) and is indexed directly by block
 * number. Entries for blocks below data_start always stay 0.
 */
struct block_state {
    /** used[b] is 1 when some inode refers to block b (as data or as an indirect block). */
    uint8_t *used;
    /** direct_refs[b] counts how often b appears as a direct address. It stops counting at 2. */
    uint8_t *direct_refs;
    /** indirect_refs[b] counts how often b is an indirect block or is listed inside one. It stops counting at 2. */
    uint8_t *indirect_refs;
};

/**
 * @brief Name accounting built from the directory entries (used by checks 9, 11 and 12).
 *
 * Each array has sb.inode_count entries (200 x 4 = 800 bytes) and is indexed by inode number.
 */
struct directory_state {
    /** inode_refs[n] = number of directory entries whose inode number is n, including "." and "..". */
    uint32_t *inode_refs;
    /** directory_refs[n] = number of entries, other than "." and "..", that name directory inode n. */
    uint32_t *directory_refs;
};

/**
 * @brief In-memory copy of the fields xcheck needs from a 64 byte on-disk inode.
 *
 * Disk layout: type at byte 0 (2), major at 2 (skipped), minor at 4 (skipped),
 * links at 6 (2), size at 8 (4), addresses at 12 (13 x 4 = 52 bytes).
 */
struct inode {
    /** 0 free, 1 directory, 2 file, 3 device. */
    uint16_t type;
    /** Link count: how many directory entries are supposed to name this inode. */
    uint16_t links;
    /** Size of the content in bytes. For a directory: number of entries x 16. */
    uint32_t size;
    /**
     * Block numbers. [0] to [11] are direct addresses of the first 12 blocks of
     * the content. [12] is the indirect address: a block holding 128 more block
     * numbers. A value of 0 means "no block".
     */
    uint32_t addresses[DIRECT_COUNT + 1];
};

/**
 * @brief Read a 2 byte little-endian unsigned number.
 *
 * Little-endian means the low byte comes first: bytes 03 00 give 3.
 * Formula: p[0] + p[1] x 2^8.
 *
 * @param p Address of the first of the 2 bytes. The caller must make sure both bytes are inside the image.
 * @return The 16 bit value.
 */
static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;  // low byte | high byte shifted up by 8 bits
}

/**
 * @brief Read a 4 byte little-endian unsigned number.
 *
 * Formula: p[0] + p[1] x 2^8 + p[2] x 2^16 + p[3] x 2^24.
 * Example: bytes ad 03 00 00 give 173 + 3 x 256 = 941.
 *
 * @param p Address of the first of the 4 bytes. The caller must make sure all 4 bytes are inside the image.
 * @return The 32 bit value.
 */
static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0]          // byte 0 is the lowest 8 bits
        | (uint32_t)p[1] << 8      // byte 1 moves up by 8 bits
        | (uint32_t)p[2] << 16     // byte 2 moves up by 16 bits
        | (uint32_t)p[3] << 24;    // byte 3 moves up by 24 bits
}

/**
 * @brief Load inode number `number` from the inode table into a struct inode.
 *
 * Address of the inode record: inode_start x 512 + number x 64. Inode 3 of the
 * reference image is at 32 x 512 + 3 x 64 = 16,576. major and minor are skipped.
 *
 * @param image  The loaded image.
 * @param number Inode number, which must be below sb.inode_count.
 * @return A copy of the inode. Changing it does not change the image.
 */
static struct inode read_inode(const struct image *image, uint32_t number)
{
    struct inode inode;  // the copy that will be returned
    size_t offset = (size_t)image->sb.inode_start * BLOCK_SIZE  // start of the inode table in bytes
        + (size_t)number * INODE_SIZE;                          // plus this inode's slot
    const uint8_t *bytes = image->data + offset;                // address of the 64 byte record

    inode.type = read_u16(bytes);                // bytes 0-1: type
    inode.links = read_u16(bytes + 6);           // bytes 6-7: link count (bytes 2-5 are major and minor)
    inode.size = read_u32(bytes + 8);            // bytes 8-11: size in bytes
    for (size_t i = 0; i < DIRECT_COUNT + 1; i++)                        // 13 addresses: 12 direct + 1 indirect
        inode.addresses[i] = read_u32(bytes + 12 + i * sizeof(uint32_t)); // starting at byte 12, 4 bytes each
    return inode;                                // hand the copy back
}

/**
 * @brief Test whether a block number lies inside the data region.
 *
 * True when data_start <= block < size, which is 59 <= block < 1000 for the
 * reference image. Address 0 ("no block") is not valid; callers test for 0 first.
 *
 * @param image The loaded image.
 * @param block Block number to test.
 * @return 1 when the block is a valid data block, 0 otherwise.
 */
static int valid_data_block(const struct image *image, uint32_t block)
{
    return block >= image->data_start && block < image->sb.size;  // not below the data region, not past the end
}

/**
 * @brief Check 1: every inode has a valid type.
 *
 * Valid types are 0 (free), 1 (directory), 2 (file) and 3 (device).
 * Prints BAD_INODE on failure.
 *
 * @param image The loaded image.
 * @return 0 when all inodes pass, -1 on the first bad inode.
 */
static int check_inode_types(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode, 0 to 199
        uint16_t type = read_inode(image, number).type;                    // only the type field is needed
        if (type != 0 && type != TYPE_DIRECTORY && type != TYPE_FILE      // none of 0, 1, 2 ...
            && type != TYPE_DEVICE) {                                      // ... or 3
            fprintf(stderr, "%s\n", BAD_INODE);                            // report the problem
            return -1;                                                     // stop at the first failure
        }
    }
    return 0;  // every type was valid
}

/**
 * @brief Check 2: every block address in every in-use inode points into the data region.
 *
 * For each inode with type != 0 this tests the 12 direct addresses, then the
 * indirect address, then all 128 numbers inside the indirect block. A value of
 * 0 is allowed everywhere because it means "no block".
 * Prints BAD_DIRECT_ADDRESS or BAD_INDIRECT_ADDRESS on failure.
 *
 * @param image The loaded image.
 * @return 0 when all addresses are valid, -1 on the first bad one.
 */
static int check_inode_addresses(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode inode = read_inode(image, number);                    // copy of the inode
        if (inode.type == 0)                                               // free inodes are not checked
            continue;

        for (size_t i = 0; i < DIRECT_COUNT; i++) {                        // the 12 direct addresses
            if (inode.addresses[i] != 0                                    // 0 means no block, which is fine
                && !valid_data_block(image, inode.addresses[i])) {         // otherwise it must be in 59..999
                fprintf(stderr, "%s\n", BAD_DIRECT_ADDRESS);
                return -1;
            }
        }

        uint32_t indirect = inode.addresses[DIRECT_COUNT];                 // address of the indirect block
        if (indirect == 0)                                                 // no indirect block, nothing more to check
            continue;
        if (!valid_data_block(image, indirect)) {                          // the indirect block itself must be valid
            fprintf(stderr, "%s\n", BAD_INDIRECT_ADDRESS);
            return -1;
        }

        const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;  // first byte of the indirect block
        for (size_t i = 0; i < INDIRECT_COUNT; i++) {                      // its 128 block numbers
            uint32_t block = read_u32(entries + i * sizeof(uint32_t));     // number i, 4 bytes each
            if (block != 0 && !valid_data_block(image, block)) {           // 0 is fine, others must be in 59..999
                fprintf(stderr, "%s\n", BAD_INDIRECT_ADDRESS);
                return -1;
            }
        }
    }
    return 0;  // every address was valid
}

/**
 * @brief Find the block number of the index-th block of a file or directory.
 *
 * Index 0 to 11 come from the direct addresses. Index 12 to 139 come from the
 * indirect block: number (index - 12). Example: index 12 is the first number
 * stored in the indirect block.
 *
 * @param image The loaded image.
 * @param inode The file or directory.
 * @param index Which block of its content, counting from 0.
 * @return The block number, or 0 when the index is out of range or that block is missing.
 */
static uint32_t file_block(const struct image *image, const struct inode *inode,
                           uint32_t index)
{
    if (index >= MAX_FILE_BLOCKS)                          // no file has 140 or more blocks
        return 0;
    if (index < DIRECT_COUNT)                              // index 0..11
        return inode->addresses[index];                    // stored directly in the inode

    uint32_t indirect = inode->addresses[DIRECT_COUNT];    // address of the indirect block
    if (indirect == 0)                                     // there is none
        return 0;
    const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;       // first byte of the indirect block
    return read_u32(entries + (index - DIRECT_COUNT) * sizeof(uint32_t));       // number (index - 12), 4 bytes each
}

/**
 * @brief Test whether the name stored in a directory entry equals `name`.
 *
 * The name field is 14 bytes at entry + 2, padded with zero bytes. So `name`
 * matches only when its characters equal the field and a zero byte follows
 * them. That is why name_is(entry, ".") is false for an entry named "..".
 * Only names shorter than 14 characters can be tested (it is used for "." and "..").
 *
 * @param entry Address of a 16 byte directory entry.
 * @param name  The name to compare with, as a C string.
 * @return 1 when the entry has exactly this name, 0 otherwise.
 */
static int name_is(const uint8_t *entry, const char *name)
{
    size_t length = 0;                          // length of `name`
    while (name[length] != '\0')                // count characters up to the terminator
        length++;
    return length < 14 && entry[2 + length] == '\0'   // name must fit, and the entry's name must end right here
        && memcmp(entry + 2, name, length) == 0;      // and the characters before that must be identical
}

/**
 * @brief Get the address of one 16 byte entry inside a directory's content.
 *
 * `offset` is a position inside the directory content, not inside the image.
 * It is translated to a block with file_block(offset / 512) and then to a byte
 * with block x 512 + offset % 512. The returned pointer points into the mapped
 * image, so nothing is copied.
 *
 * @param image     The loaded image.
 * @param directory The directory inode.
 * @param offset    Byte position of the entry in the directory (0, 16, 32, ...).
 * @return Address of the entry, or NULL when the offset is past the end, the size is above 71,680 bytes, or that block is missing.
 */
static const uint8_t *directory_entry(const struct image *image,
                                      const struct inode *directory,
                                      uint32_t offset)
{
    if (offset >= directory->size || directory->size > MAX_FILE_BLOCKS * BLOCK_SIZE)  // past the end, or impossible size
        return NULL;

    uint32_t block = file_block(image, directory, offset / BLOCK_SIZE);  // which block of the directory holds this offset
    if (block == 0)                                                      // that block is not allocated
        return NULL;
    return image->data + (size_t)block * BLOCK_SIZE + offset % BLOCK_SIZE;  // start of the block + position inside the block
}

/**
 * @brief Check 3: the root directory exists.
 *
 * Inode 1 must be a directory, and it must contain an entry named ".." whose
 * inode number is 1 (the parent of the root is the root itself).
 * Prints ROOT_MISSING on failure.
 *
 * @param image The loaded image.
 * @return 0 when the root is found, -1 otherwise.
 */
static int check_root(const struct image *image)
{
    struct inode root = read_inode(image, 1);       // the root is always inode 1 (inode 0 is never used)
    if (root.type != TYPE_DIRECTORY) {              // free, file or device: not a root directory
        fprintf(stderr, "%s\n", ROOT_MISSING);
        return -1;
    }

    for (uint32_t offset = 0;                              // start at the first entry
         offset + DIRECTORY_ENTRY_SIZE <= root.size;       // while a whole 16 byte entry fits inside the size
         offset += DIRECTORY_ENTRY_SIZE) {                 // next entry
        const uint8_t *entry = directory_entry(image, &root, offset);  // address of this entry
        if (entry == NULL) {                               // its block is missing
            fprintf(stderr, "%s\n", ROOT_MISSING);
            return -1;
        }
        if (read_u16(entry) == 1 && name_is(entry, ".."))  // entry points to inode 1 and is called ".."
            return 0;                                      // the root is its own parent: pass
    }

    fprintf(stderr, "%s\n", ROOT_MISSING);                 // no such entry in the whole root directory
    return -1;
}

/**
 * @brief Check 4: every directory is well formed.
 *
 * For every inode of type directory:
 *  - size is a multiple of 16 and at most 71,680 bytes,
 *  - every entry can be reached (its block exists),
 *  - exactly one entry is named "." and it holds the directory's own number,
 *  - exactly one entry is named "..".
 * It does not check where ".." of a subdirectory points. Prints BAD_DIRECTORY on failure.
 *
 * @param image The loaded image.
 * @return 0 when all directories are well formed, -1 on the first bad one.
 */
static int check_directory_format(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode directory = read_inode(image, number);                // copy of the inode
        if (directory.type != TYPE_DIRECTORY)                              // only directories are checked
            continue;
        if (directory.size % DIRECTORY_ENTRY_SIZE != 0                     // not a whole number of entries
            || directory.size > MAX_FILE_BLOCKS * BLOCK_SIZE) {            // or larger than any file can be
            fprintf(stderr, "%s\n", BAD_DIRECTORY);
            return -1;
        }

        unsigned dot_count = 0;       // how many entries are named "."
        unsigned dotdot_count = 0;    // how many entries are named ".."
        for (uint32_t offset = 0; offset < directory.size;                 // every entry of this directory
             offset += DIRECTORY_ENTRY_SIZE) {
            const uint8_t *entry = directory_entry(image, &directory, offset);  // address of the entry
            if (entry == NULL) {                                           // its block is missing
                fprintf(stderr, "%s\n", BAD_DIRECTORY);
                return -1;
            }
            if (name_is(entry, ".")) {                                     // the "." entry
                dot_count++;
                if (read_u16(entry) != number) {                           // "." must name the directory itself
                    fprintf(stderr, "%s\n", BAD_DIRECTORY);
                    return -1;
                }
            } else if (name_is(entry, "..")) {                             // the ".." entry
                dotdot_count++;
            }
        }
        if (dot_count != 1 || dotdot_count != 1) {                         // each must appear exactly once
            fprintf(stderr, "%s\n", BAD_DIRECTORY);
            return -1;
        }
    }
    return 0;  // every directory was well formed
}

/**
 * @brief Allocate the three block arrays of a struct block_state, all zero.
 *
 * Each array gets sb.size bytes (one per block, 1000 for the reference image).
 * calloc() fills the memory with zeros. On failure everything is freed, the
 * pointers are set to NULL and "xcheck: out of memory" is printed.
 *
 * @param image The loaded image (gives sb.size).
 * @param state The structure to fill in.
 * @return 0 on success, -1 when memory ran out.
 */
static int init_block_state(const struct image *image, struct block_state *state)
{
    state->used = calloc(image->sb.size, sizeof(*state->used));                    // 1000 zero bytes
    state->direct_refs = calloc(image->sb.size, sizeof(*state->direct_refs));      // 1000 zero bytes
    state->indirect_refs = calloc(image->sb.size, sizeof(*state->indirect_refs));  // 1000 zero bytes
    if (state->used == NULL || state->direct_refs == NULL || state->indirect_refs == NULL) {  // any allocation failed
        free(state->used);             // free whatever was allocated (free(NULL) does nothing)
        free(state->direct_refs);
        free(state->indirect_refs);
        state->used = NULL;            // leave the structure in a safe, empty state
        state->direct_refs = NULL;
        state->indirect_refs = NULL;
        fprintf(stderr, "xcheck: out of memory\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Free the arrays of a struct block_state. Safe when some or all pointers are NULL.
 *
 * @param state The structure to release.
 */
static void free_block_state(struct block_state *state)
{
    free(state->used);           // free(NULL) is a no-op, so this is safe after a failed init
    free(state->direct_refs);
    free(state->indirect_refs);
}

/**
 * @brief Read the bitmap bit of a block.
 *
 * The bitmap has one bit for every block of the disk, from block 0 (boot) to
 * block size - 1, including the metadata blocks. Block b is at byte
 * bitmap_start x 512 + b / 8, bit b % 8. Example: block 66 is byte 8 of the
 * bitmap, bit 2.
 *
 * @param image The loaded image.
 * @param block Block number, below sb.size.
 * @return 1 when the bit is set (block in use), 0 when it is clear (block free).
 */
static int bitmap_is_set(const struct image *image, uint32_t block)
{
    size_t offset = (size_t)image->sb.bitmap_start * BLOCK_SIZE + block / 8;  // byte of the bitmap holding this block's bit
    return (image->data[offset] & (1U << (block % 8))) != 0;                  // isolate bit (block % 8) and test it
}

/**
 * @brief Record that some inode refers to `block`.
 *
 * Sets used[block] to 1 and adds 1 to direct_refs[block] or indirect_refs[block].
 * The counter stops at 2, because the checks only need "0, 1, or more than one",
 * and because a uint8_t would wrap around to 0 after 255 increments.
 *
 * @param state     The block accounting arrays.
 * @param block     Block number (already known to be a valid data block).
 * @param is_direct 1 when it came from a direct address, 0 when it came from an indirect block.
 */
static void add_block_reference(struct block_state *state, uint32_t block,
                                int is_direct)
{
    uint8_t *count = is_direct ? &state->direct_refs[block]    // pick the direct counter ...
                               : &state->indirect_refs[block]; // ... or the indirect counter
    state->used[block] = 1;      // the block is in use by some inode
    if (*count < 2)              // saturate at 2
        (*count)++;
}

/**
 * @brief Fill block_state by walking every in-use inode.
 *
 * Every nonzero direct address is counted as a direct reference. A nonzero
 * indirect address is counted as an indirect reference (the indirect block
 * itself occupies a data block), and so is every nonzero number stored in it.
 * Must run after checks 1 to 4, which guarantee that every address is valid.
 *
 * @param image The loaded image.
 * @param state Zero-filled arrays from init_block_state().
 */
static void collect_block_references(const struct image *image,
                                    struct block_state *state)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode inode = read_inode(image, number);                    // copy of the inode
        if (inode.type == 0)                                               // free inodes use no blocks
            continue;

        for (size_t i = 0; i < DIRECT_COUNT; i++) {                        // the 12 direct addresses
            uint32_t block = inode.addresses[i];
            if (block != 0)                                                // 0 means no block
                add_block_reference(state, block, 1);                      // count it as a direct reference
        }

        uint32_t indirect = inode.addresses[DIRECT_COUNT];                 // address of the indirect block
        if (indirect == 0)                                                 // none: go to the next inode
            continue;
        /* The indirect block itself also occupies an allocated data block. */
        add_block_reference(state, indirect, 0);                           // count the indirect block itself

        const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;  // first byte of the indirect block
        for (size_t i = 0; i < INDIRECT_COUNT; i++) {                      // its 128 numbers
            uint32_t block = read_u32(entries + i * sizeof(uint32_t));
            if (block != 0)
                add_block_reference(state, block, 0);                      // count each as an indirect reference
        }
    }
}

/**
 * @brief Check 5: every block an inode uses is marked in use in the bitmap.
 *
 * Reads the inodes and the bitmap directly (it does not use block_state).
 * Prints FREE_BLOCK_REFERENCE on failure.
 *
 * @param image The loaded image.
 * @return 0 when all used blocks are marked, -1 on the first block whose bit is 0.
 */
static int check_bitmap_for_references(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode inode = read_inode(image, number);
        if (inode.type == 0)                                               // free inodes use no blocks
            continue;

        for (size_t i = 0; i < DIRECT_COUNT; i++) {                        // the 12 direct addresses
            uint32_t block = inode.addresses[i];
            if (block != 0 && !bitmap_is_set(image, block)) {              // used, but the bitmap says free
                fprintf(stderr, "%s\n", FREE_BLOCK_REFERENCE);
                return -1;
            }
        }

        uint32_t indirect = inode.addresses[DIRECT_COUNT];                 // address of the indirect block
        if (indirect == 0)
            continue;
        if (!bitmap_is_set(image, indirect)) {                             // the indirect block must be marked too
            fprintf(stderr, "%s\n", FREE_BLOCK_REFERENCE);
            return -1;
        }

        const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;  // first byte of the indirect block
        for (size_t i = 0; i < INDIRECT_COUNT; i++) {                      // its 128 numbers
            uint32_t block = read_u32(entries + i * sizeof(uint32_t));
            if (block != 0 && !bitmap_is_set(image, block)) {              // used, but the bitmap says free
                fprintf(stderr, "%s\n", FREE_BLOCK_REFERENCE);
                return -1;
            }
        }
    }
    return 0;  // every used block is marked
}

/**
 * @brief Check 6: every data block marked in use in the bitmap is really used.
 *
 * Tests blocks data_start to size - 1 only; the bits of the metadata blocks
 * (0 to 58) are not tested. Prints UNUSED_BITMAP_BLOCK on failure.
 *
 * @param image The loaded image.
 * @param state Arrays filled by collect_block_references().
 * @return 0 when all marked blocks are used, -1 on the first marked block nobody uses.
 */
static int check_bitmap_for_unused_blocks(const struct image *image,
                                          const struct block_state *state)
{
    for (uint32_t block = image->data_start; block < image->sb.size; block++) {  // data blocks 59..999
        if (bitmap_is_set(image, block) && !state->used[block]) {                // marked in use but no inode uses it
            fprintf(stderr, "%s\n", UNUSED_BITMAP_BLOCK);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Checks 7 and 8: no block belongs to two owners.
 *
 * Check 7 fails when a block is a direct address more than once, or is both a
 * direct address and an indirect reference. Check 8 fails when a block is an
 * indirect reference more than once. Check 7 runs over all blocks first.
 * Prints REPEATED_DIRECT_ADDRESS or REPEATED_INDIRECT_ADDRESS.
 *
 * @param image The loaded image.
 * @param state Arrays filled by collect_block_references().
 * @return 0 when no block is shared, -1 on the first shared block.
 */
static int check_duplicate_blocks(const struct image *image,
                                  const struct block_state *state)
{
    for (uint32_t block = image->data_start; block < image->sb.size; block++) {  // check 7 over the data region
        if (state->direct_refs[block] > 1                                        // direct more than once
            || (state->direct_refs[block] > 0 && state->indirect_refs[block] > 0)) {  // or direct and indirect
            fprintf(stderr, "%s\n", REPEATED_DIRECT_ADDRESS);
            return -1;
        }
    }
    for (uint32_t block = image->data_start; block < image->sb.size; block++) {  // check 8 over the data region
        if (state->indirect_refs[block] > 1) {                                   // indirect more than once
            fprintf(stderr, "%s\n", REPEATED_INDIRECT_ADDRESS);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Allocate the two inode arrays of a struct directory_state, all zero.
 *
 * Each array has sb.inode_count entries of 4 bytes (800 bytes for 200 inodes).
 * On failure everything is freed, the pointers are set to NULL and
 * "xcheck: out of memory" is printed.
 *
 * @param image The loaded image (gives sb.inode_count).
 * @param state The structure to fill in.
 * @return 0 on success, -1 when memory ran out.
 */
static int init_directory_state(const struct image *image,
                                struct directory_state *state)
{
    state->inode_refs = calloc(image->sb.inode_count, sizeof(*state->inode_refs));          // 200 zero counters
    state->directory_refs = calloc(image->sb.inode_count, sizeof(*state->directory_refs));  // 200 zero counters
    if (state->inode_refs == NULL || state->directory_refs == NULL) {                       // any allocation failed
        free(state->inode_refs);       // free whatever was allocated
        free(state->directory_refs);
        state->inode_refs = NULL;      // leave the structure empty
        state->directory_refs = NULL;
        fprintf(stderr, "xcheck: out of memory\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Free the arrays of a struct directory_state. Safe when pointers are NULL.
 *
 * @param state The structure to release.
 */
static void free_directory_state(struct directory_state *state)
{
    free(state->inode_refs);       // free(NULL) is a no-op
    free(state->directory_refs);
}

/**
 * @brief Fill directory_state by reading every entry of every directory.
 *
 * For each entry the target inode number is the first 2 bytes. Entries with
 * target 0 (empty slot) or target >= inode_count are skipped here (check 10
 * reports the out-of-range ones). inode_refs[target] counts every valid entry.
 * directory_refs[target] counts only entries that are not "." or "..", and only
 * when the target is a directory.
 * Must run after check 4, which guarantees every entry can be reached.
 *
 * @param image The loaded image.
 * @param state Zero-filled arrays from init_directory_state().
 */
static void collect_directory_references(const struct image *image,
                                         struct directory_state *state)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode directory = read_inode(image, number);
        if (directory.type != TYPE_DIRECTORY)                              // only directories hold entries
            continue;

        for (uint32_t offset = 0; offset < directory.size;                 // every entry of this directory
             offset += DIRECTORY_ENTRY_SIZE) {
            const uint8_t *entry = directory_entry(image, &directory, offset);  // address of the entry
            uint32_t target = read_u16(entry);                             // inode number this entry names
            if (target == 0 || target >= image->sb.inode_count)            // empty slot or out of range
                continue;
            state->inode_refs[target]++;                                   // one more name for this inode
            /* Dot entries describe the directory itself and its parent. */
            if (!name_is(entry, ".") && !name_is(entry, "..")              // a real name, not "." or ".."
                && read_inode(image, target).type == TYPE_DIRECTORY)       // and the target is a directory
                state->directory_refs[target]++;                           // one more real name for this directory
        }
    }
}

/**
 * @brief Check 9: every inode in use is named by at least one directory entry.
 *
 * Prints UNREFERENCED_INODE on failure.
 *
 * @param image The loaded image.
 * @param state Arrays filled by collect_directory_references().
 * @return 0 when every in-use inode has a name, -1 on the first one without.
 */
static int check_inodes_referenced(const struct image *image,
                                   const struct directory_state *state)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        if (read_inode(image, number).type != 0 && state->inode_refs[number] == 0) {  // in use but nobody names it
            fprintf(stderr, "%s\n", UNREFERENCED_INODE);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Check 10: every directory entry names an inode that is in use.
 *
 * An entry is accepted when its inode number is 0 (empty slot) or names an
 * inode that exists and is not free. Prints FREE_INODE_REFERENCE on failure.
 * Must run after check 4, which guarantees every entry can be reached.
 *
 * @param image The loaded image.
 * @return 0 when all entries are fine, -1 on the first bad entry.
 */
static int check_directory_targets(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode directory = read_inode(image, number);
        if (directory.type != TYPE_DIRECTORY)                              // only directories hold entries
            continue;

        for (uint32_t offset = 0; offset < directory.size;                 // every entry of this directory
             offset += DIRECTORY_ENTRY_SIZE) {
            const uint8_t *entry = directory_entry(image, &directory, offset);  // address of the entry
            uint32_t target = read_u16(entry);                             // inode number this entry names
            if (target != 0 && (target >= image->sb.inode_count            // out of range ...
                                || read_inode(image, target).type == 0)) { // ... or names a free inode
                fprintf(stderr, "%s\n", FREE_INODE_REFERENCE);
                return -1;
            }
        }
    }
    return 0;
}

/**
 * @brief Check 11: a file's link count equals the number of entries that name it.
 *
 * Only inodes of type file are compared (directories are not). A hard link is a
 * second name for the same inode, so it raises both numbers by one.
 * Prints BAD_FILE_LINKS on failure.
 *
 * @param image The loaded image.
 * @param state Arrays filled by collect_directory_references().
 * @return 0 when all file link counts match, -1 on the first mismatch.
 */
static int check_file_link_counts(const struct image *image,
                                  const struct directory_state *state)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        struct inode inode = read_inode(image, number);
        if (inode.type == TYPE_FILE && inode.links != state->inode_refs[number]) {  // stored count differs from counted names
            fprintf(stderr, "%s\n", BAD_FILE_LINKS);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Check 12: no directory is named by more than one entry.
 *
 * "." and ".." are not counted (see directory_state.directory_refs).
 * Prints REPEATED_DIRECTORY on failure.
 *
 * @param image The loaded image.
 * @param state Arrays filled by collect_directory_references().
 * @return 0 when every directory has at most one name, -1 on the first with more.
 */
static int check_repeated_directory_links(const struct image *image,
                                          const struct directory_state *state)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {  // every inode
        if (read_inode(image, number).type == TYPE_DIRECTORY               // a directory ...
            && state->directory_refs[number] > 1) {                        // ... named more than once
            fprintf(stderr, "%s\n", REPEATED_DIRECTORY);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Open, map and sanity-check the image, and fill struct image.
 *
 * Steps:
 *  1. open() the file read-only.
 *  2. fstat() the size: at least 1024 bytes and a multiple of 512.
 *  3. mmap() the whole file read-only so it can be used as an array.
 *  4. Copy the seven superblock numbers (bytes 512 to 539) into image->sb.
 *  5. Consistency of the numbers with the file: size == length / 512,
 *     inode_count >= 2, data_blocks != 0, data_blocks < size.
 *  6. Compute data_start and where the inode table and bitmap end.
 *  7. Layout: inode_start >= 2, inode table ends before the bitmap starts,
 *     the bitmap ends exactly at data_start, and the log (if any) lies between
 *     block 2 and inode_start.
 *
 * It does not undo anything on failure; the caller calls close_image().
 * Prints "image not found.", a perror message, or INVALID_IMAGE.
 *
 * @param image Structure to fill. fd must be -1 and data must be NULL on entry.
 * @param path  Path of the image file.
 * @return 0 on success, -1 on failure.
 */
static int load_image(struct image *image, const char *path)
{
    struct stat file_stat;     // result of fstat(): holds the file size
    uint64_t inode_blocks;     // blocks needed for the inode table: 25
    uint64_t bitmap_blocks;    // blocks needed for the bitmap: 1
    uint64_t inode_end;        // first block after the inode table: 57
    uint64_t bitmap_end;       // first block after the bitmap: 59

    image->fd = open(path, O_RDONLY);           // open the file read-only; fd stays valid for mmap
    if (image->fd < 0) {                        // a negative value means open() failed
        if (errno == ENOENT || errno == ENOTDIR)    // the path does not exist
            fprintf(stderr, "image not found.\n");
        else
            perror("xcheck");                   // any other reason: print the system's message
        return -1;
    }

    if (fstat(image->fd, &file_stat) < 0 || file_stat.st_size < 2 * BLOCK_SIZE   // cannot read size, or fewer than 2 blocks
        || file_stat.st_size % BLOCK_SIZE != 0) {                                // or not a whole number of blocks
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    image->length = (size_t)file_stat.st_size;  // image length in bytes (512000)
    // Map the file's bytes into memory read-only; nothing is copied up front, the OS loads pages when they are touched.
    image->data = mmap(NULL, image->length, PROT_READ, MAP_PRIVATE, image->fd, 0);
    if (image->data == MAP_FAILED) {            // the OS could not create the mapping
        image->data = NULL;                     // keep close_image() from unmapping garbage
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    const uint8_t *sb = image->data + SUPERBLOCK_OFFSET;   // pointer to byte 512, the first byte of the superblock
    image->sb.size = read_u32(sb);                         // bytes 512-515: total blocks, 1000
    image->sb.data_blocks = read_u32(sb + 4);              // bytes 516-519: data blocks, 941
    image->sb.inode_count = read_u32(sb + 8);              // bytes 520-523: inodes, 200
    image->sb.log_count = read_u32(sb + 12);               // bytes 524-527: log blocks, 30
    image->sb.log_start = read_u32(sb + 16);               // bytes 528-531: first log block, 2
    image->sb.inode_start = read_u32(sb + 20);             // bytes 532-535: first inode block, 32
    image->sb.bitmap_start = read_u32(sb + 24);            // bytes 536-539: first bitmap block, 58

    // Check A: do the numbers agree with the file?
    if (image->sb.size != image->length / BLOCK_SIZE       // superblock size must equal file length / 512
        || image->sb.inode_count < 2                       // need inode 0 (never used) and inode 1 (root)
        || image->sb.data_blocks == 0                      // there must be a data region
        || image->sb.data_blocks >= image->sb.size) {      // and metadata must take some blocks; also avoids unsigned underflow below
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    image->data_start = image->sb.size - image->sb.data_blocks;   // first data block: 1000 - 941 = 59
    inode_blocks = ((uint64_t)image->sb.inode_count + INODES_PER_BLOCK - 1)
        / INODES_PER_BLOCK;                                       // (200 + 7) / 8 = 25, divide and round up
    bitmap_blocks = ((uint64_t)image->sb.size + BLOCK_BITS - 1) / BLOCK_BITS;  // (1000 + 4095) / 4096 = 1, one bit per block of the disk
    inode_end = (uint64_t)image->sb.inode_start + inode_blocks;   // 32 + 25 = 57, first block after the inode table
    bitmap_end = (uint64_t)image->sb.bitmap_start + bitmap_blocks; // 58 + 1 = 59, first block after the bitmap

    // Check B: do the regions fit together without overlap?
    if (image->sb.inode_start < 2                          // blocks 0 and 1 are boot and superblock
        || inode_end > image->sb.bitmap_start              // inode table must not run into the bitmap
        || bitmap_end != image->data_start                 // bitmap must end exactly where the data region begins
        || (image->sb.log_count != 0                       // if there is a log ...
            && ((uint64_t)image->sb.log_start + image->sb.log_count > image->sb.inode_start  // ... it must end before the inode table
                || image->sb.log_start < 2))) {            // ... and start after the superblock
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    return 0;   // the image is mapped and its layout is believable
}

/**
 * @brief Release what load_image() acquired. Safe on a partly initialised image.
 *
 * @param image The image; fd must be -1 and data NULL when not acquired.
 */
static void close_image(struct image *image)
{
    if (image->data != NULL)                 // the file was mapped
        munmap(image->data, image->length);  // remove the mapping
    if (image->fd >= 0)                      // the file was opened
        close(image->fd);                    // close it
}

/**
 * @brief Program entry: load the image and run the 12 checks in order.
 *
 * @param argc Must be 2 (program name and image path).
 * @param argv argv[1] is the path of the image file.
 * @return 0 when the image passes every check, 1 on any failure or on bad usage.
 */
int main(int argc, char **argv)
{
    // Handle for the mapped image and its superblock numbers. fd -1 and data NULL mean "nothing acquired yet".
    struct image image = { .fd = -1, .data = NULL, .length = 0 };
    // Block accounting: which blocks are used and how many times each is listed as direct or indirect.
    // All pointers start NULL so that free_block_state() is always safe to call.
    struct block_state blocks = { .used = NULL, .direct_refs = NULL, .indirect_refs = NULL };
    // Name accounting: how many directory entries name each inode, and how often each directory is named.
    // All pointers start NULL so that free_directory_state() is always safe to call.
    struct directory_state directories = { .inode_refs = NULL, .directory_refs = NULL };

    if (argc != 2) {                                       // exactly one argument is expected
        fprintf(stderr, "Usage: xcheck <file_system_image>\n");
        return 1;
    }

    // Open and map the image given on the command line, and sanity-check the superblock.
    if (load_image(&image, argv[1]) < 0) {
        close_image(&image);                               // release whatever was opened before the failure
        return 1;
    }

    // Phase 1: make the image safe to walk. Each call returns 0 when fine, or prints one error and returns -1.
    // The || chain stops at the first failure, so only one error is ever printed.
    //  1> check_inode_types: every inode's type is 0 (free), 1 (directory), 2 (file) or 3 (device).
    //  2> check_inode_addresses: every direct address, the indirect address, and every number inside
    //     the indirect block is either 0 (no block) or a data block, 59 <= b < 1000.
    //  3> check_root: inode 1 is a directory and has an entry ".." that points to inode 1 itself.
    //  4> check_directory_format: every directory has a size that is a multiple of 16, entries that can
    //     be reached, exactly one "." (pointing to itself) and exactly one "..".
    //  5> init_block_state: allocate the three block arrays. This is not a check on the image; it fails
    //     only when memory runs out.
    // On failure: free the block arrays (safe even if they were never allocated), release the image, exit 1.
    if (check_inode_types(&image) < 0
        || check_inode_addresses(&image) < 0
        || check_root(&image) < 0
        || check_directory_format(&image) < 0
        || init_block_state(&image, &blocks) < 0) {
        free_block_state(&blocks);
        close_image(&image);
        return 1;
    }

    // Phase 2: compare the bitmap with the blocks that inodes really use.
    // First walk all inodes once and fill used, direct_refs and indirect_refs.
    collect_block_references(&image, &blocks);
    //  5> check_bitmap_for_references: every block an inode uses has its bitmap bit set.
    //  6> check_bitmap_for_unused_blocks: every data block with its bitmap bit set is used by some inode.
    //  7, 8> check_duplicate_blocks: no block is used twice (7: direct or direct+indirect, 8: indirect).
    if (check_bitmap_for_references(&image) < 0
        || check_bitmap_for_unused_blocks(&image, &blocks) < 0
        || check_duplicate_blocks(&image, &blocks) < 0) {
        free_block_state(&blocks);
        close_image(&image);
        return 1;
    }

    // Allocate the two inode arrays. This fails only when memory runs out.
    if (init_directory_state(&image, &directories) < 0) {
        free_directory_state(&directories);
        free_block_state(&blocks);
        close_image(&image);
        return 1;
    }

    // Phase 3: compare directory entries with inodes.
    // First walk all directory entries once and fill inode_refs and directory_refs.
    collect_directory_references(&image, &directories);
    //  9> check_inodes_referenced: every inode in use is named by at least one directory entry.
    // 10> check_directory_targets: no directory entry names an inode that is free or out of range.
    // 11> check_file_link_counts: for each file, the stored link count equals the number of entries naming it.
    // 12> check_repeated_directory_links: no directory is named by more than one entry (besides "." and "..").
    if (check_inodes_referenced(&image, &directories) < 0
        || check_directory_targets(&image) < 0
        || check_file_link_counts(&image, &directories) < 0
        || check_repeated_directory_links(&image, &directories) < 0) {
        free_directory_state(&directories);
        free_block_state(&blocks);
        close_image(&image);
        return 1;
    }

    // All 12 checks passed: release everything and report success.
    free_directory_state(&directories);
    free_block_state(&blocks);
    close_image(&image);
    return 0;
}