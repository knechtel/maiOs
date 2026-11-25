#include <stddef.h>
#include <stdint.h>
#include "archpkg.h"
#include "ext4.h"
#include "zstd.h"

#define TAR_BLOCK 512u
#define EXT4_BLOCK 1024u
#define PATH_CAP 256u
#define ZSTD_INPUT_SIZE 16384u
#define ZSTD_OUTPUT_SIZE 262144u

static uint8_t zstd_input[ZSTD_INPUT_SIZE];
static uint8_t zstd_output[ZSTD_OUTPUT_SIZE];
static char installed_package_name[64];
static char installed_package_version[96];
static char installed_dependencies[32][96];
static size_t installed_dependency_count;
static char package_manifest[1024u * 1024u];
static uint32_t package_manifest_size;
static int last_error;
static int last_filesystem_error;
static char last_failed_path[PATH_CAP];
static archpkg_progress_fn progress_callback;
static int ensure_directory(const char *path);

int archpkg_last_error(void) { return last_error; }
const char *archpkg_last_failed_path(void) { return last_failed_path; }
int archpkg_last_filesystem_error(void) { return last_filesystem_error; }
void archpkg_set_progress_callback(archpkg_progress_fn callback) { progress_callback = callback; }

static void entry_installed(int *count)
{
    ++*count;
    if (progress_callback && (*count % 256) == 0) progress_callback((size_t)*count);
}

static void remember_failed_path(const char *path)
{
    size_t i = 0;
    while (path[i] && i + 1 < sizeof(last_failed_path)) {
        last_failed_path[i] = path[i];
        ++i;
    }
    last_failed_path[i] = 0;
}

static int valid_package_name(const char *name)
{
    size_t length = 0;
    while (name[length]) {
        char c = name[length++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+')) return 0;
        if (length >= 64) return 0;
    }
    return length != 0;
}

static int manifest_add(char kind, const char *path)
{
    size_t length = 0;
    while (path[length]) ++length;
    if (!length || length + 3 > sizeof(package_manifest) - package_manifest_size) return 0;
    package_manifest[package_manifest_size++] = kind;
    package_manifest[package_manifest_size++] = ' ';
    for (size_t i = 0; i < length; ++i) package_manifest[package_manifest_size++] = path[i];
    package_manifest[package_manifest_size++] = '\n';
    return 1;
}

static int store_package_manifest(const char *previous_directory)
{
    if (!valid_package_name(installed_package_name)) return 0;
    char manifest_name[80];
    size_t n = 0;
    while (installed_package_name[n]) { manifest_name[n] = installed_package_name[n]; ++n; }
    const char suffix[] = ".files";
    for (size_t i = 0; i < sizeof(suffix); ++i) manifest_name[n + i] = suffix[i];
    if (!ensure_directory("/var/lib/pacman") || !ext4_chdir("/var/lib/pacman") ||
        !ext4_write_begin(manifest_name)) {
        (void)ext4_chdir(previous_directory);
        return 0;
    }
    uint32_t offset = 0, index = 0;
    int ok = 1;
    while (offset < package_manifest_size) {
        uint32_t count = package_manifest_size - offset;
        if (count > EXT4_BLOCK) count = EXT4_BLOCK;
        if (!ext4_write_block(index++, (const uint8_t *)package_manifest + offset, count)) { ok = 0; break; }
        offset += count;
    }
    if (!ext4_write_finish(ok ? package_manifest_size : offset)) ok = 0;
    (void)ext4_chdir(previous_directory);
    return ok;
}

static int text_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right) { ++left; ++right; }
    return *left == 0 && *right == 0;
}

