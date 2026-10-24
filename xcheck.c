#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK_SIZE 512
#define BLOCK_BITS (BLOCK_SIZE * 8)
#define INODE_SIZE 64
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define DIRECTORY_ENTRY_SIZE 16
#define DIRECT_COUNT 12
#define INDIRECT_COUNT (BLOCK_SIZE / sizeof(uint32_t))
#define MAX_FILE_BLOCKS (DIRECT_COUNT + INDIRECT_COUNT)
#define SUPERBLOCK_OFFSET BLOCK_SIZE
#define TYPE_DIRECTORY 1
#define TYPE_FILE 2
#define TYPE_DEVICE 3

#define INVALID_IMAGE "ERROR: invalid file system image."
#define BAD_INODE "ERROR: bad inode."
#define BAD_DIRECT_ADDRESS "ERROR: bad direct address in inode."
#define BAD_INDIRECT_ADDRESS "ERROR: bad indirect address in inode."
#define ROOT_MISSING "ERROR: root directory does not exist."

struct superblock {
    uint32_t size;
    uint32_t data_blocks;
    uint32_t inode_count;
    uint32_t log_count;
    uint32_t log_start;
    uint32_t inode_start;
    uint32_t bitmap_start;
};

struct image {
    int fd;
    uint8_t *data;
    size_t length;
    struct superblock sb;
    uint32_t data_start;
};

struct block_state {
    uint8_t *used;
    uint8_t *direct_refs;
    uint8_t *indirect_refs;
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t addresses[DIRECT_COUNT + 1];
};

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
        | (uint32_t)p[1] << 8
        | (uint32_t)p[2] << 16
        | (uint32_t)p[3] << 24;
}

static struct inode read_inode(const struct image *image, uint32_t number)
{
    struct inode inode;
    size_t offset = (size_t)image->sb.inode_start * BLOCK_SIZE
        + (size_t)number * INODE_SIZE;
    const uint8_t *bytes = image->data + offset;

    inode.type = read_u16(bytes);
    inode.links = read_u16(bytes + 6);
    inode.size = read_u32(bytes + 8);
    for (size_t i = 0; i < DIRECT_COUNT + 1; i++)
        inode.addresses[i] = read_u32(bytes + 12 + i * sizeof(uint32_t));
    return inode;
}

static int valid_data_block(const struct image *image, uint32_t block)
{
    return block >= image->data_start && block < image->sb.size;
}

static int check_inode_types(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {
        uint16_t type = read_inode(image, number).type;
        if (type != 0 && type != TYPE_DIRECTORY && type != TYPE_FILE
            && type != TYPE_DEVICE) {
            fprintf(stderr, "%s\n", BAD_INODE);
            return -1;
        }
    }
    return 0;
}

static int check_inode_addresses(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {
        struct inode inode = read_inode(image, number);
        if (inode.type == 0)
            continue;

        for (size_t i = 0; i < DIRECT_COUNT; i++) {
            if (inode.addresses[i] != 0
                && !valid_data_block(image, inode.addresses[i])) {
                fprintf(stderr, "%s\n", BAD_DIRECT_ADDRESS);
                return -1;
            }
        }

        uint32_t indirect = inode.addresses[DIRECT_COUNT];
        if (indirect == 0)
            continue;
        if (!valid_data_block(image, indirect)) {
            fprintf(stderr, "%s\n", BAD_INDIRECT_ADDRESS);
            return -1;
        }

        const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;
        for (size_t i = 0; i < INDIRECT_COUNT; i++) {
            uint32_t block = read_u32(entries + i * sizeof(uint32_t));
            if (block != 0 && !valid_data_block(image, block)) {
                fprintf(stderr, "%s\n", BAD_INDIRECT_ADDRESS);
                return -1;
            }
        }
    }
    return 0;
}

static uint32_t file_block(const struct image *image, const struct inode *inode,
                           uint32_t index)
{
    if (index >= MAX_FILE_BLOCKS)
        return 0;
    if (index < DIRECT_COUNT)
        return inode->addresses[index];

    uint32_t indirect = inode->addresses[DIRECT_COUNT];
    if (indirect == 0)
        return 0;
    const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;
    return read_u32(entries + (index - DIRECT_COUNT) * sizeof(uint32_t));
}

static int name_is(const uint8_t *entry, const char *name)
{
    size_t length = 0;
    while (name[length] != '\0')
        length++;
    return length < 14 && entry[2 + length] == '\0'
        && memcmp(entry + 2, name, length) == 0;
}

