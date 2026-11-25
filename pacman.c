#include <stddef.h>
#include <stdint.h>
#include "ext4.h"
#include "pacman.h"
#include "archpkg.h"

#define PACMAN_DB_FILE "local.db"
#define PACMAN_DB_LIMIT (64u * 1024u)
static char package_db[PACMAN_DB_LIMIT + 1];
static uint8_t package_file[PACMAN_DB_LIMIT];
static int database_error;

int pacman_last_database_error(void) { return database_error; }

static int valid_component(const char *text)
{
    if (!*text) return 0;
    if (text[0] == '.' && (text[1] == 0 || (text[1] == '.' && text[2] == 0))) return 0;
    for (; *text; ++text) {
        char c = *text;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+')) return 0;
    }
    return 1;
}

static int read_current_file(const char *name, uint8_t *buffer, uint32_t capacity,
                             uint32_t *size)
{
    if (!ext4_read_start(name, size)) return 0;
    if (*size > capacity) { ext4_read_end(); return -1; }
    for (uint32_t offset = 0; offset < *size;) {
        uint8_t block[1024];
        uint32_t index = offset / 1024u;
        uint32_t count = *size - offset;
        if (count > 1024u) count = 1024u;
        if (!ext4_read_block(index, block)) { ext4_read_end(); return -1; }
        for (uint32_t i = 0; i < count; ++i) buffer[offset + i] = block[i];
        offset += count;
    }
    ext4_read_end();
    return 1;
}

static int package_is_installed(const char *name)
{
    uint32_t size = 0;
    if (!ext4_read_start(PACMAN_DB_FILE, &size)) { database_error = 1; return -1; }
    if (size > PACMAN_DB_LIMIT) { database_error = 2; ext4_read_end(); return -1; }
    for (uint32_t offset = 0; offset < size;) {
        uint8_t block[1024];
        uint32_t index = offset / 1024u;
        uint32_t count = size - offset;
        if (count > 1024u) count = 1024u;
        if (!ext4_read_block(index, block)) { database_error = 3; ext4_read_end(); return -1; }
        for (uint32_t i = 0; i < count; ++i) package_db[offset + i] = (char)block[i];
        offset += count;
    }
    ext4_read_end();
    package_db[size] = 0;
    uint32_t at = 0;
    while (at < size) {
        uint32_t end = at;
        while (end < size && package_db[end] != '\n') end++;
        uint32_t name_length = 0;
        while (at + name_length < end && package_db[at + name_length] != ' ') name_length++;
        if (name_length == 0) { at = end + 1; continue; }
        uint32_t wanted_length = 0;
        while (name[wanted_length]) wanted_length++;
        if (name_length == wanted_length) {
            uint32_t i = 0;
            while (i < name_length && package_db[at + i] == name[i]) i++;
            if (i == name_length) return 1;
        }
        at = end + 1;
    }
    return 0;
}

static int name_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right) { left++; right++; }
    return *left == 0 && *right == 0;
}

static int ensure_dir(const char *name)
{
    if (ext4_chdir(name)) return 1;
    return ext4_mkdir(name) && ext4_chdir(name);
}

static int open_db(char *original, size_t capacity)
{
    if (!ext4_pwd(original, capacity) || !ext4_chdir("/")) return 0;
    if (!ensure_dir("var") || !ensure_dir("lib") || !ensure_dir("pacman")) {
        (void)ext4_chdir(original);
        return 0;
    }
    return 1;
}

static void close_db(const char *original)
{
    (void)ext4_chdir(original);
}

static int package_archive_header(uint32_t size, char *name, char *version,
                                  char *filename, uint32_t *payload_offset)
{
    static const char magic[] = "MAIOSPKG1 ";
    char header[160];
    uint32_t length = 0;
    while (length < size && length + 1 < sizeof(header) && package_file[length] != '\n') {
        header[length] = (char)package_file[length];
        length++;
    }
    if (length == size || length + 1 >= sizeof(header)) return 0;
    header[length] = 0;
    uint32_t i = 0;
    while (magic[i] && header[i] == magic[i]) i++;
    if (magic[i]) return 0;
    uint32_t out = 0;
    while (header[i] && header[i] != ' ' && out < 63) name[out++] = header[i++];
    if (!out || header[i++] != ' ') return 0;
    name[out] = 0;
    out = 0;
    while (header[i] && header[i] != ' ' && out < 63) version[out++] = header[i++];
    if (!out || header[i++] != ' ') return 0;
    version[out] = 0;
    out = 0;
    while (header[i] && out < 63) filename[out++] = header[i++];
    filename[out] = 0;
    if (header[i]) return 0;
    if (!valid_component(name) || !valid_component(version) || !valid_component(filename)) return 0;
    *payload_offset = length + 1;
    return 1;
}