static int pkginfo_field(const uint8_t *data, uint32_t length,
                         const char *key, char *destination, size_t capacity)
{
    size_t key_length = 0;
    while (key[key_length]) ++key_length;
    for (uint32_t at = 0; at < length;) {
        uint32_t end = at;
        while (end < length && data[end] != '\n') ++end;
        if (end - at > key_length + 3 && data[at] == key[0]) {
            size_t i = 0;
            while (i < key_length && data[at + i] == (uint8_t)key[i]) ++i;
            if (i == key_length && data[at + i] == ' ' && data[at + i + 1] == '=' && data[at + i + 2] == ' ') {
                uint32_t start = at + (uint32_t)key_length + 3u;
                uint32_t value_length = end - start;
                if (!value_length || value_length >= capacity) return 0;
                for (uint32_t j = 0; j < value_length; ++j) destination[j] = (char)data[start + j];
                destination[value_length] = 0;
                return 1;
            }
        }
        at = end + (end < length);
    }
    return 0;
}

int archpkg_package_info(char *name, size_t name_capacity,
                         char *version, size_t version_capacity)
{
    size_t name_length = 0, version_length = 0;
    while (installed_package_name[name_length]) ++name_length;
    while (installed_package_version[version_length]) ++version_length;
    if (!name_length || !version_length || name_length >= name_capacity ||
        version_length >= version_capacity) return 0;
    for (size_t i = 0; i <= name_length; ++i) name[i] = installed_package_name[i];
    for (size_t i = 0; i <= version_length; ++i) version[i] = installed_package_version[i];
    return 1;
}

size_t archpkg_dependency_count(void)
{
    return installed_dependency_count;
}

int archpkg_dependency(size_t index, char *dependency, size_t capacity)
{
    if (index >= installed_dependency_count) return 0;
    size_t length = 0;
    while (installed_dependencies[index][length]) ++length;
    if (!length || length >= capacity) return 0;
    for (size_t i = 0; i <= length; ++i) dependency[i] = installed_dependencies[index][i];
    return 1;
}

typedef struct {
    uint32_t size, offset, cached_block;
    uint8_t cache[EXT4_BLOCK];
    int cache_valid;
} tar_reader_t;

static int tar_read(tar_reader_t *reader, uint32_t offset, void *out, size_t length)
{
    uint8_t *destination = out;
    while (length) {
        uint32_t block = offset / EXT4_BLOCK;
        uint32_t inside = offset % EXT4_BLOCK;
        size_t count = EXT4_BLOCK - inside;
        if (count > length) count = length;
        if (offset > reader->size || count > reader->size - offset) return 0;
        if (!reader->cache_valid || reader->cached_block != block) {
            if (!ext4_read_block(block, reader->cache)) return 0;
            reader->cached_block = block;
            reader->cache_valid = 1;
        }
        for (size_t i = 0; i < count; ++i) destination[i] = reader->cache[inside + i];
        destination += count;
        offset += (uint32_t)count;
        length -= count;
    }
    return 1;
}

static uint32_t tar_octal(const uint8_t *field, size_t length, int *ok)
{
    uint32_t value = 0;
    size_t i = 0;
    while (i < length && (field[i] == ' ' || field[i] == 0)) ++i;
    for (; i < length && field[i] && field[i] != ' '; ++i) {
        if (field[i] < '0' || field[i] > '7' || value > (UINT32_MAX >> 3)) {
            *ok = 0;
            return 0;
        }
        value = (value << 3) | (uint32_t)(field[i] - '0');
    }
    return value;
}

static int tar_path(const uint8_t *header, char *path)
{
    size_t prefix = 0, name = 0, used = 0;
    while (prefix < 155 && header[345 + prefix]) ++prefix;
    while (name < 100 && header[name]) ++name;
    if (!name || prefix + (prefix ? 1u : 0u) + name >= PATH_CAP) return 0;
    if (prefix) {
        for (size_t i = 0; i < prefix; ++i) path[used++] = (char)header[345 + i];
        path[used++] = '/';
    }
    for (size_t i = 0; i < name; ++i) path[used++] = (char)header[i];
    path[used] = 0;
    while (path[0] == '.' && path[1] == '/') {
        for (size_t i = 0; path[i]; ++i) path[i] = path[i + 2];
    }
    if (!path[0]) { path[0] = '.'; path[1] = 0; }
    if (path[0] == '/') return 0;
    for (size_t i = 0; path[i];) {
        size_t start = i;
        while (path[i] && path[i] != '/') ++i;
        if (i - start == 2 && path[start] == '.' && path[start + 1] == '.') return 0;
        if (path[i]) ++i;
    }
    return 1;
}

