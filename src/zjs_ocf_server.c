// Copyright (c) 2016, Intel Corporation.

#ifdef BUILD_MODULE_OCF

#include "oc_api.h"
#include "port/oc_clock.h"
//#include "port/oc_signal_main_loop.h"

#include "zjs_util.h"
#include "zjs_common.h"

#include "zjs_ocf_server.h"
#include "zjs_ocf_encoder.h"
#include "zjs_ocf_common.h"

#include "zjs_event.h"
#include "zjs_promise.h"

struct server_resource {
    /*
     * TODO: cant reference with 'this' because iotivity-constrained callbacks
     *       which trigger events need this. They are not JS API's that have
     *       'this' pointer so we have to save it in C.
     */
    jerry_value_t object;
    char* device_id;
    char* resource_path;
    uint32_t error_code;
    oc_resource_t *res;
};

struct ocf_response {
    oc_method_t method;         // Current method being executed
    oc_request_t* request;
    struct server_resource* res;
};

struct ocf_handler {
    oc_request_t *req;
    struct ocf_response* resp;
    struct server_resource* res;
};

#define FLAG_OBSERVE        1 << 0
#define FLAG_DISCOVERABLE   1 << 1
#define FLAG_SLOW           1 << 2
#define FLAG_SECURE         1 << 3

static struct ocf_handler* new_ocf_handler(struct server_resource* res)
{
    struct ocf_handler* h = zjs_malloc(sizeof(struct ocf_handler));
    if (!h) {
        ERR_PRINT("could not allocate OCF handle, out of memory\n");
        return NULL;
    }
    memset(h, 0, sizeof(struct ocf_handler));
    h->res = res;

    return h;
}

static void post_ocf_promise(void* handle)
{
    struct ocf_handler* h = (struct ocf_handler*)handle;
    if (h) {
        if (h->resp) {
            zjs_free(h->resp);
        }
        zjs_free(h);
    }
}

static jerry_value_t make_ocf_error(const char* name, const char* msg, struct server_resource* res)
{
    if (res) {
        jerry_value_t ret = jerry_create_object();
        if (name) {
            zjs_obj_add_string(ret, name, "name");
        } else {
            ERR_PRINT("error must have a name\n");
            jerry_release_value(ret);
            return ZJS_UNDEFINED;
        }
        if (msg) {
            zjs_obj_add_string(ret, msg, "message");
        } else {
            ERR_PRINT("error must have a message\n");
            jerry_release_value(ret);
            return ZJS_UNDEFINED;
        }
        if (res->device_id) {
            zjs_obj_add_string(ret, res->device_id, "deviceId");
        }
        if (res->resource_path) {
            zjs_obj_add_string(ret, res->resource_path, "resourcePath");
        }
        zjs_obj_add_number(ret, (double)res->error_code, "errorCode");

        return ret;
    } else {
        ERR_PRINT("client resource was NULL\n");
        return ZJS_UNDEFINED;
    }
}

static jerry_value_t request_to_jerry_value(oc_request_t *request)
{
    jerry_value_t props = jerry_create_object();
    oc_rep_t *rep = request->request_payload;
    while (rep != NULL) {
        switch (rep->type) {
        case BOOL:
            zjs_obj_add_boolean(props, rep->value.boolean, oc_string(rep->name));
            break;
        case INT:
            zjs_obj_add_number(props, rep->value.integer, oc_string(rep->name));
            break;
        case BYTE_STRING:
        case STRING:
            zjs_obj_add_string(props, oc_string(rep->value.string), oc_string(rep->name));
            break;
            /*
             * TODO: Implement encoding for complex types
             */
        case STRING_ARRAY:
        case OBJECT:
            ZJS_PRINT("{ Object }\n");
            break;
        default:
            break;
        }
        rep = rep->next;
    }
    return props;
}

struct server_resource* new_server_resource(char* path)
{
    struct server_resource* resource = zjs_malloc(sizeof(struct server_resource));
    memset(resource, 0, sizeof(struct server_resource));

    resource->resource_path = zjs_malloc(strlen(path));
    memcpy(resource->resource_path, path, strlen(path));
    resource->resource_path[strlen(path)] = '\0';

    return resource;
}

static struct ocf_response* create_response(struct server_resource* resource, oc_method_t method)
{
    struct ocf_response* resp = zjs_malloc(sizeof(struct ocf_response));
    memset(resp, 0, sizeof(struct ocf_response));
    resp->res = resource;
    resp->method = method;