int pacman_install_local(const char *archive_name)
{
    if (!valid_component(archive_name)) return -1;
    char original[1100], name[64], version[64], filename[64];
    if (!ext4_pwd(original, sizeof(original))) return -1;
    uint32_t archive_size = 0, payload_offset = 0;
    if (read_current_file(archive_name, package_file, sizeof(package_file), &archive_size) != 1 ||
        !package_archive_header(archive_size, name, version, filename, &payload_offset)) return -1;

    if (!open_db(original, sizeof(original))) return -1;
    int installed = package_is_installed(name);
    if (installed != 0) { close_db(original); return installed < 0 ? -1 : -2; }

    if (!ext4_chdir("/")) { close_db(original); return -1; }
    if (!ensure_dir("apps")) { close_db(original); return -1; }
    uint32_t existing_size;
    if (ext4_read_start(filename, &existing_size)) {
        ext4_read_end();
        close_db(original);
        return -3;
    }
    uint32_t payload_size = archive_size - payload_offset;
    if (!ext4_write(filename, package_file + payload_offset, payload_size)) {
        close_db(original);
        return -1;
    }
    if (!ext4_chdir("/var/lib/pacman")) {
        (void)ext4_chdir("/apps"); (void)ext4_unlink(filename);
        close_db(original);
        return -1;
    }
    uint32_t db_size = 0;
    int db_read = read_current_file(PACMAN_DB_FILE, (uint8_t *)package_db,
                                    PACMAN_DB_LIMIT, &db_size);
    if (db_read < 0) {
        (void)ext4_chdir("/apps"); (void)ext4_unlink(filename);
        close_db(original);
        return -1;
    }
    if (!db_read) db_size = 0;
    static const char space[] = " ";
    uint32_t n = 0; while (name[n]) n++;
    uint32_t v = 0; while (version[v]) v++;
    uint32_t f = 0; while (filename[f]) f++;
    if (db_size + n + v + f + 4 > PACMAN_DB_LIMIT) {
        (void)ext4_chdir("/apps"); (void)ext4_unlink(filename);
        close_db(original);
        return -1;
    }
    uint32_t at = db_size;
    if (at && package_db[at - 1] != '\n') package_db[at++] = '\n';
    for (uint32_t i = 0; i < n; ++i) package_db[at++] = name[i];
    package_db[at++] = space[0];
    for (uint32_t i = 0; i < v; ++i) package_db[at++] = version[i];
    package_db[at++] = space[0];
    for (uint32_t i = 0; i < f; ++i) package_db[at++] = filename[i];
    package_db[at++] = '\n';
    if (!ext4_write(PACMAN_DB_FILE, (const uint8_t *)package_db, at)) {
        (void)ext4_chdir("/apps"); (void)ext4_unlink(filename);
        close_db(original);
        return -1;
    }
    close_db(original);
    return 1;
}

int pacman_register_arch(const char *name, const char *version)
{
    if (!valid_component(name) || !version || !*version) return -1;
    uint32_t name_length = 0, version_length = 0;
    while (name[name_length]) ++name_length;
    while (version[version_length]) {
        char c = version[version_length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+' || c == ':')) return -1;
        if (++version_length >= 96) return -1;
    }
    char original[1100];
    if (!ext4_pwd(original, sizeof(original)) || !open_db(original, sizeof(original))) return -1;
    int installed = package_is_installed(name);
    if (installed != 0) { close_db(original); return installed < 0 ? -1 : 0; }
    uint32_t size = 0;
    int read_result = read_current_file(PACMAN_DB_FILE, (uint8_t *)package_db,
                                        PACMAN_DB_LIMIT, &size);
    if (read_result < 0 || size + name_length + version_length + 8 > PACMAN_DB_LIMIT) {
        close_db(original);
        return -1;
    }
    if (!read_result) size = 0;
    uint32_t at = size;
    if (at && package_db[at - 1] != '\n') package_db[at++] = '\n';
    for (uint32_t i = 0; i < name_length; ++i) package_db[at++] = name[i];
    package_db[at++] = ' ';
    for (uint32_t i = 0; i < version_length; ++i) package_db[at++] = version[i];
    package_db[at++] = ' ';
    package_db[at++] = '@'; package_db[at++] = 'a'; package_db[at++] = 'r';
    package_db[at++] = 'c'; package_db[at++] = 'h'; package_db[at++] = '\n';
    int ok = ext4_write(PACMAN_DB_FILE, (const uint8_t *)package_db, at);
    close_db(original);
    return ok ? 1 : -1;
}

int pacman_package_installed(const char *name)
{
    char original[1100];
    database_error = 0;
    if (!valid_component(name)) { database_error = 4; return -1; }
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        if (!ext4_pwd(original, sizeof(original)) || !open_db(original, sizeof(original))) {
            database_error = 5;
            continue;
        }
        int result = package_is_installed(name);
        close_db(original);
        if (result >= 0) return result;
    }
    return -1;
}

