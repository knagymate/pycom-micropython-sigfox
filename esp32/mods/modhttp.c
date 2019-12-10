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

#include "mpthreadport.h"
#include "mpirq.h"

#include "esp_err.h"
#include "esp_http_server.h"

#include "modhttp.h"


/******************************************************************************
 DEFINE CONSTANTS
 ******************************************************************************/

#define MOD_HTTP_GET        (1)
#define MOD_HTTP_PUT        (2)
#define MOD_HTTP_POST       (4)
#define MOD_HTTP_DELETE     (8)



/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/
typedef struct mod_http_resource_obj_s {
    mp_obj_base_t base;
    struct mod_http_resource_obj_s* next;
    uint8_t* value;
    uint16_t value_len;
    const char* uri;
}mod_http_resource_obj_t;

typedef struct mod_http_server_obj_s {
    httpd_handle_t server;
    mod_http_resource_obj_t* resources;
}mod_http_server_obj_t;

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/
STATIC mod_http_resource_obj_t* find_resource(const char* uri);
STATIC mod_http_resource_obj_t* add_resource(const char* uri, mp_obj_t value);
STATIC void remove_resource(const char* uri);
STATIC void resource_update_value(mod_http_resource_obj_t* resource, mp_obj_t new_value);
STATIC esp_err_t mod_http_resource_callback_helper(mod_http_resource_obj_t* resource , httpd_method_t method, mp_obj_t callback, bool action);

STATIC void mod_http_server_callback_handler(void *arg_in);
STATIC esp_err_t mod_http_server_callback(httpd_req_t *r);

/******************************************************************************
 DEFINE PRIVATE VARIABLES
 ******************************************************************************/

// There can only be 1 server instance
STATIC mod_http_server_obj_t* server_obj = NULL;
STATIC bool server_initialized = false;

STATIC const mp_obj_type_t mod_http_resource_type;



/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/
// Get the resource if exists
STATIC mod_http_resource_obj_t* find_resource(const char* uri) {

    if(server_obj->resources != NULL) {
        mod_http_resource_obj_t* current = server_obj->resources;
        for(; current != NULL; current = current->next) {
            // Compare the Uri
            if(strcmp(current->uri, uri) == 0) {
                return current;
            }
        }
    }
    return NULL;
}

// Create a new resource in the scope of the only context
STATIC mod_http_resource_obj_t* add_resource(const char* uri, mp_obj_t value) {

    // Resource does not exist, create a new resource object
    mod_http_resource_obj_t* resource = m_new_obj(mod_http_resource_obj_t);
    resource->base.type = &mod_http_resource_type;

    // No next elem
    resource->next = NULL;

    // uri parameter pointer will be destroyed, pass a pointer to a permanent location
    resource->uri = m_malloc(strlen(uri));
    memcpy((char*)resource->uri, uri, strlen(uri));

    // If no default value is given set it to 0
    if(value == MP_OBJ_NULL) {
        value = mp_obj_new_int(0);
    }

    // Initialize default value
    resource_update_value(resource, value);

    // Add the resource to HTTP Server
    if(server_obj->resources == NULL) {
        // No resource exists, add as first element
        server_obj->resources = resource;
    }
    else {
        mod_http_resource_obj_t* current = server_obj->resources;
        // Find the last resource
        for(; current->next != NULL; current = current->next) {}
        // Append the new resource to the end of the list
        current->next = resource;
    }

    return resource;
}

// Remove the resource in the scope of the only context by its key
STATIC void remove_resource(const char* uri) {

    if(server_obj->resources != NULL) {
        mod_http_resource_obj_t* current = server_obj->resources;
        mod_http_resource_obj_t* previous = server_obj->resources;
        for(; current != NULL; current = current->next) {

            // Compare the URI
            if(strcmp(current->uri, uri) == 0) {
                // Resource found, remove from the list
                // Check if it is the first element in the list
                if(server_obj->resources == current) {
                    // If no more element in the list then invalidate the list
                    if(current->next == NULL) {
                        server_obj->resources = NULL;
                    }
                    // Other elements are in the list
                    else {
                        server_obj->resources = current->next;
                    }
                }
                else {
                    // It is not the first element
                    previous->next = current->next;
                }

                // Free the URI
                m_free((char*)current->uri);
                // Free the element in MP scope
                m_free(current->value);
                // Free the resource itself
                m_free(current);

                return;
            }

            // Mark the current element as previous, needed when removing the actual current element from the list
            previous = current;
        }
    }
}

