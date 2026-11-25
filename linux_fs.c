#include <stddef.h>
#include <stdint.h>
#include "ext4.h"
#include "linux_fs.h"
#include "memory.h"

#define LINUX_EBADF 9
#define LINUX_EFAULT 14
#define LINUX_EINVAL 22
#define LINUX_ENOENT 2
#define LINUX_ENOMEM 12
#define LINUX_ENAMETOOLONG 36
#define LINUX_ERANGE 34
#define LINUX_EIO 5
#define LINUX_EISDIR 21
#define LINUX_ENOTDIR 20
#define FD_FIRST 3
#define FD_COUNT 32
#define FILE_PAGE_COUNT 1024u
#define FILE_MAX_SIZE (FILE_PAGE_COUNT * 4096u)
#define FILE_BLOCK_SIZE 1024u
#define O_ACCMODE 3u
#define O_WRONLY 1u
#define O_RDWR 2u
#define O_CREAT 0x40u
#define O_EXCL 0x80u
#define O_TRUNC 0x200u
#define O_APPEND 0x400u
#define O_DIRECTORY 0x10000u

typedef struct {
    uint8_t used, readable, writable, modified, directory;
    uint32_t flags;
    uint32_t size, position, page_count, inode;
    uint64_t pages[FILE_PAGE_COUNT];
    char path[128];
} linux_file_t;

typedef struct {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev, st_size, st_blksize, st_blocks;
    int64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec;
    int64_t ctime_sec, ctime_nsec, reserved[3];
} linux_stat_t;

typedef struct {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, spare0;
    uint64_t ino, size, blocks, attributes_mask;
    struct { int64_t seconds; uint32_t nanoseconds, reserved; } atime, btime, mtime, ctime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t mount_id;
    uint32_t dio_mem_align, dio_offset_align;
    uint64_t spare[12];
} linux_statx_t;

_Static_assert(sizeof(linux_statx_t) == 256, "Linux statx ABI must be 256 bytes");

static linux_file_t files[FD_COUNT];

static int copy_user_string(uint64_t source, char *destination, size_t capacity)
{
    for (size_t i = 0; i < capacity; ++i) {
        if (!memory_copy_from_user(&destination[i], source + i, 1)) return -LINUX_EFAULT;
        if (!destination[i]) return 0;
    }
    return -LINUX_ENAMETOOLONG;
}