static int ensure_directory(const char *path)
{
    char work[PATH_CAP];
    size_t length = 0;
    while (path[length] && length + 1 < sizeof(work)) { work[length] = path[length]; ++length; }
    if (path[length] || !length) return 0;
    work[length] = 0;
    if (!ext4_chdir("/")) return 0;
    for (size_t i = 0; i <= length; ++i) {
        if (work[i] != '/' && work[i] != 0) continue;
        char saved = work[i];
        work[i] = 0;
        if (i) {
            char *component = work;
            for (size_t j = 0; j < i; ++j) if (work[j] == '/') component = work + j + 1;
            if (!ext4_chdir(component)) {
                if (!ext4_mkdir(component) || !ext4_chdir(component)) { work[i] = saved; return 0; }
            }
        }
        work[i] = saved;
    }
    return 1;
}

static int split_path(const char *path, char *parent, char *leaf)
{
    size_t length = 0, slash = (size_t)-1;
    while (path[length] && length + 1 < PATH_CAP) {
        if (path[length] == '/') slash = length;
        ++length;
    }
    if (path[length] || !length || length - (slash == (size_t)-1 ? 0 : slash + 1) > 63) return 0;
    size_t start = slash == (size_t)-1 ? 0 : slash + 1;
    if (slash == (size_t)-1) parent[0] = 0;
    else {
        size_t n = slash ? slash : 1;
        if (!slash) parent[0] = '/';
        else for (size_t i = 0; i < n; ++i) parent[i] = path[i];
        parent[n] = 0;
    }
    for (size_t i = 0; i <= length - start; ++i) leaf[i] = path[start + i];
    return leaf[0] != 0;
}

static uint32_t tar_checksum(const uint8_t *header)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; ++i) sum += (i >= 148 && i < 156) ? ' ' : header[i];
    return sum;
}

static int pax_parse(const uint8_t *data, uint32_t length, char *path,
                     char *linkpath, uint32_t *file_size, int *has_size)
{
    uint32_t at = 0;
    while (at < length) {
        uint32_t record_length = 0, digits = 0;
        while (at < length && data[at] >= '0' && data[at] <= '9') {
            if (record_length > UINT32_MAX / 10u) return 0;
            record_length = record_length * 10u + (uint32_t)(data[at++] - '0');
            ++digits;
        }
        if (!digits || at >= length || data[at++] != ' ' ||
            record_length < digits + 4u || record_length > length - (at - digits - 1u)) return 0;
        uint32_t start = at, end = at - digits - 1u + record_length;
        uint32_t equal = start;
        while (equal < end && data[equal] != '=') ++equal;
        if (equal == end) return 0;
        uint32_t value = equal + 1u, value_end = end;
        if (value_end && data[value_end - 1] == '\n') --value_end;
        if (equal - start == 4 && data[start] == 'p' && data[start + 1] == 'a' &&
            data[start + 2] == 't' && data[start + 3] == 'h') {
            uint32_t n = value_end - value;
            if (!n || n >= PATH_CAP) return 0;
            for (uint32_t i = 0; i < n; ++i) path[i] = (char)data[value + i];
            path[n] = 0;
        } else if (equal - start == 8 && data[start] == 'l' && data[start + 1] == 'i' &&
                   data[start + 2] == 'n' && data[start + 3] == 'k' && data[start + 4] == 'p' &&
                   data[start + 5] == 'a' && data[start + 6] == 't' && data[start + 7] == 'h') {
            uint32_t n = value_end - value;
            if (!n || n >= 256) return 0;
            for (uint32_t i = 0; i < n; ++i) linkpath[i] = (char)data[value + i];
            linkpath[n] = 0;
        } else if (equal - start == 4 && data[start] == 's' && data[start + 1] == 'i' &&
                   data[start + 2] == 'z' && data[start + 3] == 'e') {
            uint32_t number = 0;
            if (value == value_end) return 0;
            for (uint32_t i = value; i < value_end; ++i) {
                uint32_t digit = (uint32_t)(data[i] - '0');
                if (data[i] < '0' || data[i] > '9' || number > (UINT32_MAX - digit) / 10u) return 0;
                number = number * 10u + digit;
            }
            *file_size = number;
            *has_size = 1;
        }
        at = end;
    }
    return at == length;
}