// Update the value of a resource
STATIC void resource_update_value(mod_http_resource_obj_t* resource, mp_obj_t new_value) {

    // Invalidate current data first
    resource->value_len = 0;
    m_free(resource->value);

    if (mp_obj_is_integer(new_value)) {

        uint32_t value = mp_obj_get_int_truncated(new_value);
        if (value > 0xFF) {
            resource->value_len = 2;
        } else if (value > 0xFFFF) {
            resource->value_len = 4;
        } else {
            resource->value_len = 1;
        }

        // Allocate memory for the new data
        resource->value = m_malloc(resource->value_len);
        memcpy(resource->value, &value, sizeof(value));

    } else {

        mp_buffer_info_t value_bufinfo;
        mp_get_buffer_raise(new_value, &value_bufinfo, MP_BUFFER_READ);
        resource->value_len = value_bufinfo.len;

        // Allocate memory for the new data
        resource->value = m_malloc(resource->value_len);
        memcpy(resource->value, value_bufinfo.buf, resource->value_len);
    }
}

STATIC esp_err_t mod_http_resource_callback_helper(mod_http_resource_obj_t* resource , httpd_method_t method, mp_obj_t callback, bool action){

    esp_err_t ret = ESP_OK;

    if(action == true) {

        httpd_uri_t uri;
        // Set the URI
        uri.uri = resource->uri;
        // Save the user's callback into user context field for future usage
        uri.user_ctx = callback;
        // The registered handler is our own handler which will handle different requests and call user's callback, if any
        uri.handler = mod_http_server_callback;

        if((method & MOD_HTTP_GET) && (ret == ESP_OK)) {
            uri.method = HTTP_GET;
            ret = httpd_register_uri_handler(server_obj->server, &uri);
        }

        if((method & MOD_HTTP_PUT) && (ret == ESP_OK)) {
            uri.method = HTTP_PUT;
            ret = httpd_register_uri_handler(server_obj->server, &uri);
        }

        if((method & MOD_HTTP_POST) && (ret == ESP_OK)) {
            uri.method = HTTP_POST;
            ret = httpd_register_uri_handler(server_obj->server, &uri);
        }

        if((method & MOD_HTTP_DELETE) && (ret == ESP_OK)) {
            uri.method = HTTP_DELETE;
            ret = httpd_register_uri_handler(server_obj->server, &uri);
        }
    }
    else {

        if((method & MOD_HTTP_GET) && (ret == ESP_OK)) {
            ret = httpd_unregister_uri_handler(server_obj->server, resource->uri, HTTP_GET);
        }

        if((method & MOD_HTTP_PUT) && (ret == ESP_OK)) {
            ret = httpd_unregister_uri_handler(server_obj->server, resource->uri, HTTP_PUT);
        }

        if((method & MOD_HTTP_POST) && (ret == ESP_OK)) {
            ret = httpd_unregister_uri_handler(server_obj->server, resource->uri, HTTP_POST);
        }

        if((method & MOD_HTTP_DELETE) && (ret == ESP_OK)) {
            ret = httpd_unregister_uri_handler(server_obj->server, resource->uri, HTTP_DELETE);
        }
    }

    return ret;
}

STATIC void mod_http_server_callback_handler(void *arg_in) {

    /* The received arg_in is a tuple with 4 elements
     * 0 - user's MicroPython callback
     * 1 - URI as string
     * 2 - HTTP method as INT
     * 3 - Content of the method as a string (if any)
     */

    mp_obj_t args[3];
    // URI
    args[0] = ((mp_obj_tuple_t*)arg_in)->items[1];
    // method
    args[1] = ((mp_obj_tuple_t*)arg_in)->items[2];
    // Content
    args[2] = ((mp_obj_tuple_t*)arg_in)->items[3];

    // Call the user registered MicroPython function
    mp_call_function_n_kw(((mp_obj_tuple_t*)arg_in)->items[0], 3, 0, args);

}