static int split_parent(const char *path, char *parent, char *leaf)
{
    char work[128];
    size_t length = 0;
    while (path[length] && length < sizeof(work) - 1) { work[length] = path[length]; ++length; }
    if (!length || path[length]) return 0;
    work[length] = 0;
    char *slash = 0;
    for (size_t i = 0; i < length; ++i) if (work[i] == '/') slash = &work[i];
    char *name = slash ? slash + 1 : work;
    size_t name_length = 0;
    while (name[name_length]) ++name_length;
    if (!name_length || name_length > 63 || (name_length == 1 && name[0] == '.') ||
        (name_length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    for (size_t i = 0; i <= name_length; ++i) leaf[i] = name[i];
    if (slash) {
        size_t parent_length = slash == work ? 1 : (size_t)(slash - work);
        if (parent_length == 1 && slash == work) parent[0] = '/';
        else for (size_t i = 0; i < parent_length; ++i) parent[i] = work[i];
        parent[parent_length] = 0;
    } else {
        if (!ext4_pwd(parent, 128)) return 0;
    }
    return 1;
}

static int path_enter(const char *path, char *old_directory, char *basename)
{
    if (!path || !*path || !ext4_pwd(old_directory, 256)) return 0;
    size_t length = 0;
    while (path[length] && length < 127) ++length;
    if (!length || path[length]) return 0;
    char work[128];
    for (size_t i = 0; i <= length; ++i) work[i] = path[i];
    char *slash = 0;
    for (size_t i = 0; i < length; ++i) if (work[i] == '/') slash = &work[i];
    char *leaf = slash ? slash + 1 : work;
    if (!*leaf || (*leaf == '.' && !leaf[1]) ||
        (*leaf == '.' && leaf[1] == '.' && !leaf[2])) return 0;
    size_t leaf_length = 0;
    while (leaf[leaf_length]) ++leaf_length;
    if (leaf_length > 63) return 0;
    for (size_t i = 0; i <= leaf_length; ++i) basename[i] = leaf[i];
    if (slash) {
        if (slash == work) slash[1] = 0;
        else *slash = 0;
        if (!ext4_chdir(work)) return 0;
    }
    return 1;
}

static void path_leave(const char *old_directory)
{
    ext4_chdir(old_directory);
}

static int reserve_pages(linux_file_t *file, uint32_t required)
{
    if (required > FILE_PAGE_COUNT) return 0;
    while (file->page_count < required) {
        uint64_t page = memory_alloc_page();
        if (!page) return 0;
        file->pages[file->page_count++] = page;
    }
    return 1;
}

static void release_file_pages(linux_file_t *file)
{
    for (uint32_t i = 0; i < file->page_count; ++i)
        memory_free_page(file->pages[i]);
    file->page_count = 0;
}

static void discard_file(linux_file_t *file)
{
    release_file_pages(file);
    for (size_t i = 0; i < sizeof(*file); ++i) ((uint8_t *)file)[i] = 0;
}

static void file_store(linux_file_t *file, uint32_t offset, const uint8_t *data, size_t size)
{
    while (size) {
        uint32_t page_index = offset / 4096u;
        size_t page_offset = offset & 4095u;
        size_t count = 4096u - page_offset;
        if (count > size) count = size;
        uint8_t *destination = (uint8_t *)(uintptr_t)file->pages[page_index] + page_offset;
        for (size_t i = 0; i < count; ++i) destination[i] = data[i];
        offset += (uint32_t)count; data += count; size -= count;
    }
}

static void file_load(linux_file_t *file, uint32_t offset, uint8_t *data, size_t size)
{
    while (size) {
        uint32_t page_index = offset / 4096u;
        size_t page_offset = offset & 4095u;
        size_t count = 4096u - page_offset;
        if (count > size) count = size;
        const uint8_t *source = (const uint8_t *)(uintptr_t)file->pages[page_index] + page_offset;
        for (size_t i = 0; i < count; ++i) data[i] = source[i];
        offset += (uint32_t)count; data += count; size -= count;
    }
}

static int flush_file(linux_file_t *file)
{
    if (!file->used || !file->modified) return 1;
    char old_directory[256], basename[64];
    if (!path_enter(file->path, old_directory, basename)) return 0;
    if (!ext4_write_begin(basename)) { path_leave(old_directory); return 0; }
    uint8_t block[FILE_BLOCK_SIZE];
    uint32_t written = 0;
    uint32_t blocks = (file->size + FILE_BLOCK_SIZE - 1) / FILE_BLOCK_SIZE;
    for (uint32_t i = 0; i < blocks; ++i) {
        size_t count = file->size - i * FILE_BLOCK_SIZE;
        if (count > sizeof(block)) count = sizeof(block);
        file_load(file, i * FILE_BLOCK_SIZE, block, count);
        if (!ext4_write_block(i, block, count)) break;
        written += (uint32_t)count;
    }
    int ok = ext4_write_finish(written) && written == file->size;
    path_leave(old_directory);
    if (ok) file->modified = 0;
    return ok;
}

void linux_fs_process_reset(void)
{
    for (size_t i = 0; i < FD_COUNT; ++i) {
        if (files[i].used) flush_file(&files[i]);
        discard_file(&files[i]);
    }
}

int64_t linux_fs_openat(uint64_t dirfd, uint64_t user_path, uint64_t flags, uint64_t mode)
{
    (void)mode;
    if ((int32_t)dirfd != -100) return -LINUX_EBADF;
    char path[128];
    int copied = copy_user_string(user_path, path, sizeof(path));
    if (copied) return copied;
    char canonical[128];
    if (!ext4_resolve_path(path, canonical, sizeof(canonical), (flags & O_CREAT) != 0))
        return -LINUX_ENOENT;
    char prior_directory[256];
    if (!ext4_pwd(prior_directory, sizeof(prior_directory))) return -LINUX_EIO;
    if (ext4_chdir(canonical)) {
        if (flags & (O_CREAT | O_TRUNC)) { (void)ext4_chdir(prior_directory); return -LINUX_EISDIR; }
        size_t index;
        for (index = 0; index < FD_COUNT && files[index].used; ++index) { }
        if (index == FD_COUNT) { (void)ext4_chdir(prior_directory); return -24; }
        linux_file_t *file = &files[index];
        for (size_t i = 0; i < sizeof(*file); ++i) ((uint8_t *)file)[i] = 0;
        file->used = file->directory = 1;
        file->readable = (flags & O_ACCMODE) != O_WRONLY;
        file->flags = (uint32_t)flags;
        size_t n = 0; while (canonical[n] && n + 1 < sizeof(file->path)) { file->path[n] = canonical[n]; ++n; }
        file->path[n] = 0;
        if (!ext4_inode_number(file->path, &file->inode)) {
            discard_file(file); (void)ext4_chdir(prior_directory); return -LINUX_EIO;
        }
        (void)ext4_chdir(prior_directory);
        return FD_FIRST + (int64_t)index;
    }
    (void)ext4_chdir(prior_directory);
    if (flags & O_DIRECTORY) return -LINUX_ENOTDIR;
    char original_directory[256], basename[64];
    if (!path_enter(canonical, original_directory, basename)) return -LINUX_ENOENT;
    char absolute[128];
    size_t canonical_length = 0;
    while (canonical[canonical_length] && canonical_length < sizeof(absolute) - 1) {
        absolute[canonical_length] = canonical[canonical_length];
        ++canonical_length;
    }
    absolute[canonical_length] = 0;

    uint32_t file_size = 0;
    int exists = ext4_read_start(basename, &file_size);
    if (exists) ext4_read_end();
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
        path_leave(original_directory); return -17;
    }
    if (!exists && !(flags & O_CREAT)) {
        path_leave(original_directory); return -LINUX_ENOENT;
    }
    if (file_size > FILE_MAX_SIZE) {
        path_leave(original_directory); return -LINUX_ENOMEM;
    }
    size_t index;
    for (index = 0; index < FD_COUNT && files[index].used; ++index) { }
    if (index == FD_COUNT) { path_leave(original_directory); return -24; }
    linux_file_t *file = &files[index];
    for (size_t i = 0; i < sizeof(*file); ++i) ((uint8_t *)file)[i] = 0;
    file->used = 1;
    file->readable = (flags & O_ACCMODE) != O_WRONLY;
    file->writable = (flags & O_ACCMODE) != 0;
    file->flags = (uint32_t)flags;
    file->size = file_size;
    for (size_t i = 0; i < sizeof(file->path); ++i) file->path[i] = absolute[i];

    if (!exists) {
        int created = ext4_write(basename, (const uint8_t *)"", 0);
        path_leave(original_directory);
        if (!created) {
            discard_file(file);
            return -LINUX_EIO;
        }
        if (!ext4_inode_number(file->path, &file->inode)) {
            discard_file(file); return -LINUX_EIO;
        }
        return FD_FIRST + (int64_t)index;
    }
    if (!(flags & O_TRUNC) && file_size) {
        uint32_t pages = (file_size + 4095u) / 4096u;
        if (!reserve_pages(file, pages)) {
            path_leave(original_directory);
            discard_file(file);
            return -LINUX_ENOMEM;
        }
        if (!ext4_read_start(basename, &file_size)) {
            path_leave(original_directory);
            discard_file(file);
            return -LINUX_EIO;
        }
        uint8_t block[FILE_BLOCK_SIZE];
        uint32_t offset = 0;
        int ok = 1;
        for (uint32_t i = 0; offset < file_size; ++i) {
            if (!ext4_read_block(i, block)) { ok = 0; break; }
            size_t count = file_size - offset;
            if (count > sizeof(block)) count = sizeof(block);
            file_store(file, offset, block, count);
            offset += (uint32_t)count;
        }
        ext4_read_end();
        if (!ok) {
            path_leave(original_directory);
            discard_file(file);
            return -LINUX_EIO;
        }
    }
    if (flags & O_TRUNC) { file->size = 0; file->modified = 1; }
    if (flags & O_APPEND) file->position = file->size;
    path_leave(original_directory);
    if (!ext4_inode_number(file->path, &file->inode)) {
        discard_file(file); return -LINUX_EIO;
    }
    return FD_FIRST + (int64_t)index;
}

int64_t linux_fs_open(uint64_t user_path, uint64_t flags, uint64_t mode)
{
    return linux_fs_openat((uint64_t)(int64_t)-100, user_path, flags, mode);
}

static linux_file_t *get_file(uint64_t fd)
{
    if (fd < FD_FIRST || fd >= FD_FIRST + FD_COUNT) return 0;
    linux_file_t *file = &files[fd - FD_FIRST];
    return file->used ? file : 0;
}

int linux_fs_fd_valid(uint64_t fd)
{
    return fd < 3 || get_file(fd) != 0;
}

int64_t linux_fs_close(uint64_t fd)
{
    linux_file_t *file = get_file(fd);
    if (!file) return fd < 3 ? 0 : -LINUX_EBADF;
    int ok = flush_file(file);
    discard_file(file);
    return ok ? 0 : -LINUX_EIO;
}

int64_t linux_fs_read(uint64_t fd, uint64_t buffer, uint64_t count,
                      uint64_t offset, int positional)
{
    linux_file_t *file = get_file(fd);
    if (!file || !file->readable) return -LINUX_EBADF;
    if (file->directory) return -LINUX_EISDIR;
    uint64_t position = positional ? offset : file->position;
    if (position >= file->size || !count) return 0;
    uint64_t amount = file->size - position;
    if (amount > count) amount = count;
    uint64_t done = 0;
    uint8_t chunk[256];
    while (done < amount) {
        size_t size = (size_t)(amount - done);
        if (size > sizeof(chunk)) size = sizeof(chunk);
        file_load(file, (uint32_t)(position + done), chunk, size);
        if (!memory_copy_to_user(buffer + done, chunk, size))
            return done ? (int64_t)done : -LINUX_EFAULT;
        done += size;
    }
    if (!positional) file->position += (uint32_t)done;
    return (int64_t)done;
}

int64_t linux_fs_write(uint64_t fd, uint64_t buffer, uint64_t count,
                       uint64_t offset, int positional)
{
    linux_file_t *file = get_file(fd);
    if (!file || !file->writable) return -LINUX_EBADF;
    if (file->directory) return -LINUX_EISDIR;
    if (!count) return 0;
    uint64_t position = positional ? offset : file->position;
    if (!positional && (file->flags & O_APPEND)) position = file->size;
    if (position >= FILE_MAX_SIZE) return -LINUX_ENOMEM;
    uint64_t amount = count;
    if (amount > FILE_MAX_SIZE - position) amount = FILE_MAX_SIZE - position;
    uint32_t required = (uint32_t)((position + amount + 4095u) / 4096u);
    if (!reserve_pages(file, required)) return -LINUX_ENOMEM;
    uint64_t done = 0;
    uint8_t chunk[256];
    while (done < amount) {
        size_t size = (size_t)(amount - done);
        if (size > sizeof(chunk)) size = sizeof(chunk);
        if (!memory_copy_from_user(chunk, buffer + done, size))
            return done ? (int64_t)done : -LINUX_EFAULT;
        file_store(file, (uint32_t)(position + done), chunk, size);
        done += size;
    }
    if (position + done > file->size) file->size = (uint32_t)(position + done);
    if (!positional) file->position = (uint32_t)(position + done);
    file->modified = 1;
    return (int64_t)done;
}

int64_t linux_fs_seek(uint64_t fd, int64_t offset, uint64_t whence)
{
    linux_file_t *file = get_file(fd);
    if (!file) return -LINUX_EBADF;
    if (file->directory) {
        if (whence != 0 || offset < 0 || offset > 0x7fffffffu) return -LINUX_EINVAL;
        file->position = (uint32_t)offset;
        return offset;
    }
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)file->position :
                   whence == 2 ? (int64_t)file->size : -1;
    if (base < 0 || offset < -base || offset > (int64_t)FILE_MAX_SIZE - base)
        return -LINUX_EINVAL;
    file->position = (uint32_t)(base + offset);
    return file->position;
}