int archpkg_install_tar(const char *archive_path)
{
    last_error = ARCHPKG_ERROR_TAR_FORMAT;
    last_filesystem_error = ARCHPKG_FS_ERROR_NONE;
    last_failed_path[0] = 0;
    installed_package_name[0] = installed_package_version[0] = 0;
    installed_dependency_count = 0;
    package_manifest_size = 0;
    char previous_directory[256];
    if (!ext4_pwd(previous_directory, sizeof(previous_directory))) {
        last_error = ARCHPKG_ERROR_ARCHIVE_READ;
        return 0;
    }
    uint32_t archive_size;
    if (!ext4_read_start(archive_path, &archive_size)) {
        last_error = ARCHPKG_ERROR_ARCHIVE_READ;
        return 0;
    }
    tar_reader_t reader = { .size = archive_size, .cached_block = UINT32_MAX };
    uint8_t header[TAR_BLOCK], data[EXT4_BLOCK];
    uint8_t pax_data[4096];
    char pax_path[PATH_CAP] = {0}, pax_link[256] = {0};
    uint32_t pax_size = 0;
    int pax_has_size = 0;
    uint32_t cursor = 0;
    int result = 1, saw_end = 0, installed_entries = 0;
    while (cursor <= archive_size && archive_size - cursor >= TAR_BLOCK) {
        if (!tar_read(&reader, cursor, header, sizeof(header))) { result = 0; break; }
        int zero_header = 1;
        for (size_t i = 0; i < sizeof(header); ++i) if (header[i]) { zero_header = 0; break; }
        if (zero_header) { saw_end = 1; break; }
        int valid = 1;
        uint32_t expected = tar_octal(header + 148, 8, &valid);
        uint32_t size = tar_octal(header + 124, 12, &valid);
        if (!valid || expected != tar_checksum(header) || size > archive_size - cursor - TAR_BLOCK) { result = 0; break; }
        char type = (char)header[156];
        uint32_t payload = cursor + TAR_BLOCK;
        if (type == 'x') {
            if (size > sizeof(pax_data) || !tar_read(&reader, payload, pax_data, size) ||
                !pax_parse(pax_data, size, pax_path, pax_link, &pax_size, &pax_has_size)) {
                result = 0; break;
            }
            uint32_t padded_pax = (size + TAR_BLOCK - 1u) & ~(TAR_BLOCK - 1u);
            if (padded_pax < size || payload > UINT32_MAX - padded_pax) { result = 0; break; }
            cursor = payload + padded_pax;
            continue;
        }
        char path[PATH_CAP], parent[PATH_CAP], leaf[64];
        if (!tar_path(header, path)) { result = 0; break; }
        if (pax_path[0]) {
            size_t n = 0; while (pax_path[n]) ++n;
            if (n >= sizeof(path)) { result = 0; break; }
            for (size_t i = 0; i <= n; ++i) path[i] = pax_path[i];
        }
        if (path[0] == '/' || !path[0]) { result = 0; break; }
        remember_failed_path(path);
        for (size_t i = 0; path[i];) {
            size_t start = i;
            while (path[i] && path[i] != '/') ++i;
            if (i - start == 2 && path[start] == '.' && path[start + 1] == '.') { result = 0; break; }
            if (path[i]) ++i;
        }
        if (!result) break;
        if (pax_has_size) size = pax_size;
        if (size > archive_size - payload) { result = 0; break; }
        int root_metadata = path[0] == '.' && path[1] != '/';
        for (size_t i = 0; path[i]; ++i) if (path[i] == '/') root_metadata = 0;
        if (root_metadata) {
            if (text_equal(path, ".PKGINFO") && size <= 8192) {
                uint8_t metadata[8192];
                if (!tar_read(&reader, payload, metadata, size) ||
                    !pkginfo_field(metadata, size, "pkgname", installed_package_name,
                                   sizeof(installed_package_name)) ||
                    !pkginfo_field(metadata, size, "pkgver", installed_package_version,
                                   sizeof(installed_package_version))) { result = 0; break; }
                if (!valid_package_name(installed_package_name)) { result = 0; break; }
                for (uint32_t at = 0; at < size;) {
                    uint32_t end = at;
                    while (end < size && metadata[end] != '\n') ++end;
                    if (end - at > 9 && metadata[at] == 'd' && metadata[at + 1] == 'e' &&
                        metadata[at + 2] == 'p' && metadata[at + 3] == 'e' &&
                        metadata[at + 4] == 'n' && metadata[at + 5] == 'd' &&
                        metadata[at + 6] == ' ' && metadata[at + 7] == '=' && metadata[at + 8] == ' ') {
                        uint32_t n = end - at - 9;
                        if (!n || n >= sizeof(installed_dependencies[0]) ||
                            installed_dependency_count == sizeof(installed_dependencies) / sizeof(installed_dependencies[0])) {
                            result = 0; break;
                        }
                        for (uint32_t i = 0; i < n; ++i)
                            installed_dependencies[installed_dependency_count][i] = (char)metadata[at + 9 + i];
                        installed_dependencies[installed_dependency_count++][n] = 0;
                    }
                    at = end + (end < size);
                }
                if (!result) break;
            }
        } else if (type == '5') {
            size_t n = 0; while (path[n]) ++n;
            if (n && path[n - 1] == '/') path[n - 1] = 0;
            if (!ensure_directory(path) || !manifest_add('d', path)) {
                last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
            }
            entry_installed(&installed_entries);
        } else if (type == '2' || type == '1' || type == '0' || type == 0) {
            if (path[0] == '.' && path[1] == 0) { cursor = payload + ((size + 511u) & ~511u); continue; }
            if (!split_path(path, parent, leaf) || (parent[0] && !ensure_directory(parent)) ||
                !ext4_chdir("/") || (parent[0] && !ext4_chdir(parent))) {
                last_filesystem_error = ARCHPKG_FS_ERROR_PATH;
                last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
            }
            int existing_type = ext4_entry_type(leaf);
            if (existing_type && existing_type != 2 && !ext4_unlink(leaf)) {
                last_filesystem_error = ARCHPKG_FS_ERROR_REPLACE;
                last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
            }
            if (type == '2') {
                char target[256]; size_t n = 0;
                if (pax_link[0]) {
                    while (pax_link[n] && n < sizeof(target) - 1) { target[n] = pax_link[n]; ++n; }
                } else while (n < 100 && header[157 + n]) { target[n] = (char)header[157 + n]; ++n; }
                target[n] = 0;
                if (!n || !ext4_symlink(target, leaf) || !manifest_add('l', path)) {
                    last_filesystem_error = ARCHPKG_FS_ERROR_SYMLINK;
                    last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
                }
                entry_installed(&installed_entries);
            } else if (type == '1') {
                char target[256], source_absolute[258], destination_absolute[258];
                size_t n = 0;
                if (pax_link[0]) {
                    while (pax_link[n] && n < sizeof(target) - 1) { target[n] = pax_link[n]; ++n; }
                } else while (n < 100 && header[157 + n]) { target[n] = (char)header[157 + n]; ++n; }
                target[n] = 0;
                if (!n || n + 2 > sizeof(source_absolute) ||
                    sizeof(path) > sizeof(destination_absolute)) { result = 0; break; }
                source_absolute[0] = '/';
                for (size_t i = 0; i <= n; ++i) source_absolute[i + 1] = target[i];
                destination_absolute[0] = '/';
                size_t path_length = 0; while (path[path_length]) ++path_length;
                if (path_length + 2 > sizeof(destination_absolute)) { result = 0; break; }
                for (size_t i = 0; i <= path_length; ++i) destination_absolute[i + 1] = path[i];
                if (!ext4_link(source_absolute, destination_absolute) || !manifest_add('h', path)) {
                    last_filesystem_error = ARCHPKG_FS_ERROR_HARDLINK;
                    last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
                }
                entry_installed(&installed_entries);
            } else {
                if (!ext4_write_begin(leaf)) {
                    last_filesystem_error = ARCHPKG_FS_ERROR_FILE_OPEN;
                    last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
                }
                uint32_t remaining = size, index = 0;
                while (remaining) {
                    size_t count = remaining > EXT4_BLOCK ? EXT4_BLOCK : remaining;
                    if (!tar_read(&reader, payload + index * EXT4_BLOCK, data, count)) {
                        last_filesystem_error = ARCHPKG_FS_ERROR_FILE_READ;
                        last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
                    }
                    if (!ext4_write_block(index, data, count)) {
                        last_filesystem_error = ARCHPKG_FS_ERROR_FILE_WRITE;
                        last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0; break;
                    }
                    remaining -= (uint32_t)count;
                    ++index;
                }
                int finish_ok = ext4_write_finish(result ? size : 0);
                if (!finish_ok && result) {
                    last_filesystem_error = ARCHPKG_FS_ERROR_FILE_FINISH;
                    last_error = ARCHPKG_ERROR_TAR_FILESYSTEM; result = 0;
                }
                if (!result) break;
                if (!manifest_add('f', path)) {
                    last_error = ARCHPKG_ERROR_MANIFEST; result = 0; break;
                }
                entry_installed(&installed_entries);
            }
        } else if (type != 'x' && type != 'g') {
            result = 0;
            break;
        }
        uint32_t padded = (size + TAR_BLOCK - 1u) & ~(TAR_BLOCK - 1u);
        if (padded < size || payload > UINT32_MAX - padded) { result = 0; break; }
        cursor = payload + padded;
        pax_path[0] = pax_link[0] = 0;
        pax_size = 0;
        pax_has_size = 0;
    }
    if (!saw_end || !installed_entries) result = 0;
    ext4_read_end();
    if (result && installed_package_name[0] && !store_package_manifest(previous_directory)) {
        last_error = ARCHPKG_ERROR_MANIFEST; result = 0;
    }
    (void)ext4_chdir(previous_directory);
    if (result) { last_error = ARCHPKG_ERROR_NONE; last_failed_path[0] = 0; }
    return result;
}