STATIC esp_err_t mod_http_server_callback(httpd_req_t *r){

    char* content = NULL;
    mp_obj_t args[4];

    mod_http_resource_obj_t* resource = find_resource(r->uri);

    // If the resource does not exist anymore then send back 404
    if(resource == NULL){
        httpd_resp_send_404(r);
        // This can happen is locally the resource has been removed but for some reason it still exists in the HTTP Server library context...
        return ESP_FAIL;
    }

    // Get the content part of the message
    if(r->content_len > 0) {
        // Allocate memory for the content
        content = m_malloc(r->content_len);
        if(content != NULL) {
            // Get the content from the message
            int recv_length = httpd_req_recv(r, content, r->content_len);

            // If return value less than 0, error occurred
            if(recv_length < 0) {
                // Free up local content, no longer needed
                m_free(content);

                // Check if timeout error occurred and send back appropriate response
                if (recv_length == HTTPD_SOCK_ERR_TIMEOUT) {
                    httpd_resp_send_408(r);
                }

                //TODO: ESP_FAIL should be returned as per HTTP LIB, check if exception is needed
                return ESP_FAIL;
                //nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "HTTP Server could not be initialized, error code: %d", recv_length));
            }

            if(recv_length != r->content_len) {
                //TODO: Handle this case properly
                printf("recv_length != r->content_len !!\n");
            }
        }
        else {
            //TODO: ESP_FAIL should be returned as per HTTP LIB, check if exception is needed
            return ESP_FAIL;
            //nlr_raise(mp_obj_new_exception_msg(&mp_type_MemoryError, "Not enough free memory to handle request to HTTP Server!"));
        }
    }

    // This is a GET request, send back the current value of the resource
    if(r->method == HTTP_GET) {

        //TODO: handle the representation format
        httpd_resp_send(r, (const char*)resource->value, (ssize_t)resource->value_len);
    }
    // This is a POST request
    else if(r->method == HTTP_POST) {
        // Update the resource if new value is provided
        if(content != NULL) {
            // Update the resource
            resource_update_value(resource, mp_obj_new_str(content, r->content_len));
            //TODO: compose here a better message
            const char resp[] = "Resource is updated.";
            httpd_resp_send(r, resp, strlen(resp));
        }
    }

    // If there is a registered MP callback for this resource the parameters need to be prepared
    if(r->user_ctx != MP_OBJ_NULL) {
        // The MicroPython callback is stored in user_ctx
        args[0] = r->user_ctx;
        args[1] = mp_obj_new_str(r->uri, strlen(r->uri));
        args[2] = mp_obj_new_int(r->method);
        args[3] = mp_obj_new_str(content, r->content_len);

        // The user registered MicroPython callback will be called decoupled from the HTTP Server context in the IRQ Task
        mp_irq_queue_interrupt(mod_http_server_callback_handler, (void *)mp_obj_new_tuple(4, args));
    }

    // Free up local content, no longer needed
    m_free(content);

    return ESP_OK;
}


/******************************************************************************
 DEFINE HTTP RESOURCE CLASS FUNCTIONS
 ******************************************************************************/
// Gets or sets the value of a resource
STATIC mp_obj_t mod_http_resource_value(mp_uint_t n_args, const mp_obj_t *args) {

    mod_http_resource_obj_t* self = (mod_http_resource_obj_t*)args[0];
    mp_obj_t ret = mp_const_none;

    // If the value exists, e.g.: not deleted from another task before we got the semaphore
    if(self->value != NULL) {
        if (n_args == 1) {
            // get
            ret = mp_obj_new_bytes(self->value, self->value_len);
        } else {
            // set
            resource_update_value(self, (mp_obj_t)args[1]);
        }
    }
    return ret;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_http_resource_value_obj, 1, 2, mod_http_resource_value);

// Sets or removes the callback on a given method
STATIC mp_obj_t mod_http_resource_callback(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    const mp_arg_t mod_http_resource_callback_args[] = {
            { MP_QSTR_self,                    MP_ARG_OBJ  | MP_ARG_REQUIRED, },
            { MP_QSTR_method,                  MP_ARG_INT  | MP_ARG_REQUIRED, },
            { MP_QSTR_callback,                MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}},
            { MP_QSTR_action,                  MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_http_resource_callback_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_http_resource_callback_args, args);

    // Get the resource
    mod_http_resource_obj_t* self = (mod_http_resource_obj_t*)args[0].u_obj;
    // Get the method
    httpd_method_t method = args[1].u_int;
    // Get the callback
    mp_obj_t callback = args[2].u_obj;
    // Get the action
    bool action = args[3].u_bool;

    if(action == true && callback == MP_OBJ_NULL) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "If the \"action\" is TRUE then \"callback\" must be defined"));
    }

    esp_err_t  ret = mod_http_resource_callback_helper(self, method, callback, action);

    if(ret != ESP_OK) {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "Callback of the resource could not be updated, error code: %d!", ret));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_http_resource_callback_obj, 2, mod_http_resource_callback);

STATIC const mp_map_elem_t http_resource_locals_table[] = {
    // instance methods
        { MP_OBJ_NEW_QSTR(MP_QSTR_value),                       (mp_obj_t)&mod_http_resource_value_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_callback),                    (mp_obj_t)&mod_http_resource_callback_obj },

};
STATIC MP_DEFINE_CONST_DICT(http_resource_locals, http_resource_locals_table);

STATIC const mp_obj_type_t mod_http_resource_type = {
    { &mp_type_type },
    .name = MP_QSTR_HTTP_Resource,
    .locals_dict = (mp_obj_t)&http_resource_locals,
};