    return resp;
}

static jerry_value_t create_resource(const char* path, jerry_value_t resource_init)
{
    jerry_value_t res = jerry_create_object();

    if (path) {
        zjs_obj_add_string(res, path, "resourcePath");
    }
    jerry_value_t properties = zjs_get_property(resource_init, "properties");

    zjs_set_property(res, "properties", properties);

    DBG_PRINT("path=%s, obj number=%lu\n", path, res);

    jerry_release_value(properties);

    return res;
}

#if 0
static void print_props_data(oc_request_t *data)
{
    int i;
    oc_rep_t *rep = data->request_payload;
    while (rep != NULL) {
        ZJS_PRINT("Type: %u, Key: %s, Value: ", rep->type, oc_string(rep->name));
        switch (rep->type) {
        case BOOL:
            ZJS_PRINT("%d\n", rep->value_boolean);
            break;
        case INT:
            ZJS_PRINT("%ld\n", (int32_t)rep->value_int);
            break;
        case BYTE_STRING:
        case STRING:
            ZJS_PRINT("%s\n", oc_string(rep->value_string));
            break;
        case STRING_ARRAY:
            ZJS_PRINT("[ ");
            for (i = 0; i < oc_string_array_get_allocated_size(rep->value_array); i++) {
                ZJS_PRINT("%s ", oc_string_array_get_item(rep->value_array, i));
            }
            ZJS_PRINT("]\n");
            break;
        case OBJECT:
            ZJS_PRINT("{ Object }\n");
            break;
        default:
            break;
        }
        rep = rep->next;
    }
}
#endif

static jerry_value_t ocf_respond(const jerry_value_t function_val,
                                 const jerry_value_t this,
                                 const jerry_value_t argv[],
                                 const jerry_length_t argc)
{
    // args: properties object
    ZJS_VALIDATE_ARGS(Z_OBJECT);

    jerry_value_t promise = jerry_create_object();
    struct ocf_handler* h;
    jerry_value_t request = this;

    jerry_value_t data = argv[0];

    if (!jerry_get_object_native_handle(request, (uintptr_t*)&h)) {
        ERR_PRINT("native handle not found\n");
        REJECT(promise, "TypeMismatchError", "native handle not found", h);
        oc_send_response(h->req, OC_STATUS_INTERNAL_SERVER_ERROR);
        return promise;
    }

    void* ret;
    // Start the root encoding object
    zjs_rep_start_root_object();
    // Encode all properties from resource (argv[0])
    ret = zjs_ocf_props_setup(data, &g_encoder, true);
    zjs_rep_end_root_object();
    // Free property return handle
    zjs_ocf_free_props(ret);

    /*
     * TODO: Better error handling here. We need to implement a respond
     *       with error as well as checking the return code of previous
     *       OCF calls made.
     */
    if (h->resp->method == OC_PUT || h->resp->method == OC_POST) {
        oc_send_response(h->req, OC_STATUS_CHANGED);
    } else {
        oc_send_response(h->req, OC_STATUS_OK);
    }
    DBG_PRINT("responding to method type=%u, properties=%lu\n", h->resp->method, data);

    zjs_make_promise(promise, NULL, NULL);

    zjs_fulfill_promise(promise, NULL, 0);

    return promise;
}

static jerry_value_t create_request(struct server_resource* resource, oc_method_t method, struct ocf_handler* handler)
{

    handler->resp = create_response(resource, method);
    jerry_value_t object = jerry_create_object();
    jerry_value_t target = jerry_create_object();
    jerry_value_t source = jerry_create_object();

    if (resource->res) {
        zjs_obj_add_string(source, oc_string(resource->res->uri), "resourcePath");
        zjs_obj_add_string(target, oc_string(resource->res->uri), "resourcePath");
    }

    // source is the resource requesting the operation
    zjs_set_property(object, "source", source);

    // target is the resource being retrieved
    zjs_set_property(object, "target", target);

    zjs_obj_add_function(object, ocf_respond, "respond");

    jerry_set_object_native_handle(object, (uintptr_t)handler, NULL);

    jerry_release_value(target);
    jerry_release_value(source);

    return object;
}

static void post_get(void* handler)
{
    // ZJS_PRINT("POST GET\n");
}

