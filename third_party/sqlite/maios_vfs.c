#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "ext4.h"

#define DB_LIMIT (4u * 1024u * 1024u)
#define BLOCK_SIZE 1024u

typedef struct {
    sqlite3_file base;
    unsigned char *data;
    sqlite3_int64 size;
    sqlite3_int64 capacity;
    char name[64];
    int main_db;
    int writable;
} maios_file;

static int reserve(maios_file *f, sqlite3_int64 capacity)
{
    if (capacity < 0 || capacity > DB_LIMIT) return SQLITE_FULL;
    if (capacity <= f->capacity) return SQLITE_OK;
    sqlite3_int64 next = f->capacity ? f->capacity : BLOCK_SIZE;
    while (next < capacity) next *= 2;
    if (next > DB_LIMIT) next = DB_LIMIT;
    void *p = realloc(f->data, (size_t)next);
    if (!p) return SQLITE_NOMEM;
    memset((unsigned char *)p + f->capacity, 0, (size_t)(next - f->capacity));
    f->data = p;
    f->capacity = next;
    return SQLITE_OK;
}

static int flush_file(maios_file *f)
{
    if (!f->main_db || !f->writable || !f->name[0]) return SQLITE_OK;
    if (!ext4_write_begin(f->name)) return SQLITE_IOERR;
    sqlite3_int64 offset = 0;
    while (offset < f->size) {
        size_t n = (size_t)(f->size - offset);
        if (n > BLOCK_SIZE) n = BLOCK_SIZE;
        if (!ext4_write_block((uint32_t)(offset / BLOCK_SIZE), f->data + offset, n)) {
            (void)ext4_write_finish((uint32_t)offset);
            return SQLITE_IOERR_WRITE;
        }
        offset += n;
    }
    return ext4_write_finish((uint32_t)f->size) ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int io_close(sqlite3_file *file)
{
    maios_file *f = (maios_file *)file;
    int rc = flush_file(f);
    free(f->data);
    memset(f, 0, sizeof(*f));
    return rc;
}

static int io_read(sqlite3_file *file, void *out, int amount, sqlite3_int64 offset)
{
    maios_file *f = (maios_file *)file;
    if (offset < 0 || amount < 0) return SQLITE_IOERR_READ;
    if (offset >= f->size) { memset(out, 0, (size_t)amount); return SQLITE_IOERR_SHORT_READ; }
    sqlite3_int64 available = f->size - offset;
    if (available < amount) {
        memcpy(out, f->data + offset, (size_t)available);
        memset((unsigned char *)out + available, 0, (size_t)(amount - available));
        return SQLITE_IOERR_SHORT_READ;
    }
    memcpy(out, f->data + offset, (size_t)amount);
    return SQLITE_OK;
}

static int io_write(sqlite3_file *file, const void *input, int amount, sqlite3_int64 offset)
{
    maios_file *f = (maios_file *)file;
    if (offset < 0 || amount < 0 || offset > DB_LIMIT - amount) return SQLITE_FULL;
    int rc = reserve(f, offset + amount);
    if (rc != SQLITE_OK) return rc;
    memcpy(f->data + offset, input, (size_t)amount);
    if (offset + amount > f->size) f->size = offset + amount;
    return SQLITE_OK;
}

static int io_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    maios_file *f = (maios_file *)file;
    if (size < 0 || size > DB_LIMIT) return SQLITE_FULL;
    int rc = reserve(f, size);
    if (rc != SQLITE_OK) return rc;
    if (size > f->size) memset(f->data + f->size, 0, (size_t)(size - f->size));
    f->size = size;
    return SQLITE_OK;
}