int archpkg_install_zstd(const char *archive_path)
{
    last_error = ARCHPKG_ERROR_ARCHIVE_READ;
    last_failed_path[0] = 0;
    char previous_directory[256];
    uint32_t compressed_size;
    if (!ext4_pwd(previous_directory, sizeof(previous_directory)) ||
        !ext4_read_start(archive_path, &compressed_size)) return 0;

    last_error = ARCHPKG_ERROR_ZSTD_INIT;
    ZSTD_DStream *decoder = ZSTD_createDStream();
    int ok = decoder != NULL;
    size_t zresult = ok ? ZSTD_DCtx_setParameter(decoder, ZSTD_d_windowLogMax, 25) : 1;
    if (ok && !ZSTD_isError(zresult)) zresult = ZSTD_initDStream(decoder);
    if (!ok || ZSTD_isError(zresult)) ok = 0;

    if (ok && !ext4_chdir("/")) ok = 0;
    if (ok && !ext4_chdir("var") && (!ext4_mkdir("var") || !ext4_chdir("var"))) ok = 0;
    if (ok && !ext4_chdir("lib") && (!ext4_mkdir("lib") || !ext4_chdir("lib"))) ok = 0;
    if (ok && !ext4_chdir("pacman") && (!ext4_mkdir("pacman") || !ext4_chdir("pacman"))) ok = 0;
    const char *temporary_tar = ".archpkg-unpack.tar";
    if (ok) last_error = ARCHPKG_ERROR_TEMP_WRITE;
    if (ok && !ext4_write_begin(temporary_tar)) ok = 0;
    if (ok) last_error = ARCHPKG_ERROR_ZSTD_DECODE;

    uint8_t input_block[EXT4_BLOCK], file_block[EXT4_BLOCK];
    ZSTD_inBuffer input = { zstd_input, 0, 0 };
    uint32_t source_offset = 0, output_index = 0, total_output = 0;
    size_t pending = 0, last_hint = 1;
    int saw_frame = 0;
    while (ok && (source_offset < compressed_size || input.pos < input.size)) {
        if (input.pos == input.size) {
            size_t amount = compressed_size - source_offset;
            if (amount > sizeof(zstd_input)) amount = sizeof(zstd_input);
            size_t copied = 0;
            while (copied < amount) {
                uint32_t block_index = (source_offset + (uint32_t)copied) / EXT4_BLOCK;
                uint32_t within = (source_offset + (uint32_t)copied) % EXT4_BLOCK;
                if (!ext4_read_block(block_index, input_block)) { ok = 0; break; }
                size_t count = EXT4_BLOCK - within;
                if (count > amount - copied) count = amount - copied;
                for (size_t i = 0; i < count; ++i) zstd_input[copied + i] = input_block[within + i];
                copied += count;
            }
            if (!ok) break;
            source_offset += (uint32_t)amount;
            input.size = amount;
            input.pos = 0;
        }
        ZSTD_outBuffer output = { zstd_output, sizeof(zstd_output), 0 };
        last_hint = ZSTD_decompressStream(decoder, &output, &input);
        if (ZSTD_isError(last_hint)) { ok = 0; break; }
        if (last_hint == 0) saw_frame = 1;
        size_t produced = output.pos, consumed = 0;
        while (consumed < produced) {
            size_t count = EXT4_BLOCK - pending;
            if (count > produced - consumed) count = produced - consumed;
            for (size_t i = 0; i < count; ++i) file_block[pending + i] = zstd_output[consumed + i];
            pending += count;
            consumed += count;
            if (pending == EXT4_BLOCK) {
                if (!ext4_write_block(output_index++, file_block, EXT4_BLOCK)) {
                    last_error = ARCHPKG_ERROR_TEMP_WRITE; ok = 0; break;
                }
                if (total_output > UINT32_MAX - EXT4_BLOCK) { ok = 0; break; }
                total_output += EXT4_BLOCK;
                pending = 0;
            }
        }
        if (input.pos == input.size && source_offset == compressed_size && last_hint != 0) {
            while (last_hint != 0) {
                ZSTD_inBuffer empty = { zstd_input, 0, 0 };
                ZSTD_outBuffer output_tail = { zstd_output, sizeof(zstd_output), 0 };
                last_hint = ZSTD_decompressStream(decoder, &output_tail, &empty);
                if (ZSTD_isError(last_hint)) { ok = 0; break; }
                size_t tail_consumed = 0;
                while (tail_consumed < output_tail.pos) {
                    size_t count = EXT4_BLOCK - pending;
                    if (count > output_tail.pos - tail_consumed)
                        count = output_tail.pos - tail_consumed;
                    for (size_t i = 0; i < count; ++i)
                        file_block[pending + i] = zstd_output[tail_consumed + i];
                    pending += count;
                    tail_consumed += count;
                    if (pending == EXT4_BLOCK) {
                        if (!ext4_write_block(output_index++, file_block, EXT4_BLOCK) ||
                            total_output > UINT32_MAX - EXT4_BLOCK) {
                            last_error = ARCHPKG_ERROR_TEMP_WRITE; ok = 0; break;
                        }
                        total_output += EXT4_BLOCK;
                        pending = 0;
                    }
                }
                if (!ok || (last_hint != 0 && output_tail.pos < output_tail.size)) { ok = 0; break; }
            }
            if (!ok) break;
            saw_frame = 1;
        }
    }
    if (ok && (!saw_frame || last_hint != 0)) ok = 0;
    if (ok && pending) {
        if (!ext4_write_block(output_index, file_block, pending) || total_output > UINT32_MAX - pending) {
            last_error = ARCHPKG_ERROR_TEMP_WRITE; ok = 0;
        }
        else total_output += (uint32_t)pending;
    }
    if (ok && !ext4_write_finish(total_output)) {
        last_error = ARCHPKG_ERROR_TEMP_WRITE; ok = 0;
    }
    else if (!ok) (void)ext4_write_finish(0);

    ext4_read_end();
    if (decoder) (void)ZSTD_freeDStream(decoder);
    if (ok) ok = archpkg_install_tar("/var/lib/pacman/.archpkg-unpack.tar");
    (void)ext4_chdir("/var/lib/pacman");
    (void)ext4_unlink(temporary_tar);
    (void)ext4_chdir(previous_directory);
    return ok;
}