static void ocf_get_handler(oc_request_t *request, oc_interface_mask_t interface, void* user_data)
{
    ZJS_PRINT("ocf_get_handler()\n");
    struct ocf_handler* h = new_ocf_handler((struct server_resource*)user_data);
    if (!h) {
        ERR_PRINT("handler was NULL\n");
        return;
    }
    jerry_value_t argv[2];
    argv[0] = create_request(h->res, OC_GET, h);
    argv[1] = jerry_create_boolean(0);
    h->req = request;
    zjs_trigger_event_now(h->res->object, "retrieve", argv, 2, post_get, h);

    jerry_release_value(argv[0]);
    jerry_release_value(argv[1]);
    zjs_free(h->resp);
    zjs_free(h);
}

static void post_put(void* handler)
{
    // ZJS_PRINT("POST PUT\n");
}

static void ocf_put_handler(oc_request_t *request, oc_interface_mask_t interface, void* user_data)
{
    ZJS_PRINT("ocf_put_handler()\n");
    struct ocf_handler* h = new_ocf_handler((struct server_resource*)user_data);
    if (!h) {
        ERR_PRINT("handler was NULL\n");
        return;
    }
    jerry_value_t request_val = create_request(h->res, OC_PUT, h);
    jerry_value_t props_val = request_to_jerry_value(request);
    jerry_value_t resource_val = jerry_create_object();

    zjs_set_property(resource_val, "properties", props_val);
    zjs_set_property(request_val, "resource", resource_val);

    jerry_release_value(props_val);
    jerry_release_value(resource_val);

    h->req = request;

    zjs_trigger_event_now(h->res->object, "update", &request_val, 1, post_put, h);

    DBG_PRINT("sent PUT response, code=CHANGED\n");

    jerry_release_value(request_val);
    zjs_free(h->resp);
    zjs_free(h);
}

#ifdef ZJS_OCF_DELETE_SUPPORT
static void post_delete(void* handler)
{
    // ZJS_PRINT("POST DELETE\n");
}

static void ocf_delete_handler(oc_request_t *request, oc_interface_mask_t interface, void* user_data)
{
    struct ocf_handler* h = new_ocf_handler((struct server_resource*)user_data);
    if (!h) {
        ERR_PRINT("handler was NULL\n");
        return;
    }
    zjs_trigger_event_now(h->res->object, "delete", NULL, 0, post_delete, h);

    oc_send_response(request, OC_STATUS_DELETED);

    DBG_PRINT("sent DELETE response, code=OC_STATUS_DELETED\n");
}
#endif

/*
 * TODO: Get resource object and use it to notify
 */
static jerry_value_t ocf_notify(const jerry_value_t function_val,
                                const jerry_value_t this,
                                const jerry_value_t argv[],
                                const jerry_length_t argc)
{
    // args: resource object
    ZJS_VALIDATE_ARGS(Z_OBJECT);

    struct server_resource* resource;
    if (!jerry_get_object_native_handle(argv[0], (uintptr_t*)&resource)) {
        DBG_PRINT("native handle not found\n");
        return ZJS_UNDEFINED;
    }
    DBG_PRINT("path=%s\n", resource->resource_path);

    oc_notify_observers(resource->res);

    return ZJS_UNDEFINED;
}

/*
typedef struct resource_list {
    oc_resource_t *resource;
    struct resource_list* next;
} resource_list_t;

static resource_list_t *r_list = NULL;
static bool has_registered = false;
*/