int64_t linux_fs_fstat(uint64_t fd, uint64_t user_stat)
{
    if (fd < 3) {
        linux_stat_t stat = {0};
        stat.st_dev = 1; stat.st_ino = fd + 1; stat.st_nlink = 1;
        stat.st_mode = 0020600; /* console descriptors are character terminals */
        stat.st_rdev = 0x880; stat.st_blksize = FILE_BLOCK_SIZE;
        return memory_copy_to_user(user_stat, &stat, sizeof(stat)) ? 0 : -LINUX_EFAULT;
    }
    linux_file_t *file = get_file(fd);
    if (!file) return -LINUX_EBADF;
    linux_stat_t stat = {0};
    stat.st_dev = 1; stat.st_ino = file->inode; stat.st_nlink = 1;
    stat.st_mode = file->directory ? 0040755 : 0100644; stat.st_size = file->size;
    stat.st_blksize = FILE_BLOCK_SIZE; stat.st_blocks = (file->size + 511u) / 512u;
    return memory_copy_to_user(user_stat, &stat, sizeof(stat)) ? 0 : -LINUX_EFAULT;
}

typedef struct __attribute__((packed)) {
    uint64_t ino;
    int64_t offset;
    uint16_t record_length;
    uint8_t type;
    char name[];
} linux_dirent64_t;