int pacman_remove(const char *package_name)
{
    if (!valid_component(package_name)) return -1;
    char original[1100];
    if (!ext4_pwd(original, sizeof(original)) || !open_db(original, sizeof(original))) return -1;
    uint32_t size = 0;
    int read_result = read_current_file(PACMAN_DB_FILE, (uint8_t *)package_db,
                                        PACMAN_DB_LIMIT, &size);
    if (read_result != 1) { close_db(original); return read_result < 0 ? -1 : 0; }

    uint32_t position = 0, output = 0;
    int found = 0, arch_package = 0;
    char filename[64] = {0};
    while (position < size) {
        uint32_t start = position;
        while (position < size && package_db[position] != '\n') position++;
        uint32_t end = position;
        if (position < size) position++;
        uint32_t first_space = start;
        while (first_space < end && package_db[first_space] != ' ') first_space++;
        uint32_t second_space = first_space + 1;
        while (second_space < end && package_db[second_space] != ' ') second_space++;
        uint32_t name_length = first_space - start, wanted_length = 0;
        while (package_name[wanted_length]) wanted_length++;
        int match = name_length == wanted_length;
        for (uint32_t i = 0; match && i < name_length; ++i)
            if (package_db[start + i] != package_name[i]) match = 0;
        if (match && first_space < end && second_space < end) {
            uint32_t file_length = end - second_space - 1;
            if (!file_length || file_length >= sizeof(filename)) {
                close_db(original);
                return -1;
            }
            for (uint32_t i = 0; i < file_length; ++i) filename[i] = package_db[second_space + 1 + i];
            filename[file_length] = 0;
            if (name_equal(filename, "@arch")) {
                if (!archpkg_remove_files(package_name)) {
                    close_db(original);
                    return -1;
                }
                arch_package = 1;
            }
            found = 1;
        } else {
            for (uint32_t i = start; i < position; ++i) package_file[output++] = (uint8_t)package_db[i];
        }
    }
    if (!found) { close_db(original); return 0; }
    int removed = arch_package ?
        ext4_chdir("/var/lib/pacman") && ext4_write(PACMAN_DB_FILE, package_file, output) :
        ext4_chdir("/apps") && ext4_unlink(filename) &&
        ext4_chdir("/var/lib/pacman") && ext4_write(PACMAN_DB_FILE, package_file, output);
    if (!removed) {
        close_db(original);
        return -1;
    }
    close_db(original);
    return 1;
}

int pacman_list_installed(void (*write)(const char *), int quiet, const char *wanted)
{
    char original[1100];
    if (!open_db(original, sizeof(original))) return -1;

    uint32_t size = 0;
    if (!ext4_read_start(PACMAN_DB_FILE, &size)) {
        close_db(original);
        return 0;
    }
    if (size > PACMAN_DB_LIMIT) {
        ext4_read_end();
        close_db(original);
        return -1;
    }

    char *data = package_db;
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t block = offset / 1024u;
        uint32_t block_start = block * 1024u;
        uint32_t chunk = size - block_start;
        if (chunk > 1024u) chunk = 1024u;
        uint8_t bytes[1024];
        if (!ext4_read_block(block, bytes)) {
            ext4_read_end();
            close_db(original);
            return -1;
        }
        uint32_t copy = chunk;
        if (copy > size - offset) copy = size - offset;
        for (uint32_t i = 0; i < copy; ++i) data[offset + i] = (char)bytes[i];
        offset += copy;
    }
    ext4_read_end();
    close_db(original);
    data[size] = 0;

    int found = 0;
    uint32_t position = 0;
    while (position < size) {
        char line[160];
        uint32_t used = 0;
        while (position < size && data[position] != '\n' && used + 1 < sizeof(line))
            line[used++] = data[position++];
        while (position < size && data[position] != '\n') position++;
        if (position < size) position++;
        line[used] = 0;
        if (!used || line[0] == '#') continue;

        char *separator = line;
        while (*separator && *separator != ' ') separator++;
        if (!*separator) continue;
        *separator++ = 0;
        while (*separator == ' ') separator++;
        char *version_end = separator;
        while (*version_end && *version_end != ' ') version_end++;
        *version_end = 0;
        if (wanted) {
            if (name_equal(line, wanted)) {
                write(line);
                if (!quiet && *separator) { write(" "); write(separator); }
                write("\n");
                found = 1;
                break;
            }
        } else {
            write(line);
            if (!quiet && *separator) { write(" "); write(separator); }
            write("\n");
            found = 1;
        }
    }
    return found;
}