static const uint8_t *directory_entry(const struct image *image,
                                      const struct inode *directory,
                                      uint32_t offset)
{
    if (offset >= directory->size || directory->size % DIRECTORY_ENTRY_SIZE != 0
        || directory->size > MAX_FILE_BLOCKS * BLOCK_SIZE)
        return NULL;

    uint32_t block = file_block(image, directory, offset / BLOCK_SIZE);
    if (block == 0)
        return NULL;
    return image->data + (size_t)block * BLOCK_SIZE + offset % BLOCK_SIZE;
}

static int check_root(const struct image *image)
{
    struct inode root = read_inode(image, 1);
    if (root.type != TYPE_DIRECTORY || root.size % DIRECTORY_ENTRY_SIZE != 0) {
        fprintf(stderr, "%s\n", ROOT_MISSING);
        return -1;
    }

    for (uint32_t offset = 0; offset < root.size; offset += DIRECTORY_ENTRY_SIZE) {
        const uint8_t *entry = directory_entry(image, &root, offset);
        if (entry == NULL) {
            fprintf(stderr, "%s\n", ROOT_MISSING);
            return -1;
        }
        if (read_u16(entry) == 1 && name_is(entry, ".."))
            return 0;
    }

    fprintf(stderr, "%s\n", ROOT_MISSING);
    return -1;
}

static int init_block_state(const struct image *image, struct block_state *state)
{
    state->used = calloc(image->sb.size, sizeof(*state->used));
    state->direct_refs = calloc(image->sb.size, sizeof(*state->direct_refs));
    state->indirect_refs = calloc(image->sb.size, sizeof(*state->indirect_refs));
    if (state->used == NULL || state->direct_refs == NULL || state->indirect_refs == NULL) {
        fprintf(stderr, "xcheck: out of memory\n");
        return -1;
    }
    return 0;
}

static void free_block_state(struct block_state *state)
{
    free(state->used);
    free(state->direct_refs);
    free(state->indirect_refs);
}

static int bitmap_is_set(const struct image *image, uint32_t block)
{
    size_t offset = (size_t)image->sb.bitmap_start * BLOCK_SIZE + block / 8;
    return (image->data[offset] & (1U << (block % 8))) != 0;
}

static void add_block_reference(struct block_state *state, uint32_t block,
                                int is_direct)
{
    uint8_t *count = is_direct ? &state->direct_refs[block]
                               : &state->indirect_refs[block];
    state->used[block] = 1;
    if (*count < 2)
        (*count)++;
}

static int collect_block_references(const struct image *image,
                                    struct block_state *state)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {
        struct inode inode = read_inode(image, number);
        if (inode.type == 0)
            continue;

        for (size_t i = 0; i < DIRECT_COUNT; i++) {
            uint32_t block = inode.addresses[i];
            if (block != 0)
                add_block_reference(state, block, 1);
        }

        uint32_t indirect = inode.addresses[DIRECT_COUNT];
        if (indirect == 0)
            continue;
        add_block_reference(state, indirect, 0);

        const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;
        for (size_t i = 0; i < INDIRECT_COUNT; i++) {
            uint32_t block = read_u32(entries + i * sizeof(uint32_t));
            if (block != 0)
                add_block_reference(state, block, 0);
        }
    }
    return 0;
}

static int check_bitmap_for_references(const struct image *image)
{
    for (uint32_t number = 0; number < image->sb.inode_count; number++) {
        struct inode inode = read_inode(image, number);
        if (inode.type == 0)
            continue;

        for (size_t i = 0; i < DIRECT_COUNT; i++) {
            uint32_t block = inode.addresses[i];
            if (block != 0 && !bitmap_is_set(image, block)) {
                fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
                return -1;
            }
        }

        uint32_t indirect = inode.addresses[DIRECT_COUNT];
        if (indirect == 0)
            continue;
        if (!bitmap_is_set(image, indirect)) {
            fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
            return -1;
        }

        const uint8_t *entries = image->data + (size_t)indirect * BLOCK_SIZE;
        for (size_t i = 0; i < INDIRECT_COUNT; i++) {
            uint32_t block = read_u32(entries + i * sizeof(uint32_t));
            if (block != 0 && !bitmap_is_set(image, block)) {
                fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
                return -1;
            }
        }
    }
    return 0;
}

