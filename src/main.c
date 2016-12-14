// Copyright (c) 2016, Intel Corporation.
#ifndef ZJS_LINUX_BUILD
// Zephyr includes
#include <zephyr.h>
#include "zjs_zephyr_port.h"
#else
#include "zjs_linux_port.h"
#endif // ZJS_LINUX_BUILD
#include <string.h>
#include "zjs_script.h"

// JerryScript includes
#include "jerry-api.h"

// Platform agnostic modules/headers
#include "zjs_callbacks.h"
#include "zjs_modules.h"
#ifdef BUILD_MODULE_SENSOR
#include "zjs_sensor.h"
#endif
#include "zjs_timers.h"
#ifdef BUILD_MODULE_OCF
#include "zjs_ocf_common.h"
#endif
#ifdef BUILD_MODULE_BLE
#include "zjs_ble.h"
#endif
#ifdef ZJS_LINUX_BUILD
#include "zjs_unit_tests.h"
#endif

#define ZJS_MAX_PRINT_SIZE      512

#ifdef ZJS_SNAPSHOT_BUILD
extern const uint8_t snapshot_bytecode[];
extern const int snapshot_len;
#else
extern const char *script_gen;
#endif

// native eval handler
static jerry_value_t native_eval_handler(const jerry_value_t function_obj,
                                         const jerry_value_t this,
                                         const jerry_value_t argv[],
                                         const jerry_length_t argc)
{
    return zjs_error("native_eval_handler: eval not supported");
}

// native print handler
static jerry_value_t native_print_handler(const jerry_value_t function_obj,
                                          const jerry_value_t this,
                                          const jerry_value_t argv[],
                                          const jerry_length_t argc)
{
    jerry_size_t jlen = jerry_get_string_size(argv[0]);
    if (jlen > ZJS_MAX_PRINT_SIZE) {
        ERR_PRINT("maximum print string length exceeded\n");
        return ZJS_UNDEFINED;
    }
    char buffer[jlen + 1];
    int wlen = jerry_string_to_char_buffer(argv[0], (jerry_char_t *)buffer, jlen);
    buffer[wlen] = '\0';

    ZJS_PRINT("%s\n", buffer);
    return ZJS_UNDEFINED;
}

#ifndef ZJS_LINUX_BUILD
void main(void)
#else
int main(int argc, char *argv[])
#endif
{
#ifndef ZJS_SNAPSHOT_BUILD
    const char *script = NULL;
    jerry_value_t code_eval;
    uint32_t len;
#endif
    jerry_value_t result;

    // print newline here to make it easier to find
    // the beginning of the program
    ZJS_PRINT("\n");

#ifdef ZJS_POOL_CONFIG
    zjs_init_mem_pools();
#ifdef DUMP_MEM_STATS
    zjs_print_pools();
#endif
#endif

    jerry_init(JERRY_INIT_EMPTY);

    zjs_init_callbacks();

    // Add module.exports to global namespace
    jerry_value_t global_obj = jerry_get_global_object();
    jerry_value_t modules_obj = jerry_create_object();
    jerry_value_t exports_obj = jerry_create_object();

    zjs_set_property(modules_obj, "exports", exports_obj);
    zjs_set_property(global_obj, "module", modules_obj);

    // initialize modules
    zjs_modules_init();

#ifdef BUILD_MODULE_OCF
    zjs_register_service_routine(NULL, main_poll_routine);
#endif

#ifndef ZJS_SNAPSHOT_BUILD
#ifdef ZJS_LINUX_BUILD
    if (argc > 1) {
        if (!strncmp(argv[1], "--unittest", 10)) {
            // run unit tests
            zjs_run_unit_tests();
        }
        else {
            if (zjs_read_script(argv[1], &script, &len)) {
                ERR_PRINT("could not read script file %s\n", argv[1]);
                return -1;
            }
        }
    } else
    // slightly tricky: reuse next section as else clause
#endif
    {
        script = script_gen;
        len = strnlen(script_gen, MAX_SCRIPT_SIZE);
        if (len == MAX_SCRIPT_SIZE) {
            ZJS_PRINT("Error: Script size too large! Increase MAX_SCRIPT_SIZE.\n");
            goto error;
        }
    }
#endif

    // Todo: find a better solution to disable eval() in JerryScript.
    // For now, just inject our eval() function in the global space
    zjs_obj_add_function(global_obj, native_eval_handler, "eval");
    zjs_obj_add_function(global_obj, native_print_handler, "print");

#ifndef ZJS_SNAPSHOT_BUILD
    code_eval = jerry_parse((jerry_char_t *)script, len, false);
    if (jerry_value_has_error_flag(code_eval)) {
        ZJS_PRINT("JerryScript: cannot parse javascript\n");
        goto error;
    }
#endif

#ifdef ZJS_LINUX_BUILD
    if (argc > 1) {
        zjs_free_script(script);
    }
#endif

#ifdef ZJS_SNAPSHOT_BUILD
    result = jerry_exec_snapshot(snapshot_bytecode,
                                 snapshot_len,
                                 false);
#else
    result = jerry_run(code_eval);
#endif

    if (jerry_value_has_error_flag(result)) {
        ZJS_PRINT("JerryScript: cannot run javascript\n");
        goto error;
    }

#ifndef ZJS_SNAPSHOT_BUILD
    jerry_release_value(code_eval);
#endif
    jerry_release_value(global_obj);
    jerry_release_value(modules_obj);
    jerry_release_value(exports_obj);
    jerry_release_value(result);

#ifndef ZJS_LINUX_BUILD
#ifndef QEMU_BUILD
#ifdef BUILD_MODULE_BLE
    zjs_ble_enable();
#endif
#endif
#endif // ZJS_LINUX_BUILD

    while (1) {
        zjs_timers_process_events();
        zjs_service_callbacks();
        zjs_service_routines();
        // not sure if this is okay, but it seems better to sleep than
        //   busy wait
        zjs_sleep(1);
    }

error:
#ifdef ZJS_LINUX_BUILD
    return 1;
#else
    return;
#endif
}