int64_t linux_fs_getdents64(uint64_t fd, uint64_t user_buffer, uint64_t count)
{
    linux_file_t *file = get_file(fd);
    if (!file) return -LINUX_EBADF;
    if (!file->directory) return -LINUX_ENOTDIR;
    char old_directory[256];
    if (!ext4_pwd(old_directory, sizeof(old_directory))) return -LINUX_EIO;
    if (!ext4_chdir(file->path)) return -LINUX_ENOENT;
    ext4_entry_t entries[128];
    int entry_count = ext4_list(entries, sizeof(entries) / sizeof(entries[0]));
    (void)ext4_chdir(old_directory);
    if (entry_count < 0) return -LINUX_EIO;
    if (file->position >= (uint32_t)entry_count) return 0;
    if (file->position >= 128) return -75;
    uint8_t record[128];
    uint64_t written = 0;
    while (file->position < (uint32_t)entry_count && file->position < 128) {
        ext4_entry_t *entry = &entries[file->position];
        size_t name_length = 0; while (entry->name[name_length]) ++name_length;
        size_t record_length = (size_t)((offsetof(linux_dirent64_t, name) + name_length + 1 + 7) & ~7u);
        if (record_length > sizeof(record)) return written ? (int64_t)written : -LINUX_EINVAL;
        if (record_length > count - written) break;
        for (size_t i = 0; i < record_length; ++i) record[i] = 0;
        linux_dirent64_t *dirent = (linux_dirent64_t *)record;
        dirent->ino = (uint64_t)file->position + 2;
        dirent->offset = (int64_t)file->position + 1;
        dirent->record_length = (uint16_t)record_length;
        dirent->type = entry->directory ? 4 : 8;
        for (size_t i = 0; i <= name_length; ++i) dirent->name[i] = entry->name[i];
        if (!memory_copy_to_user(user_buffer + written, record, record_length))
            return written ? (int64_t)written : -LINUX_EFAULT;
        written += record_length;
        file->position++;
    }
    if (!written && count) return -LINUX_EINVAL;
    return (int64_t)written;
}