static int check_bitmap_for_unused_blocks(const struct image *image,
                                          const struct block_state *state)
{
    for (uint32_t block = image->data_start; block < image->sb.size; block++) {
        if (bitmap_is_set(image, block) && !state->used[block]) {
            fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
            return -1;
        }
    }
    return 0;
}

static int check_duplicate_blocks(const struct image *image,
                                  const struct block_state *state)
{
    for (uint32_t block = image->data_start; block < image->sb.size; block++) {
        if (state->direct_refs[block] > 1
            || (state->direct_refs[block] > 0 && state->indirect_refs[block] > 0)) {
            fprintf(stderr, "ERROR: direct address used more than once.\n");
            return -1;
        }
    }
    for (uint32_t block = image->data_start; block < image->sb.size; block++) {
        if (state->indirect_refs[block] > 1) {
            fprintf(stderr, "ERROR: indirect address used more than once.\n");
            return -1;
        }
    }
    return 0;
}

static int load_image(struct image *image, const char *path)
{
    struct stat file_stat;
    uint32_t inode_blocks;
    uint32_t bitmap_blocks;
    uint64_t inode_end;
    uint64_t bitmap_end;

    image->fd = open(path, O_RDONLY);
    if (image->fd < 0) {
        fprintf(stderr, "image not found.\n");
        return -1;
    }

    if (fstat(image->fd, &file_stat) < 0 || file_stat.st_size < 2 * BLOCK_SIZE
        || file_stat.st_size % BLOCK_SIZE != 0) {
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    image->length = (size_t)file_stat.st_size;
    image->data = mmap(NULL, image->length, PROT_READ, MAP_PRIVATE, image->fd, 0);
    if (image->data == MAP_FAILED) {
        image->data = NULL;
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    const uint8_t *sb = image->data + SUPERBLOCK_OFFSET;
    image->sb.size = read_u32(sb);
    image->sb.data_blocks = read_u32(sb + 4);
    image->sb.inode_count = read_u32(sb + 8);
    image->sb.log_count = read_u32(sb + 12);
    image->sb.log_start = read_u32(sb + 16);
    image->sb.inode_start = read_u32(sb + 20);
    image->sb.bitmap_start = read_u32(sb + 24);

    if (image->sb.size != image->length / BLOCK_SIZE
        || image->sb.inode_count < 2
        || image->sb.data_blocks == 0
        || image->sb.data_blocks >= image->sb.size) {
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    image->data_start = image->sb.size - image->sb.data_blocks;
    inode_blocks = (image->sb.inode_count + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
    bitmap_blocks = (image->sb.size + BLOCK_BITS - 1) / BLOCK_BITS;
    inode_end = (uint64_t)image->sb.inode_start + inode_blocks;
    bitmap_end = (uint64_t)image->sb.bitmap_start + bitmap_blocks;

    if (image->sb.inode_start < 2
        || inode_end > image->sb.bitmap_start
        || bitmap_end != image->data_start
        || (image->sb.log_count != 0
            && ((uint64_t)image->sb.log_start + image->sb.log_count > image->sb.inode_start
                || image->sb.log_start < 2))) {
        fprintf(stderr, "%s\n", INVALID_IMAGE);
        return -1;
    }

    return 0;
}

static void close_image(struct image *image)
{
    if (image->data != NULL)
        munmap(image->data, image->length);
    if (image->fd >= 0)
        close(image->fd);
}

int main(int argc, char **argv)
{
    struct image image = { .fd = -1, .data = NULL, .length = 0 };
    struct block_state blocks = { .used = NULL, .direct_refs = NULL, .indirect_refs = NULL };

    if (argc != 2) {
        fprintf(stderr, "Usage: xcheck <file_system_image>\n");
        return 1;
    }

    if (load_image(&image, argv[1]) < 0) {
        close_image(&image);
        return 1;
    }

    if (check_inode_types(&image) < 0
        || check_inode_addresses(&image) < 0
        || check_root(&image) < 0
        || init_block_state(&image, &blocks) < 0) {
        free_block_state(&blocks);
        close_image(&image);
        return 1;
    }

    collect_block_references(&image, &blocks);
    if (check_bitmap_for_references(&image) < 0
        || check_bitmap_for_unused_blocks(&image, &blocks) < 0
        || check_duplicate_blocks(&image, &blocks) < 0) {
        free_block_state(&blocks);
        close_image(&image);
        return 1;
    }

    free_block_state(&blocks);
    close_image(&image);
    return 0;
}
