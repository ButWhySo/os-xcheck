#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

#define INVALID_IMAGE "ERROR: invalid file system image."

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

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
        | (uint32_t)p[1] << 8
        | (uint32_t)p[2] << 16
        | (uint32_t)p[3] << 24;
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
    int result;

    if (argc != 2) {
        fprintf(stderr, "Usage: xcheck <file_system_image>\n");
        return 1;
    }

    if (load_image(&image, argv[1]) < 0) {
        close_image(&image);
        return 1;
    }

    result = 0;
    close_image(&image);
    return result;
}