static int remove_manifest_path(const char *path, char kind)
{
    char parent[PATH_CAP], leaf[64];
    if (!split_path(path, parent, leaf) || !ext4_chdir("/") ||
        (parent[0] && !ext4_chdir(parent))) return 0;
    if (kind == 'd') {
        (void)ext4_rmdir(leaf); /* Keep directories shared with other packages. */
        return 1;
    }
    return ext4_unlink(leaf);
}

int archpkg_remove_files(const char *package_name)
{
    if (!valid_package_name(package_name)) return 0;
    char original[256], manifest_name[80];
    if (!ext4_pwd(original, sizeof(original)) || !ext4_chdir("/var/lib/pacman")) return 0;
    size_t name_length = 0;
    while (package_name[name_length]) { manifest_name[name_length] = package_name[name_length]; ++name_length; }
    const char suffix[] = ".files";
    for (size_t i = 0; i < sizeof(suffix); ++i) manifest_name[name_length + i] = suffix[i];

    uint32_t manifest_size = 0;
    if (!ext4_read_start(manifest_name, &manifest_size) || manifest_size > sizeof(package_manifest)) {
        if (manifest_size > sizeof(package_manifest)) ext4_read_end();
        (void)ext4_chdir(original);
        return 0;
    }
    for (uint32_t offset = 0; offset < manifest_size;) {
        uint8_t block[EXT4_BLOCK];
        if (!ext4_read_block(offset / EXT4_BLOCK, block)) {
            ext4_read_end();
            (void)ext4_chdir(original);
            return 0;
        }
        uint32_t count = manifest_size - offset;
        if (count > EXT4_BLOCK) count = EXT4_BLOCK;
        for (uint32_t i = 0; i < count; ++i) package_manifest[offset + i] = (char)block[i];
        offset += count;
    }
    ext4_read_end();

    int ok = 1;
    uint32_t end = manifest_size;
    while (end) {
        if (package_manifest[end - 1] == '\n') --end;
        uint32_t start = end;
        while (start && package_manifest[start - 1] != '\n') --start;
        if (end - start < 3 || package_manifest[start + 1] != ' ') { ok = 0; break; }
        char kind = package_manifest[start];
        if (kind != 'f' && kind != 'd' && kind != 'l' && kind != 'h') { ok = 0; break; }
        uint32_t path_length = end - start - 2;
        if (!path_length || path_length >= PATH_CAP) { ok = 0; break; }
        char path[PATH_CAP];
        for (uint32_t i = 0; i < path_length; ++i) path[i] = package_manifest[start + 2 + i];
        path[path_length] = 0;
        if (path[0] == '/' || !remove_manifest_path(path, kind)) { ok = 0; break; }
        end = start;
    }
    if (ok) {
        (void)ext4_chdir("/var/lib/pacman");
        if (!ext4_unlink(manifest_name)) ok = 0;
    }
    (void)ext4_chdir(original);
    return ok;
}
