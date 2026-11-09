#include <stdint.h>
#include <stddef.h>
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "net.h"

#define HTTP_REQUEST_MAX 2048u
#define HTTP_BODY_MAX 16384u

static int send_all(const uint8_t *data, size_t length) {
    while (length) {
        uint16_t count = length > 1460 ? 1460 : (uint16_t)length;
        if (!net_tcp_send(data, count)) return 0;
        data += count;
        length -= count;
    }
    return 1;
}

static mp_obj_t webserver_serve(size_t n_args, const mp_obj_t *args) {
    mp_obj_t handler = args[0];
    mp_int_t port = n_args > 1 ? mp_obj_get_int(args[1]) : 8070;
    if (port < 1 || port > 65535) mp_raise_ValueError(MP_ERROR_TEXT("invalid port"));
    mp_printf(&mp_plat_print, "HTTP server listening on port %d\n", (int)port);
    for (;;) {
        if (!net_tcp_accept((uint16_t)port)) continue;
        char request[HTTP_REQUEST_MAX]; size_t used = 0; int complete = 0;
        while (used < sizeof(request) - 1 && !complete) {
            uint8_t chunk[512]; uint16_t got = 0;
            int result = net_tcp_receive(chunk, sizeof(chunk), &got);
            if (result <= 0) break;
            for (uint16_t i = 0; i < got && used < sizeof(request) - 1; ++i) {
                request[used++] = (char)chunk[i];
                if (used >= 4 && request[used-4]=='\r' && request[used-3]=='\n' && request[used-2]=='\r' && request[used-1]=='\n') { complete = 1; break; }
            }
        }
        request[used] = 0;
        if (complete) {
            char *space1 = NULL, *space2 = NULL;
            for (size_t i = 0; i < used && request[i] != '\r'; ++i) {
                if (request[i] == ' ' && !space1) space1 = &request[i];
                else if (request[i] == ' ' && space1) { space2 = &request[i]; break; }
            }
            if (space1 && space2) {
                *space1 = 0; *space2 = 0;
                mp_obj_t call_args[2] = { mp_obj_new_str(request, space1 - request), mp_obj_new_str(space1 + 1, space2 - space1 - 1) };
                mp_obj_t response = mp_call_function_n_kw(handler, 2, 0, call_args);
                size_t body_length; const char *body = mp_obj_str_get_data(response, &body_length);
                if (body_length > HTTP_BODY_MAX) body_length = HTTP_BODY_MAX;
                char header[112]; size_t h = 0;
                const char prefix[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
                for (size_t i = 0; i < sizeof(prefix)-1; ++i) header[h++] = prefix[i];
                char digits[10]; size_t nd = 0, value = body_length;
                do { digits[nd++] = '0' + value % 10; value /= 10; } while (value);
                while (nd) header[h++] = digits[--nd];
                const char suffix[] = "\r\nConnection: close\r\n\r\n";
                for (size_t i = 0; i < sizeof(suffix)-1; ++i) header[h++] = suffix[i];
                (void)send_all((const uint8_t *)header, h);
                (void)send_all((const uint8_t *)body, body_length);
            }
        }
        (void)net_tcp_close();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(webserver_serve_obj, 1, 2, webserver_serve);
static const mp_rom_map_elem_t webserver_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webserver) },
    { MP_ROM_QSTR(MP_QSTR_serve), MP_ROM_PTR(&webserver_serve_obj) },
};
static MP_DEFINE_CONST_DICT(webserver_globals, webserver_globals_table);
const mp_obj_module_t maios_webserver_module = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&webserver_globals };
MP_REGISTER_MODULE(MP_QSTR_webserver, maios_webserver_module);