int64_t linux_fs_stat_path(uint64_t user_path, uint64_t user_stat)
{
    int64_t fd = linux_fs_open(user_path, 0, 0);
    if (fd < 0) return fd;
    int64_t result = linux_fs_fstat((uint64_t)fd, user_stat);
    int64_t close_result = linux_fs_close((uint64_t)fd);
    return result ? result : close_result;
}

int64_t linux_fs_statx_path(uint64_t user_path, uint64_t user_statx)
{
    int64_t fd = linux_fs_open(user_path, 0, 0);
    if (fd < 0) return fd;
    linux_file_t *file = get_file((uint64_t)fd);
    if (!file) { (void)linux_fs_close((uint64_t)fd); return -LINUX_EBADF; }
    linux_statx_t statx = {0};
    statx.mask = 0x7ffu; /* STATX_BASIC_STATS */
    statx.blksize = FILE_BLOCK_SIZE;
    statx.nlink = 1;
    statx.mode = file->directory ? 0040755 : 0100644;
    statx.ino = file->inode;
    statx.size = file->size;
    statx.blocks = (file->size + 511u) / 512u;
    statx.dev_minor = 1; /* same device number as st_dev = 1 in fstat */
    int copied = memory_copy_to_user(user_statx, &statx, sizeof(statx));
    int64_t closed = linux_fs_close((uint64_t)fd);
    return copied ? closed : -LINUX_EFAULT;
}

