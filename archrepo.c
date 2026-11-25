#include <stddef.h>
#include <stdint.h>
#include "archrepo.h"
#include "archpkg.h"
#include "ext4.h"
#include "net.h"
#include "pacman.h"

#define JSON_CAPACITY 16384u
#define PACKAGE_CAPACITY (64u * 1024u * 1024u)
#define PACKAGE_LIMIT 48u
#define DEPENDENCY_LIMIT 32u

typedef struct {
    char data[JSON_CAPACITY];
    size_t length;
} json_response_t;

typedef struct {
    uint8_t block[1024];
    uint16_t used;
    uint32_t block_index, total;
    int failed;
} package_writer_t;

static char requested_packages[PACKAGE_LIMIT][64];
static size_t requested_count;
static archrepo_log_fn report;
static int last_error;

int archrepo_last_error(void) { return last_error; }

static void set_error(int error)
{
    if (last_error == ARCHREPO_ERROR_NONE) last_error = error;
}

static int same_text(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

static int safe_component(const char *text)
{
    size_t n = 0;
    if (!text || !*text) return 0;
    while (text[n]) {
        char c = text[n++];
        if (n > 63 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '+')) return 0;
    }
    return 1;
}

static int safe_archive_filename(const char *text)
{
    size_t n = 0;
    if (!text || !*text) return 0;
    while (text[n]) {
        char c = text[n++];
        if (n > 127 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '+' || c == ':')) return 0;
    }
    return 1;
}

static int json_sink(const uint8_t *data, uint16_t length, void *context)
{
    json_response_t *response = context;
    if (length > sizeof(response->data) - 1 - response->length) return 0;
    for (uint16_t i = 0; i < length; ++i) response->data[response->length++] = (char)data[i];
    response->data[response->length] = 0;
    return 1;
}

static int package_sink(const uint8_t *data, uint16_t length, void *context)
{
    package_writer_t *writer = context;
    if (writer->total > PACKAGE_CAPACITY - length) { writer->failed = 1; return 0; }
    for (uint16_t i = 0; i < length; ++i) {
        writer->block[writer->used++] = data[i];
        if (writer->used == sizeof(writer->block)) {
            if (!ext4_write_block(writer->block_index++, writer->block, sizeof(writer->block))) {
                writer->failed = 1; return 0;
            }
            writer->used = 0;
        }
    }
    writer->total += length;
    return 1;
}

static int package_finish(package_writer_t *writer)
{
    if (writer->used && !writer->failed) {
        if (!ext4_write_block(writer->block_index, writer->block, writer->used)) writer->failed = 1;
    }
    int ok = !writer->failed && writer->total != 0 && ext4_write_finish(writer->total);
    if (!ok) (void)ext4_write_finish(0);
    return ok;
}

static const char *json_key(const char *json, const char *key)
{
    size_t key_length = 0;
    while (key[key_length]) ++key_length;
    const char *p = json;
    while (*p) {
        if (*p++ != '"') continue;
        const char *start = p;
        int escaped = 0;
        while (*p) {
            if (!escaped && *p == '"') break;
            if (!escaped && *p == '\\') escaped = 1;
            else escaped = 0;
            ++p;
        }
        if (!*p) return 0;
        const char *end = p++;
        if (!escaped && (size_t)(end - start) == key_length) {
            size_t i = 0;
            while (i < key_length && start[i] == key[i]) ++i;
            if (i == key_length) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
                if (*p++ != ':') continue;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
                return p;
            }
        }
    }
    return 0;
}

static int json_string(const char *value, char *output, size_t capacity)
{
    size_t used = 0;
    if (!value || *value++ != '"') return 0;
    while (*value && *value != '"') {
        char c = *value++;
        if (c == '\\') {
            c = *value++;
            if (!c) return 0;
            if (c == '"' || c == '\\' || c == '/') { }
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else return 0;
        }
        if (used + 1 >= capacity) return 0;
        output[used++] = c;
    }
    if (*value != '"') return 0;
    output[used] = 0;
    return 1;
}

static int copy_dependency_name(const char *value, char *name, size_t capacity)
{
    size_t n = 0;
    while (value[n] && value[n] != '>' && value[n] != '<' && value[n] != '=' && value[n] != ':') ++n;
    if (!n || n >= capacity) return 0;
    for (size_t i = 0; i < n; ++i) name[i] = value[i];
    name[n] = 0;
    /* Shared-library sonames are provided by packages, not named repository packages. */
    for (size_t i = 0; i + 2 < n; ++i)
        if (name[i] == '.' && name[i + 1] == 's' && name[i + 2] == 'o') return 0;
    /* Arch's bash package provides the virtual dependency named "sh". */
    if (same_text(name, "sh")) {
        name[0] = 'b'; name[1] = 'a'; name[2] = 's'; name[3] = 'h'; name[4] = 0;
    }
    return safe_component(name);
}