/******************************************************************************
 DEFINE HTTP SERVER CLASS FUNCTIONS
 ******************************************************************************/

// Initialize the http server module
STATIC mp_obj_t mod_http_server_init(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    const mp_arg_t mod_http_server_init_args[] = {
            { MP_QSTR_port,                     MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_obj = mp_const_none}}
    };

    if(server_initialized == false) {

        MP_STATE_PORT(http_server_ptr) = m_malloc(sizeof(mod_http_server_obj_t));
        server_obj = MP_STATE_PORT(http_server_ptr);

        mp_arg_val_t args[MP_ARRAY_SIZE(mod_http_server_init_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_http_server_init_args, args);

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();

        config.task_priority = MP_THREAD_PRIORITY;
        if(args[0].u_obj != mp_const_none) {
            config.server_port = args[0].u_int;
        }

        esp_err_t ret = httpd_start(&server_obj->server, &config);
        if(ret != ESP_OK) {
            m_free(server_obj);
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "HTTP Server could not be initialized, error code: %d", ret));
        }

        server_obj->resources = NULL;

        server_initialized = true;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "HTTP Server module is already initialized!"));
    }

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_http_server_init_obj, 0, mod_http_server_init);

// Adds a resource to the http server module
STATIC mp_obj_t mod_http_server_add_resource(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    const mp_arg_t mod_http_server_add_resource_args[] = {
            { MP_QSTR_uri,                     MP_ARG_OBJ  | MP_ARG_REQUIRED, },
            { MP_QSTR_value,                   MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}},
    };

    if(server_initialized == true) {

        mp_arg_val_t args[MP_ARRAY_SIZE(mod_http_server_add_resource_args)];
        mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_http_server_add_resource_args, args);

        mod_http_resource_obj_t* resource = MP_OBJ_NULL;
        esp_err_t ret = ESP_OK;
        httpd_uri_t uri;

        // Get the URI
        uri.uri = mp_obj_str_get_str(args[0].u_obj);

        if(find_resource(uri.uri) != NULL) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Resource already added!"));
        }

        // Create the resource in the esp-idf http server's context
        ret = httpd_register_uri_handler(server_obj->server, &uri);

        if(ret == ESP_OK) {
            // Add resource to MicroPython http server's context with default value
            resource = add_resource(uri.uri, args[1].u_obj);
        }

        if(ret != ESP_OK) {
            // Error occurred, remove the registered resource from MicroPython http server's context
            remove_resource(uri.uri);
            // Error occurred, remove the registered resource from esp-idf http server's context
            (void)httpd_unregister_uri(server_obj->server, uri.uri);
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "Resource could not be added, error code: %d!", ret));
        }

        return resource;
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "HTTP Server module is not initialized!"));
        // Just to fulfills the compiler's needs
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_http_server_add_resource_obj, 1, mod_http_server_add_resource);

// Removes a resource from the http server module
STATIC mp_obj_t mod_http_server_remove_resource(mp_obj_t uri_in) {

    if(server_initialized == true) {

        const char* uri = mp_obj_str_get_str(uri_in);

        if(find_resource(uri) == NULL) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Resource does not exist!"));
        }

        esp_err_t ret = httpd_unregister_uri(server_obj->server, uri);
        if(ret != ESP_OK) {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "Resource could not be removed, error code: %d!", ret));
        }

        remove_resource(uri);
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "HTTP Server module is not initialized!"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_http_server_remove_resource_obj, mod_http_server_remove_resource);


STATIC const mp_map_elem_t mod_http_server_globals_table[] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                        MP_OBJ_NEW_QSTR(MP_QSTR_HTTP_Server) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_init),                            (mp_obj_t)&mod_http_server_init_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_add_resource),                    (mp_obj_t)&mod_http_server_add_resource_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_remove_resource),                 (mp_obj_t)&mod_http_server_remove_resource_obj },

        // class constants
        { MP_OBJ_NEW_QSTR(MP_QSTR_GET),                      MP_OBJ_NEW_SMALL_INT(MOD_HTTP_GET) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_PUT),                      MP_OBJ_NEW_SMALL_INT(MOD_HTTP_PUT) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_POST),                     MP_OBJ_NEW_SMALL_INT(MOD_HTTP_POST) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_DELETE),                   MP_OBJ_NEW_SMALL_INT(MOD_HTTP_DELETE) },


};

STATIC MP_DEFINE_CONST_DICT(mod_http_server_globals, mod_http_server_globals_table);

const mp_obj_module_t mod_http_server = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mod_http_server_globals,
};