static int io_sync(sqlite3_file *file, int flags) { (void)flags; return flush_file((maios_file *)file); }
static int io_size(sqlite3_file *file, sqlite3_int64 *size) { *size = ((maios_file *)file)->size; return SQLITE_OK; }
static int io_lock(sqlite3_file *file, int lock) { (void)file; (void)lock; return SQLITE_OK; }
static int io_unlock(sqlite3_file *file, int lock) { (void)file; (void)lock; return SQLITE_OK; }
static int io_reserved(sqlite3_file *file, int *result) { (void)file; *result = 0; return SQLITE_OK; }
static int io_control(sqlite3_file *file, int op, void *arg) { (void)file; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int io_sector(sqlite3_file *file) { (void)file; return 512; }
static int io_device(sqlite3_file *file) { (void)file; return 0; }

static const sqlite3_io_methods io_methods = {
    1, io_close, io_read, io_write, io_truncate, io_sync, io_size,
    io_lock, io_unlock, io_reserved, io_control, io_sector, io_device,
    0, 0, 0, 0, 0, 0
};

static int vfs_open(sqlite3_vfs *vfs, const char *path, sqlite3_file *file, int flags, int *out_flags)
{
    (void)vfs;
    maios_file *f = (maios_file *)file;
    memset(f, 0, sizeof(*f));
    if (flags & SQLITE_OPEN_WAL) return SQLITE_CANTOPEN;
    if (path && (flags & SQLITE_OPEN_MAIN_DB)) {
        const char *base = path;
        for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
        size_t len = strlen(base);
        if (!len || len > 63) return SQLITE_CANTOPEN;
        memcpy(f->name, base, len + 1);
        f->main_db = 1;
        f->writable = (flags & SQLITE_OPEN_READWRITE) != 0;
        uint32_t size;
        if (ext4_read_start(f->name, &size)) {
            if (size > DB_LIMIT) { ext4_read_end(); return SQLITE_FULL; }
            int rc = reserve(f, size);
            if (rc != SQLITE_OK) { ext4_read_end(); return rc; }
            f->size = size;
            for (uint32_t block = 0, offset = 0; offset < size; ++block) {
                unsigned char data[BLOCK_SIZE];
                if (!ext4_read_block(block, data)) { ext4_read_end(); return SQLITE_IOERR_READ; }
                uint32_t n = size - offset;
                if (n > BLOCK_SIZE) n = BLOCK_SIZE;
                memcpy(f->data + offset, data, n);
                offset += n;
            }
            ext4_read_end();
        } else if (!(flags & SQLITE_OPEN_CREATE)) return SQLITE_CANTOPEN;
    } else {
        f->name[0] = 0;
    }
    f->base.pMethods = &io_methods;
    if (out_flags) *out_flags = flags;
    return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *path, int sync_dir)
{
    (void)vfs; (void)sync_dir;
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    if (strstr(base, "-journal") || strstr(base, "-wal") || strstr(base, "-shm")) return SQLITE_OK;
    return ext4_unlink(base) ? SQLITE_OK : SQLITE_IOERR_DELETE;
}
static int vfs_access(sqlite3_vfs *vfs, const char *path, int flags, int *result)
{
    (void)vfs; (void)flags;
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    uint32_t size;
    *result = ext4_read_start(base, &size);
    if (*result) ext4_read_end();
    return SQLITE_OK;
}
static int vfs_fullpath(sqlite3_vfs *vfs, const char *path, int n, char *out)
{
    (void)vfs;
    size_t len = strlen(path);
    if (len + 1 > (size_t)n) return SQLITE_CANTOPEN;
    memcpy(out, path, len + 1);
    return SQLITE_OK;
}
static int vfs_random(sqlite3_vfs *vfs, int n, char *out)
{
    (void)vfs;
    static uint32_t state = 0x6d61696f;
    for (int i = 0; i < n; ++i) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; out[i] = (char)state; }
    return n;
}
static int vfs_sleep(sqlite3_vfs *vfs, int micros) { (void)vfs; (void)micros; return 0; }
static int vfs_time(sqlite3_vfs *vfs, double *time) { (void)vfs; *time = 2460000.0; return SQLITE_OK; }
static int vfs_error(sqlite3_vfs *vfs, int n, char *out) { (void)vfs; if (n) out[0] = 0; return 0; }

static sqlite3_vfs maios_vfs = {
    1, sizeof(maios_file), 256, 0, "maios", 0, vfs_open, vfs_delete,
    vfs_access, vfs_fullpath, 0, 0, 0, 0, vfs_random, vfs_sleep, vfs_time,
    vfs_error, 0, 0, 0
};

int maios_sqlite_vfs_register(void) { return sqlite3_vfs_register(&maios_vfs, 1); }
int sqlite3_os_init(void) { return maios_sqlite_vfs_register(); }
int sqlite3_os_end(void) { return SQLITE_OK; }

extern int maios_python_read_char(void);
extern void maios_python_write_char(char value);

static int print_row(void *unused, int columns, char **values, char **names)
{
    (void)unused;
    for (int i = 0; i < columns; ++i) {
        if (i) maios_python_write_char('|');
        const char *text = values[i] ? values[i] : "NULL";
        while (*text) maios_python_write_char(*text++);
    }
    maios_python_write_char('\n');
    (void)names;
    return 0;
}

static void shell_text(const char *text)
{
    while (*text) maios_python_write_char(*text++);
}

int maios_sqlite_shell(const char *filename)
{
    sqlite3 *db = 0;
    char line[512];
    size_t length = 0;
    int rc = sqlite3_open(filename, &db);
    if (rc != SQLITE_OK) {
        const char *message = db ? sqlite3_errmsg(db) : "out of memory";
        while (*message) maios_python_write_char(*message++);
        maios_python_write_char('\n');
        if (db) sqlite3_close(db);
        return -1;
    }
    for (;;) {
        shell_text(length ? "  ...> " : "sqlite> ");
        for (;;) {
            int key = maios_python_read_char();
            if (key == 4 && length == 0) goto done;
            if (key == '\r' || key == '\n') {
                maios_python_write_char('\n');
                line[length] = 0;
                if (strcmp(line, ".quit") == 0 || strcmp(line, ".exit") == 0) goto done;
                if (length && sqlite3_complete(line)) {
                    char *error = 0;
                    rc = sqlite3_exec(db, line, print_row, 0, &error);
                    if (rc != SQLITE_OK) {
                        const char *message = error ? error : sqlite3_errmsg(db);
                        while (*message) maios_python_write_char(*message++);
                        maios_python_write_char('\n');
                    }
                    sqlite3_free(error);
                    length = 0;
                    break;
                }
                if (length < sizeof(line) - 1) line[length++] = '\n';
                break;
            }
            if ((key == '\b' || key == 127) && length) {
                --length;
                maios_python_write_char('\b');
            } else if (key >= 32 && key < 127 && length < sizeof(line) - 1) {
                line[length++] = (char)key;
                maios_python_write_char((char)key);
            }
        }
    }
done:
    return sqlite3_close(db) == SQLITE_OK ? 0 : -1;
}