static jerry_value_t ocf_register(const jerry_value_t function_val,
                                  const jerry_value_t this,
                                  const jerry_value_t argv[],
                                  const jerry_length_t argc)
{
    // args: resource object
    ZJS_VALIDATE_ARGS(Z_OBJECT);

    struct server_resource* resource;
    int i;
    jerry_value_t promise = jerry_create_object();
    struct ocf_handler* h;

    // Required
    jerry_value_t resource_path_val = zjs_get_property(argv[0], "resourcePath");
    if (!jerry_value_is_string(resource_path_val)) {
        ERR_PRINT("resourcePath not found\n");
        REJECT(promise, "TypeMismatchError", "resourcePath not found", h);
        return promise;
    }
    ZJS_GET_STRING(resource_path_val, resource_path, OCF_MAX_RES_PATH_LEN);
    jerry_release_value(resource_path_val);

    jerry_value_t res_type_array = zjs_get_property(argv[0], "resourceTypes");
    if (!jerry_value_is_array(res_type_array)) {
        ERR_PRINT("resourceTypes array not found\n");
        REJECT(promise, "TypeMismatchError", "resourceTypes array not found", h);
        return promise;
    }
    uint32_t num_types = jerry_get_array_length(res_type_array);

    // Optional
    uint32_t flags = 0;
    jerry_value_t observable_val = zjs_get_property(argv[0], "observable");
    if (jerry_value_is_boolean(observable_val)) {
        if (jerry_get_boolean_value(observable_val)) {
            flags |= FLAG_OBSERVE;
        }
    }
    jerry_release_value(observable_val);

    jerry_value_t discoverable_val = zjs_get_property(argv[0], "discoverable");
    if (jerry_value_is_boolean(discoverable_val)) {
        if (jerry_get_boolean_value(discoverable_val)) {
            flags |= FLAG_DISCOVERABLE;
        }
    }
    jerry_release_value(discoverable_val);

    jerry_value_t slow_val = zjs_get_property(argv[0], "slow");
    if (jerry_value_is_boolean(slow_val)) {
        if (jerry_get_boolean_value(slow_val)) {
            flags |= FLAG_SLOW;
        }
    }
    jerry_release_value(slow_val);

    jerry_value_t secure_val = zjs_get_property(argv[0], "secure");
    if (jerry_value_is_boolean(secure_val)) {
        if (jerry_get_boolean_value(secure_val)) {
            flags |= FLAG_SECURE;
        }
    }
    jerry_release_value(secure_val);

    resource = new_server_resource(resource_path);

    if (zjs_ocf_start() < 0) {
        REJECT(promise, "InternalError", "OCF failed to start", h);
        return promise;
    }

    resource->res = oc_new_resource(resource_path, num_types, 0);

    for (i = 0; i < num_types; ++i) {
        jerry_value_t type_val = jerry_get_property_by_index(res_type_array, i);
        ZJS_GET_STRING(type_val, type_name, OCF_MAX_RES_TYPE_LEN);
        oc_resource_bind_resource_type(resource->res, type_name);
        jerry_release_value(type_val);
    }
    oc_resource_bind_resource_interface(resource->res, OC_IF_RW);
    oc_resource_set_default_interface(resource->res, OC_IF_RW);

#ifdef OC_SECURITY
  oc_resource_make_secure(resource->res);
#endif

    if (flags & FLAG_DISCOVERABLE) {
        oc_resource_set_discoverable(resource->res, 1);
    }
    if (flags & FLAG_OBSERVE) {
        oc_resource_set_periodic_observable(resource->res, 1);
    }
    /*
     * TODO: Since requests are handled in JS can POST/PUT use the same handler?
     */
    oc_resource_set_request_handler(resource->res, OC_GET, ocf_get_handler, resource);
    oc_resource_set_request_handler(resource->res, OC_PUT, ocf_put_handler, resource);
    oc_resource_set_request_handler(resource->res, OC_POST, ocf_put_handler, resource);

    oc_add_resource(resource->res);

    /*resource_list_t *new = zjs_malloc(sizeof(resource_list_t));
    new->resource = resource->res;
    new->next = r_list;
    r_list = new;*/

    h = new_ocf_handler(resource);
    zjs_make_promise(promise, post_ocf_promise, h);
    /*
     * Find lifetime of resource, free in post promise
     */
    jerry_value_t res = create_resource(resource_path, argv[0]);
    zjs_fulfill_promise(promise, &res, 1);

    /*
     * TODO: Add native handle to ensure it gets freed,
     *       check if we can reference with 'this'
     */
    resource->object = this;

    jerry_set_object_native_handle(res, (uintptr_t)resource, NULL);

    DBG_PRINT("registered resource, path=%s\n", resource_path);

    jerry_release_value(res_type_array);

    return promise;
}

/*
 * TODO: iotivity-constrained requires that resources get registered in this
 *       function, which gets called long before we know about any resources.
 *       What we have "works" but its not how its supposed to be structured.
 */
void zjs_ocf_register_resources(void)
{
    // ZJS_PRINT("zjs_ocf_register_resources() callback\n");
    /*resource_list_t *cur = r_list;
    while (cur) {
        oc_add_resource(cur->resource);
        resource_list_t *next = cur->next;
        zjs_free(cur);
        cur = next;
    }*/
}

jerry_value_t zjs_ocf_server_init()
{
    jerry_value_t server = jerry_create_object();

    zjs_obj_add_function(server, ocf_register, "register");
    zjs_obj_add_function(server, ocf_notify, "notify");

    zjs_make_event(server, ZJS_UNDEFINED);

    return server;
}

#endif // BUILD_MODULE_OCF