static int fetch_metadata(const char *name, char *filename, size_t filename_capacity,
                          char *repository, size_t repository_capacity,
                          char dependencies[DEPENDENCY_LIMIT][64], size_t *dependency_count)
{
    static const char *const repositories[] = {"extra", "core"};
    static const char *const architectures[] = {"x86_64", "any"};
    char path[256];
    json_response_t response;
    uint16_t status = 0;
    uint32_t length = 0;
    *dependency_count = 0;
    for (size_t r = 0; r < sizeof(repositories) / sizeof(repositories[0]); ++r) {
        for (size_t a = 0; a < sizeof(architectures) / sizeof(architectures[0]); ++a) {
            size_t n = 0;
            const char *parts[] = {"/packages/", repositories[r], "/", architectures[a], "/", name, "/json/"};
            for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p)
                for (size_t i = 0; parts[p][i] && n + 1 < sizeof(path); ++i) path[n++] = parts[p][i];
            if (n + 1 >= sizeof(path)) return 0;
            path[n] = 0;
            response.length = 0; response.data[0] = 0; status = 0; length = 0;
            if (!net_https_request("archlinux.org", path, "GET", 0, 0, 0,
                                   json_sink, &response, &length, &status) || status != 200) continue;
            const char *field = json_key(response.data, "filename");
            if (!json_string(field, filename, filename_capacity) || !safe_archive_filename(filename)) continue;
            size_t fl = 0; while (filename[fl]) ++fl;
            if (fl < 12 || !same_text(filename + fl - 12, ".pkg.tar.zst")) continue;
            size_t rl = 0; while (repositories[r][rl]) ++rl;
            if (rl + 1 > repository_capacity) return 0;
            for (size_t i = 0; i <= rl; ++i) repository[i] = repositories[r][i];
            const char *array = json_key(response.data, "depends");
            if (array && *array == '[') {
                ++array;
                while (*array && *array != ']' && *dependency_count < DEPENDENCY_LIMIT) {
                    while (*array == ' ' || *array == '\t' || *array == '\r' || *array == '\n' || *array == ',') ++array;
                    if (*array == ']') break;
                    char item[128], dependency[64];
                    if (!json_string(array, item, sizeof(item))) break;
                    if (copy_dependency_name(item, dependency, sizeof(dependency))) {
                        size_t i = 0; while (dependency[i]) { dependencies[*dependency_count][i] = dependency[i]; ++i; }
                        dependencies[*dependency_count][i] = 0;
                        ++*dependency_count;
                    }
                    while (*array && *array != '"') ++array;
                    if (*array) ++array;
                    while (*array && *array != ',' && *array != ']') ++array;
                }
            }
            return 1;
        }
    }
    return 0;
}

static int ensure_cache_directory(void)
{
    static const char *const components[] = {"/", "var", "cache", "pacman", "pkg"};
    if (!ext4_chdir("/")) return 0;
    for (size_t i = 1; i < sizeof(components) / sizeof(components[0]); ++i) {
        if (!ext4_chdir(components[i]) && (!ext4_mkdir(components[i]) || !ext4_chdir(components[i]))) return 0;
    }
    return 1;
}

