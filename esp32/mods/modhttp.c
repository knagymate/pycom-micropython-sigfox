/*
 * Copyright (c) 2019, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */


#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/gc.h"

#include "esp_err.h"
#include "esp_http_server.h"

#include "modhttp.h"


/******************************************************************************
 DEFINE CONSTANTS
 ******************************************************************************/

/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/

typedef struct mod_http_server_obj_s {
    httpd_handle_t server;
}mod_http_server_obj_t;

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/

/******************************************************************************
 DEFINE PRIVATE VARIABLES
 ******************************************************************************/

// There can only be 1 server instance
STATIC mod_http_server_obj_t* server_obj;

/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/

/******************************************************************************
 DEFINE HTTP SERVER CLASS FUNCTIONS
 ******************************************************************************/

// Initialize the http server module
STATIC mp_obj_t mod_http_server_init(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    const mp_arg_t mod_http_server_init_args[] = {
            { MP_QSTR_port,                     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}}
    };


    MP_STATE_PORT(http_server_ptr) = m_malloc(sizeof(mod_http_server_obj_t));
    server_obj = MP_STATE_PORT(http_server_ptr);

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_http_server_init_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_http_server_init_args, args);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if(args[0].u_obj != mp_const_none) {
        config.server_port = args[0].u_int;
    }

    esp_err_t ret = httpd_start(&server_obj->server, &config);
    if(ret != ESP_OK) {

    }

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_http_server_init_obj, 0, mod_http_server_init);


STATIC const mp_map_elem_t mod_http_server_globals_table[] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                        MP_OBJ_NEW_QSTR(MP_QSTR_HTTP_Server) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_init),                            (mp_obj_t)&mod_http_server_init_obj },
};

STATIC MP_DEFINE_CONST_DICT(mod_http_server_globals, mod_http_server_globals_table);

const mp_obj_module_t mod_http_server = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mod_http_server_globals,
};