int64_t linux_fs_access(uint64_t user_path)
{
    int64_t fd = linux_fs_open(user_path, 0, 0);
    return fd < 0 ? fd : linux_fs_close((uint64_t)fd);
}

int64_t linux_fs_fcntl(uint64_t fd, uint64_t command, uint64_t argument)
{
    (void)argument;
    linux_file_t *file = get_file(fd);
    if (!file) return fd < 3 ? 0 : -LINUX_EBADF;
    if (command == 1 || command == 2) return 0; /* F_GETFD/F_SETFD */
    if (command == 3) return (file->readable ? 0 : O_WRONLY) |
                                (file->writable ? O_RDWR : 0); /* F_GETFL */
    if (command == 4) return 0; /* F_SETFL */
    return -LINUX_EINVAL;
}

int64_t linux_fs_fsync(uint64_t fd)
{
    linux_file_t *file = get_file(fd);
    if (!file) return fd < 3 ? 0 : -LINUX_EBADF;
    return flush_file(file) ? 0 : -LINUX_EIO;
}

int64_t linux_fs_ftruncate(uint64_t fd, int64_t length)
{
    linux_file_t *file = get_file(fd);
    if (!file || !file->writable) return -LINUX_EBADF;
    if (length < 0 || (uint64_t)length > FILE_MAX_SIZE) return -LINUX_EINVAL;
    uint32_t new_size = (uint32_t)length;
    uint32_t first_changed = new_size < file->size ? new_size : file->size;
    uint32_t last_changed = new_size > file->size ? new_size : file->size;
    uint32_t required = (new_size + 4095u) / 4096u;
    if (!reserve_pages(file, required)) return -LINUX_ENOMEM;
    for (uint32_t i = required; i < file->page_count; ++i)
        memory_free_page(file->pages[i]);
    if (required < file->page_count) file->page_count = required;
    uint8_t zeros[256] = {0};
    while (first_changed < last_changed) {
        size_t count = last_changed - first_changed;
        if (count > sizeof(zeros)) count = sizeof(zeros);
        file_store(file, first_changed, zeros, count);
        first_changed += (uint32_t)count;
    }
    file->size = new_size;
    file->modified = 1;
    return 0;
}

