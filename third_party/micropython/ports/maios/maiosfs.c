#include <stddef.h>
#include <stdint.h>
#include "ext4.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#define MAIOSFS_MAX_FILE_SIZE (64u * 1024u)
#define MAIOSFS_MAX_NAME 63u

static void checked_name(mp_obj_t obj, char output[MAIOSFS_MAX_NAME + 1]) {
    size_t length;
    const char *name = mp_obj_str_get_data(obj, &length);
    if (!length || length > MAIOSFS_MAX_NAME) {
        mp_raise_ValueError(MP_ERROR_TEXT("filename must be 1-63 characters"));
    }
    for (size_t i = 0; i < length; ++i) {
        if (name[i] == '/' || name[i] == '\\' || name[i] == 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("use a filename in the current directory"));
        }
    }
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid filename"));
    }
    for (size_t i = 0; i < length; ++i) output[i] = name[i];
    output[length] = 0;
}

static mp_obj_t maiosfs_read(mp_obj_t filename_obj) {
    char filename[MAIOSFS_MAX_NAME + 1];
    checked_name(filename_obj, filename);
    uint32_t size;
    if (!ext4_read_start(filename, &size)) mp_raise_OSError(MP_ENOENT);
    if (size > MAIOSFS_MAX_FILE_SIZE) {
        ext4_read_end();
        mp_raise_OSError(MP_EINVAL);
    }
    uint8_t *contents = m_new(uint8_t, size ? size : 1);
    for (uint32_t offset = 0, block = 0; offset < size; ++block) {
        uint8_t chunk[1024];
        if (!ext4_read_block(block, chunk)) {
            ext4_read_end();
            m_del(uint8_t, contents, size ? size : 1);
            mp_raise_OSError(MP_EIO);
        }
        uint32_t count = size - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        for (uint32_t i = 0; i < count; ++i) contents[offset + i] = chunk[i];
        offset += count;
    }
    ext4_read_end();
    mp_obj_t result = mp_obj_new_bytes(contents, size);
    m_del(uint8_t, contents, size ? size : 1);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(maiosfs_read_obj, maiosfs_read);

static mp_obj_t maiosfs_write(mp_obj_t filename_obj, mp_obj_t data_obj) {
    char filename[MAIOSFS_MAX_NAME + 1];
    checked_name(filename_obj, filename);
    size_t size;
    const char *data = mp_obj_str_get_data(data_obj, &size);
    if (size > MAIOSFS_MAX_FILE_SIZE) mp_raise_OSError(MP_EINVAL);
    if (!ext4_write_begin(filename)) mp_raise_OSError(MP_EIO);
    uint32_t blocks = (size + 1023u) / 1024u;
    for (uint32_t block = 0; block < blocks; ++block) {
        uint8_t chunk[1024];
        size_t offset = (size_t)block * sizeof(chunk);
        size_t count = size - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        for (size_t i = 0; i < count; ++i) chunk[i] = (uint8_t)data[offset + i];
        if (!ext4_write_block(block, chunk, count)) {
            (void)ext4_write_finish((uint32_t)((size_t)block * sizeof(chunk)));
            mp_raise_OSError(MP_EIO);
        }
    }
    if (!ext4_write_finish((uint32_t)size)) mp_raise_OSError(MP_EIO);
    return mp_obj_new_int_from_uint(size);
}
static MP_DEFINE_CONST_FUN_OBJ_2(maiosfs_write_obj, maiosfs_write);

static mp_obj_t maiosfs_remove(mp_obj_t filename_obj) {
    char filename[MAIOSFS_MAX_NAME + 1];
    checked_name(filename_obj, filename);
    if (!ext4_unlink(filename)) mp_raise_OSError(MP_ENOENT);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(maiosfs_remove_obj, maiosfs_remove);

static const mp_rom_map_elem_t maiosfs_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_maiosfs) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&maiosfs_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&maiosfs_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&maiosfs_remove_obj) },
};
static MP_DEFINE_CONST_DICT(maiosfs_globals, maiosfs_globals_table);
const mp_obj_module_t maiosfs_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&maiosfs_globals,
};
MP_REGISTER_MODULE(MP_QSTR_maiosfs, maiosfs_module);