static int install_one(const char *name, unsigned depth)
{
    if (!safe_component(name) || depth >= 16) { set_error(ARCHREPO_ERROR_PACKAGE_NAME); return 0; }
    int installed = pacman_package_installed(name);
    if (installed > 0) return 1;
    if (installed < 0) { set_error(ARCHREPO_ERROR_PACKAGE_DATABASE); return 0; }
    for (size_t i = 0; i < requested_count; ++i)
        if (same_text(requested_packages[i], name)) return 1;
    if (requested_count == PACKAGE_LIMIT) { set_error(ARCHREPO_ERROR_DEPENDENCY); return 0; }
    size_t name_length = 0; while (name[name_length]) ++name_length;
    for (size_t i = 0; i <= name_length; ++i) requested_packages[requested_count][i] = name[i];
    ++requested_count;
    if (report) report(name);

    char filename[128], repository[16], dependencies[DEPENDENCY_LIMIT][64];
    size_t dependency_count = 0;
    if (!fetch_metadata(name, filename, sizeof(filename), repository, sizeof(repository),
                        dependencies, &dependency_count)) { set_error(ARCHREPO_ERROR_METADATA); return 0; }
    for (size_t i = 0; i < dependency_count; ++i)
        if (!install_one(dependencies[i], depth + 1)) { set_error(ARCHREPO_ERROR_DEPENDENCY); return 0; }

    char previous[256], archive_path[256], request_path[256];
    if (!ext4_pwd(previous, sizeof(previous))) { set_error(ARCHREPO_ERROR_CACHE); return 0; }
    if (!ensure_cache_directory()) { (void)ext4_chdir(previous); set_error(ARCHREPO_ERROR_CACHE); return 0; }
    size_t n = 0;
    while (filename[n] && n + 1 < sizeof(archive_path)) { archive_path[n] = filename[n]; ++n; }
    if (filename[n]) { (void)ext4_chdir(previous); set_error(ARCHREPO_ERROR_CACHE); return 0; }
    archive_path[n] = 0;
    static const char *const mirror_hosts[] = {"geo.mirror.pkgbuild.com", "mirrors.kernel.org"};
    int package_ready = 0;
    for (size_t mirror = 0; mirror < sizeof(mirror_hosts) / sizeof(mirror_hosts[0]); ++mirror) {
        n = 0;
        const char *path_parts[4];
        size_t part_count = 0;
        if (mirror == 0) {
            path_parts[part_count++] = "/";
            path_parts[part_count++] = repository;
            path_parts[part_count++] = "/os/x86_64/";
        } else {
            path_parts[part_count++] = "/archlinux/";
            path_parts[part_count++] = repository;
            path_parts[part_count++] = "/os/x86_64/";
        }
        path_parts[part_count++] = filename;
        for (size_t p = 0; p < part_count; ++p)
            for (size_t i = 0; path_parts[p][i] && n + 1 < sizeof(request_path); ++i)
                request_path[n++] = path_parts[p][i];
        if (n + 1 >= sizeof(request_path)) {
            (void)ext4_chdir(previous);
            set_error(ARCHREPO_ERROR_DOWNLOAD); return 0;
        }
        request_path[n] = 0;
        for (uint8_t attempt = 0; attempt < 2 && !package_ready; ++attempt) {
            package_writer_t writer = {0};
            if (!ext4_write_begin(archive_path)) {
                (void)ext4_chdir(previous); set_error(ARCHREPO_ERROR_CACHE); return 0;
            }
            uint16_t status = 0; uint32_t received = 0;
            int downloaded = net_https_request(mirror_hosts[mirror], request_path, "GET", 0, 0, 0,
                                                package_sink, &writer, &received, &status);
            int saved = package_finish(&writer);
            package_ready = downloaded && status == 200 && saved && received == writer.total;
        }
        if (package_ready) break;
    }
    (void)ext4_chdir(previous);
    if (!package_ready) {
        set_error(ARCHREPO_ERROR_DOWNLOAD);
        return 0;
    }

    n = 0;
    const char *prefix = "/var/cache/pacman/pkg/";
    while (prefix[n]) { archive_path[n] = prefix[n]; ++n; }
    for (size_t i = 0; filename[i] && n + 1 < sizeof(archive_path); ++i) archive_path[n++] = filename[i];
    if (n + 1 >= sizeof(archive_path)) { set_error(ARCHREPO_ERROR_CACHE); return 0; }
    archive_path[n] = 0;
    if (!archpkg_install_zstd(archive_path)) { set_error(ARCHREPO_ERROR_EXTRACT); return 0; }
    char installed_name[64], version[96];
    if (!archpkg_package_info(installed_name, sizeof(installed_name), version, sizeof(version)) ||
        !same_text(installed_name, name)) { set_error(ARCHREPO_ERROR_PACKAGE_INFO); return 0; }
    if (pacman_register_arch(installed_name, version) == 1 || pacman_package_installed(installed_name) > 0)
        return 1;
    set_error(ARCHREPO_ERROR_REGISTER);
    return 0;
}

int archrepo_install(const char *package_name, archrepo_log_fn log)
{
    requested_count = 0;
    last_error = ARCHREPO_ERROR_NONE;
    report = log;
    int result = install_one(package_name, 0);
    if (!result && last_error == ARCHREPO_ERROR_NONE) last_error = ARCHREPO_ERROR_DEPENDENCY;
    report = 0;
    return result;
}