static int resolve_parent(uint64_t user_path, char *old_directory,
                          char *resolved_parent, char *leaf)
{
    char input[128], parent[128];
    int copied = copy_user_string(user_path, input, sizeof(input));
    if (copied || !split_parent(input, parent, leaf) ||
        !ext4_pwd(old_directory, 256) ||
        !ext4_resolve_path(parent, resolved_parent, 128, 0)) return 0;
    return 1;
}

int64_t linux_fs_unlink(uint64_t user_path, int directory)
{
    char old_directory[256], parent[128], leaf[64];
    if (!resolve_parent(user_path, old_directory, parent, leaf)) return -LINUX_ENOENT;
    if (!ext4_chdir(parent)) { ext4_chdir(old_directory); return -LINUX_ENOENT; }
    int ok = directory ? ext4_rmdir(leaf) : ext4_unlink(leaf);
    ext4_chdir(old_directory);
    return ok ? 0 : -LINUX_EIO;
}

int64_t linux_fs_rename(uint64_t old_user_path, uint64_t new_user_path)
{
    char old_directory[256], old_parent[128], old_leaf[64];
    char new_parent[128], new_leaf[64];
    if (!resolve_parent(old_user_path, old_directory, old_parent, old_leaf) ||
        !resolve_parent(new_user_path, old_directory, new_parent, new_leaf))
        return -LINUX_ENOENT;
    size_t i = 0;
    while (old_parent[i] && old_parent[i] == new_parent[i]) ++i;
    if (old_parent[i] != new_parent[i]) return -18; /* EXDEV */
    if (!ext4_chdir(old_parent)) return -LINUX_ENOENT;
    int ok = ext4_rename(old_leaf, new_leaf);
    ext4_chdir(old_directory);
    return ok ? 0 : -LINUX_EIO;
}

int64_t linux_fs_readlink(uint64_t user_path, uint64_t user_buffer, uint64_t count)
{
    char path[128], target[512];
    int copied = copy_user_string(user_path, path, sizeof(path));
    if (copied) return copied;
    int length = ext4_readlink(path, target, sizeof(target));
    if (length <= 0) return -LINUX_ENOENT;
    if ((uint64_t)length > count) length = (int)count;
    return memory_copy_to_user(user_buffer, target, (size_t)length) ? length : -LINUX_EFAULT;
}

int64_t linux_fs_map_file(uint64_t fd, uint64_t file_offset,
                          uint64_t user_address, uint64_t length)
{
    linux_file_t *file = get_file(fd);
    if (!file || !file->readable) return -LINUX_EBADF;
    if (file->directory) return -LINUX_EISDIR;
    if (file_offset > file->size) return 0;
    uint64_t count = file->size - file_offset;
    if (count > length) count = length;
    uint8_t chunk[256];
    uint64_t done = 0;
    while (done < count) {
        size_t size = (size_t)(count - done);
        if (size > sizeof(chunk)) size = sizeof(chunk);
        file_load(file, (uint32_t)(file_offset + done), chunk, size);
        if (!memory_load_to_user(user_address + done, chunk, size)) return -LINUX_EFAULT;
        done += size;
    }
    return 0;
}

int64_t linux_fs_chdir(uint64_t user_path)
{
    char path[128];
    int result = copy_user_string(user_path, path, sizeof(path));
    return result ? result : ext4_chdir(path) ? 0 : -LINUX_ENOENT;
}

int64_t linux_fs_getcwd(uint64_t user_buffer, uint64_t size)
{
    char path[256];
    if (!ext4_pwd(path, sizeof(path))) return -LINUX_EIO;
    size_t length = 0;
    while (path[length]) ++length;
    ++length;
    if (size < length) return -LINUX_ERANGE;
    return memory_copy_to_user(user_buffer, path, length) ? (int64_t)user_buffer : -LINUX_EFAULT;
}
