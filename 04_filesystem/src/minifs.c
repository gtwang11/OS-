#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MINIFS_MAGIC 0x3146534dU /* MSF1 */
#define MAX_FILES 64
#define NAME_LEN 64
#define DIRECT_BLOCKS 12
#define DEFAULT_BLOCK_SIZE 512
#define DEFAULT_BLOCKS 256

typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t data_start;
    uint32_t max_files;
    uint32_t direct_blocks;
} SuperBlock;

typedef struct {
    uint8_t used;
    char name[NAME_LEN];
    uint32_t size;
    uint32_t block_count;
    uint32_t blocks[DIRECT_BLOCKS];
} Inode;

typedef struct {
    FILE *fp;
    const char *path;
    SuperBlock sb;
    uint8_t *bitmap;
    Inode inodes[MAX_FILES];
} FileSystem;

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void die_errno(const char *msg) {
    fprintf(stderr, "error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static uint32_t ceil_div_u32(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

static long bitmap_offset(void) {
    return (long)sizeof(SuperBlock);
}

static long inode_offset(const SuperBlock *sb) {
    return bitmap_offset() + (long)sb->block_count;
}

static long block_offset(const SuperBlock *sb, uint32_t block) {
    return (long)block * (long)sb->block_size;
}

static void normalize_name(const char *input, char out[NAME_LEN]) {
    const char *p = input;
    if (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        die("empty file name; miniFS uses files under root, e.g. /hello.txt");
    }
    if (strchr(p, '/')) {
        die("nested directories are intentionally not supported by this root-directory miniFS");
    }
    if (strlen(p) >= NAME_LEN) {
        die("file name too long");
    }
    snprintf(out, NAME_LEN, "%s", p);
}

static void fs_open(FileSystem *fs, const char *path, const char *mode) {
    memset(fs, 0, sizeof(*fs));
    fs->path = path;
    fs->fp = fopen(path, mode);
    if (!fs->fp) {
        die_errno(path);
    }
    if (fread(&fs->sb, sizeof(fs->sb), 1, fs->fp) != 1) {
        die("failed to read superblock");
    }
    if (fs->sb.magic != MINIFS_MAGIC ||
        fs->sb.max_files != MAX_FILES ||
        fs->sb.direct_blocks != DIRECT_BLOCKS ||
        fs->sb.block_size == 0 ||
        fs->sb.block_count == 0 ||
        fs->sb.data_start >= fs->sb.block_count) {
        die("invalid miniFS image; run format first");
    }
    fs->bitmap = calloc(fs->sb.block_count, 1);
    if (!fs->bitmap) {
        die_errno("calloc bitmap");
    }
    if (fseek(fs->fp, bitmap_offset(), SEEK_SET) != 0) {
        die_errno("seek bitmap");
    }
    if (fread(fs->bitmap, 1, fs->sb.block_count, fs->fp) != fs->sb.block_count) {
        die("failed to read bitmap");
    }
    if (fseek(fs->fp, inode_offset(&fs->sb), SEEK_SET) != 0) {
        die_errno("seek inodes");
    }
    if (fread(fs->inodes, sizeof(Inode), MAX_FILES, fs->fp) != MAX_FILES) {
        die("failed to read inode table");
    }
}

static void fs_sync(FileSystem *fs) {
    if (fseek(fs->fp, bitmap_offset(), SEEK_SET) != 0) {
        die_errno("seek bitmap");
    }
    if (fwrite(fs->bitmap, 1, fs->sb.block_count, fs->fp) != fs->sb.block_count) {
        die("failed to write bitmap");
    }
    if (fseek(fs->fp, inode_offset(&fs->sb), SEEK_SET) != 0) {
        die_errno("seek inodes");
    }
    if (fwrite(fs->inodes, sizeof(Inode), MAX_FILES, fs->fp) != MAX_FILES) {
        die("failed to write inode table");
    }
    fflush(fs->fp);
}

static void fs_close(FileSystem *fs) {
    if (fs->fp) {
        fclose(fs->fp);
    }
    free(fs->bitmap);
}

static int find_inode(const FileSystem *fs, const char *name) {
    for (int i = 0; i < MAX_FILES; ++i) {
        if (fs->inodes[i].used && strcmp(fs->inodes[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int free_inode_index(const FileSystem *fs) {
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!fs->inodes[i].used) {
            return i;
        }
    }
    return -1;
}

static void free_inode_blocks(FileSystem *fs, Inode *inode) {
    for (uint32_t i = 0; i < inode->block_count; ++i) {
        uint32_t b = inode->blocks[i];
        if (b >= fs->sb.data_start && b < fs->sb.block_count) {
            fs->bitmap[b] = 0;
        }
    }
    inode->block_count = 0;
}

static int count_free_data_blocks(const FileSystem *fs) {
    int count = 0;
    for (uint32_t b = fs->sb.data_start; b < fs->sb.block_count; ++b) {
        if (!fs->bitmap[b]) {
            count++;
        }
    }
    return count;
}

static int allocate_blocks(FileSystem *fs, uint32_t needed, uint32_t out[DIRECT_BLOCKS]) {
    uint32_t got = 0;
    for (uint32_t b = fs->sb.data_start; b < fs->sb.block_count && got < needed; ++b) {
        if (!fs->bitmap[b]) {
            fs->bitmap[b] = 1;
            out[got++] = b;
        }
    }
    if (got != needed) {
        for (uint32_t i = 0; i < got; ++i) {
            fs->bitmap[out[i]] = 0;
        }
        return 0;
    }
    return 1;
}

static void write_inode_data(FileSystem *fs, Inode *inode, const uint8_t *data, uint32_t size) {
    uint8_t *block = calloc(fs->sb.block_size, 1);
    if (!block) {
        die_errno("calloc block");
    }
    uint32_t written = 0;
    for (uint32_t i = 0; i < inode->block_count; ++i) {
        memset(block, 0, fs->sb.block_size);
        uint32_t chunk = size - written;
        if (chunk > fs->sb.block_size) {
            chunk = fs->sb.block_size;
        }
        memcpy(block, data + written, chunk);
        if (fseek(fs->fp, block_offset(&fs->sb, inode->blocks[i]), SEEK_SET) != 0) {
            die_errno("seek data block");
        }
        if (fwrite(block, 1, fs->sb.block_size, fs->fp) != fs->sb.block_size) {
            die("failed to write data block");
        }
        written += chunk;
    }
    free(block);
}

static uint8_t *read_inode_data(FileSystem *fs, const Inode *inode) {
    uint8_t *data = calloc((size_t)inode->size + 1, 1);
    if (!data) {
        die_errno("calloc read buffer");
    }
    uint32_t read_total = 0;
    for (uint32_t i = 0; i < inode->block_count; ++i) {
        uint32_t chunk = inode->size - read_total;
        if (chunk > fs->sb.block_size) {
            chunk = fs->sb.block_size;
        }
        if (fseek(fs->fp, block_offset(&fs->sb, inode->blocks[i]), SEEK_SET) != 0) {
            die_errno("seek data block");
        }
        if (fread(data + read_total, 1, chunk, fs->fp) != chunk) {
            die("failed to read data block");
        }
        read_total += chunk;
    }
    return data;
}

static void cmd_format(const char *image, uint32_t blocks, uint32_t block_size) {
    if (blocks < 16 || block_size < 128) {
        die("format requires at least 16 blocks and block size >=128");
    }
    SuperBlock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = MINIFS_MAGIC;
    sb.block_size = block_size;
    sb.block_count = blocks;
    sb.max_files = MAX_FILES;
    sb.direct_blocks = DIRECT_BLOCKS;
    uint32_t metadata_bytes = (uint32_t)sizeof(SuperBlock) + blocks + (uint32_t)sizeof(Inode) * MAX_FILES;
    sb.data_start = ceil_div_u32(metadata_bytes, block_size);
    if (sb.data_start >= blocks) {
        die("image too small for metadata");
    }

    FILE *fp = fopen(image, "wb+");
    if (!fp) {
        die_errno(image);
    }
    long image_size = (long)blocks * (long)block_size;
    if (ftruncate(fileno(fp), image_size) != 0) {
        die_errno("ftruncate image");
    }

    uint8_t *bitmap = calloc(blocks, 1);
    Inode empty[MAX_FILES];
    memset(empty, 0, sizeof(empty));
    if (!bitmap) {
        die_errno("calloc bitmap");
    }
    for (uint32_t b = 0; b < sb.data_start; ++b) {
        bitmap[b] = 1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0 ||
        fwrite(&sb, sizeof(sb), 1, fp) != 1 ||
        fwrite(bitmap, 1, blocks, fp) != blocks ||
        fwrite(empty, sizeof(Inode), MAX_FILES, fp) != MAX_FILES) {
        die("failed to initialize image");
    }
    fclose(fp);
    free(bitmap);
    printf("FORMAT image=%s blocks=%u block_size=%u data_start=%u data_blocks=%u max_file_bytes=%u\n",
           image, blocks, block_size, sb.data_start, blocks - sb.data_start, block_size * DIRECT_BLOCKS);
}

static void cmd_create(const char *image, const char *path) {
    char name[NAME_LEN];
    normalize_name(path, name);
    FileSystem fs;
    fs_open(&fs, image, "rb+");
    if (find_inode(&fs, name) >= 0) {
        die("file already exists");
    }
    int idx = free_inode_index(&fs);
    if (idx < 0) {
        die("inode table full");
    }
    memset(&fs.inodes[idx], 0, sizeof(fs.inodes[idx]));
    fs.inodes[idx].used = 1;
    snprintf(fs.inodes[idx].name, sizeof(fs.inodes[idx].name), "%s", name);
    fs_sync(&fs);
    fs_close(&fs);
    printf("CREATE %s -> OK inode=%d\n", name, idx);
}

static void write_bytes_to_file(const char *image, const char *path, const uint8_t *data, uint32_t size) {
    char name[NAME_LEN];
    normalize_name(path, name);
    FileSystem fs;
    fs_open(&fs, image, "rb+");
    int idx = find_inode(&fs, name);
    if (idx < 0) {
        fs_close(&fs);
        die("file not found; create it first");
    }
    Inode *inode = &fs.inodes[idx];
    uint32_t needed = ceil_div_u32(size, fs.sb.block_size);
    if (needed > DIRECT_BLOCKS) {
        fs_close(&fs);
        die("file exceeds direct block limit");
    }
    int reusable = (int)inode->block_count;
    if (count_free_data_blocks(&fs) + reusable < (int)needed) {
        fs_close(&fs);
        die("not enough free data blocks");
    }
    free_inode_blocks(&fs, inode);
    uint32_t new_blocks[DIRECT_BLOCKS] = {0};
    if (needed > 0 && !allocate_blocks(&fs, needed, new_blocks)) {
        fs_close(&fs);
        die("allocation failed unexpectedly");
    }
    inode->size = size;
    inode->block_count = needed;
    for (uint32_t i = 0; i < needed; ++i) {
        inode->blocks[i] = new_blocks[i];
    }
    write_inode_data(&fs, inode, data, size);
    fs_sync(&fs);
    fs_close(&fs);
    printf("WRITE %s -> OK bytes=%u blocks=%u\n", name, size, needed);
}

static void cmd_write_text(const char *image, const char *path, const char *text) {
    write_bytes_to_file(image, path, (const uint8_t *)text, (uint32_t)strlen(text));
}

static uint8_t *read_host_file(const char *path, uint32_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        die_errno(path);
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        die_errno("seek host file");
    }
    long len = ftell(fp);
    if (len < 0) {
        die_errno("ftell host file");
    }
    rewind(fp);
    uint8_t *data = malloc((size_t)len + 1);
    if (!data) {
        die_errno("malloc host data");
    }
    if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
        die("failed to read host file");
    }
    fclose(fp);
    data[len] = 0;
    *size = (uint32_t)len;
    return data;
}

static void cmd_writefile(const char *image, const char *path, const char *host_path) {
    uint32_t size = 0;
    uint8_t *data = read_host_file(host_path, &size);
    write_bytes_to_file(image, path, data, size);
    free(data);
}

static void cmd_append(const char *image, const char *path, const char *text) {
    char name[NAME_LEN];
    normalize_name(path, name);
    FileSystem fs;
    fs_open(&fs, image, "rb+");
    int idx = find_inode(&fs, name);
    if (idx < 0) {
        fs_close(&fs);
        die("file not found; create it first");
    }
    Inode old = fs.inodes[idx];
    uint8_t *old_data = read_inode_data(&fs, &old);
    fs_close(&fs);

    uint32_t add = (uint32_t)strlen(text);
    uint32_t total = old.size + add;
    uint8_t *combined = malloc((size_t)total + 1);
    if (!combined) {
        die_errno("malloc append");
    }
    memcpy(combined, old_data, old.size);
    memcpy(combined + old.size, text, add);
    combined[total] = 0;
    free(old_data);
    write_bytes_to_file(image, path, combined, total);
    free(combined);
    printf("APPEND %s -> OK appended=%u\n", name, add);
}

static void cmd_read(const char *image, const char *path) {
    char name[NAME_LEN];
    normalize_name(path, name);
    FileSystem fs;
    fs_open(&fs, image, "rb");
    int idx = find_inode(&fs, name);
    if (idx < 0) {
        fs_close(&fs);
        die("file not found");
    }
    uint8_t *data = read_inode_data(&fs, &fs.inodes[idx]);
    printf("READ %s bytes=%u\n", name, fs.inodes[idx].size);
    fwrite(data, 1, fs.inodes[idx].size, stdout);
    printf("\n");
    free(data);
    fs_close(&fs);
}

static void cmd_readto(const char *image, const char *path, const char *host_path) {
    char name[NAME_LEN];
    normalize_name(path, name);
    FileSystem fs;
    fs_open(&fs, image, "rb");
    int idx = find_inode(&fs, name);
    if (idx < 0) {
        fs_close(&fs);
        die("file not found");
    }
    uint8_t *data = read_inode_data(&fs, &fs.inodes[idx]);
    FILE *out = fopen(host_path, "wb");
    if (!out) {
        die_errno(host_path);
    }
    if (fwrite(data, 1, fs.inodes[idx].size, out) != fs.inodes[idx].size) {
        die("failed to write host output");
    }
    fclose(out);
    printf("READTO %s -> %s bytes=%u\n", name, host_path, fs.inodes[idx].size);
    free(data);
    fs_close(&fs);
}

static void cmd_delete(const char *image, const char *path) {
    char name[NAME_LEN];
    normalize_name(path, name);
    FileSystem fs;
    fs_open(&fs, image, "rb+");
    int idx = find_inode(&fs, name);
    if (idx < 0) {
        fs_close(&fs);
        die("file not found");
    }
    free_inode_blocks(&fs, &fs.inodes[idx]);
    memset(&fs.inodes[idx], 0, sizeof(fs.inodes[idx]));
    fs_sync(&fs);
    fs_close(&fs);
    printf("DELETE %s -> OK freed_inode=%d\n", name, idx);
}

static void cmd_ls(const char *image) {
    FileSystem fs;
    fs_open(&fs, image, "rb");
    printf("LS image=%s\n", image);
    printf("name size blocks\n");
    for (int i = 0; i < MAX_FILES; ++i) {
        if (fs.inodes[i].used) {
            printf("%s %u %u\n", fs.inodes[i].name, fs.inodes[i].size, fs.inodes[i].block_count);
        }
    }
    fs_close(&fs);
}

static void cmd_stat(const char *image) {
    FileSystem fs;
    fs_open(&fs, image, "rb");
    int used_files = 0;
    int used_data = 0;
    for (int i = 0; i < MAX_FILES; ++i) {
        if (fs.inodes[i].used) {
            used_files++;
        }
    }
    for (uint32_t b = fs.sb.data_start; b < fs.sb.block_count; ++b) {
        if (fs.bitmap[b]) {
            used_data++;
        }
    }
    printf("STAT image=%s block_size=%u blocks=%u data_start=%u\n",
           image, fs.sb.block_size, fs.sb.block_count, fs.sb.data_start);
    printf("SUMMARY files=%d/%d data_used=%d data_free=%d metadata_blocks=%u max_file_bytes=%u\n",
           used_files, MAX_FILES, used_data, (int)(fs.sb.block_count - fs.sb.data_start) - used_data,
           fs.sb.data_start, fs.sb.block_size * DIRECT_BLOCKS);
    printf("ANALYSIS bitmap中1表示已占用块，0表示空闲块；删除文件会释放直接块并更新空闲空间。\n");
    fs_close(&fs);
}

static void cmd_bitmap(const char *image) {
    FileSystem fs;
    fs_open(&fs, image, "rb");
    printf("BITMAP image=%s used=1 free=0\n", image);
    for (uint32_t b = 0; b < fs.sb.block_count; ++b) {
        putchar(fs.bitmap[b] ? '1' : '0');
        if ((b + 1) % 64 == 0 || b + 1 == fs.sb.block_count) {
            putchar('\n');
        }
    }
    fs_close(&fs);
}

static uint32_t parse_u32(const char *s) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s[0] || *end || v > UINT32_MAX) {
        die("invalid number");
    }
    return (uint32_t)v;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s format IMAGE [--blocks N] [--block-size N]\n"
            "  %s create IMAGE /name\n"
            "  %s write IMAGE /name \"text\"\n"
            "  %s append IMAGE /name \"text\"\n"
            "  %s writefile IMAGE /name HOST_FILE\n"
            "  %s read IMAGE /name\n"
            "  %s readto IMAGE /name HOST_FILE\n"
            "  %s delete IMAGE /name\n"
            "  %s ls|stat|bitmap IMAGE\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "format") == 0) {
        uint32_t blocks = DEFAULT_BLOCKS;
        uint32_t block_size = DEFAULT_BLOCK_SIZE;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
                blocks = parse_u32(argv[++i]);
            } else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
                block_size = parse_u32(argv[++i]);
            } else {
                usage(argv[0]);
                return 1;
            }
        }
        cmd_format(argv[2], blocks, block_size);
    } else if (strcmp(cmd, "create") == 0 && argc == 4) {
        cmd_create(argv[2], argv[3]);
    } else if (strcmp(cmd, "write") == 0 && argc == 5) {
        cmd_write_text(argv[2], argv[3], argv[4]);
    } else if (strcmp(cmd, "append") == 0 && argc == 5) {
        cmd_append(argv[2], argv[3], argv[4]);
    } else if (strcmp(cmd, "writefile") == 0 && argc == 5) {
        cmd_writefile(argv[2], argv[3], argv[4]);
    } else if (strcmp(cmd, "read") == 0 && argc == 4) {
        cmd_read(argv[2], argv[3]);
    } else if (strcmp(cmd, "readto") == 0 && argc == 5) {
        cmd_readto(argv[2], argv[3], argv[4]);
    } else if (strcmp(cmd, "delete") == 0 && argc == 4) {
        cmd_delete(argv[2], argv[3]);
    } else if (strcmp(cmd, "ls") == 0 && argc == 3) {
        cmd_ls(argv[2]);
    } else if (strcmp(cmd, "stat") == 0 && argc == 3) {
        cmd_stat(argv[2]);
    } else if (strcmp(cmd, "bitmap") == 0 && argc == 3) {
        cmd_bitmap(argv[2]);
    } else {
        usage(argv[0]);
        return 1;
    }
    return 0;
}
