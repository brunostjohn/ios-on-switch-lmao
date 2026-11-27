// SPDX-License-Identifier: GPL-3.0-or-later
// C ABI of the portable C++17 GPU shader seed front (directpack + native-seed), callable from Rust FFI or CLI tools.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum switch_seed_status {
    SWITCH_SEED_OK = 0,
    SWITCH_SEED_ERR_INVALID_ARG = 1,
    SWITCH_SEED_ERR_IO = 2,
    SWITCH_SEED_ERR_COMPILE = 3,
    SWITCH_SEED_ERR_CANCELLED = 4,
} switch_seed_status_t;

typedef int (*switch_seed_cancel_fn)(void *ctx);

typedef struct switch_seed_directpack_opts {
    const char *kind;
    const char *qc_metallib;
    const char *corpus_path;
    const char *output_path;
    const char *own_lib;
    const char *lib_root;
    const char *data_libs;
    const char *label_prefix;
    uint64_t scope;
    switch_seed_cancel_fn cancel_check;
    void *cancel_ctx;
} switch_seed_directpack_opts_t;

typedef struct switch_seed_native_opts {
    const char *kind;
    const char *libs_store;
    const char *qc_metallib;
    uint64_t stack_key;
    const char *output_dir;
    const char *const *corpora;
    size_t corpus_count;
    const char *const *derived;
    size_t derived_count;
    uint64_t scope;
    switch_seed_cancel_fn cancel_check;
    void *cancel_ctx;
} switch_seed_native_opts_t;

switch_seed_status_t switch_seed_directpack(const switch_seed_directpack_opts_t *opts, char *err_buf, size_t err_cap);

switch_seed_status_t switch_seed_native_bucket(const switch_seed_native_opts_t *opts, char *err_buf, size_t err_cap);

switch_seed_status_t switch_seed_native_restamp(uint64_t stack_key, const char *output_dir,
                                                const char *const *input_dirs, size_t input_count, char *err_buf,
                                                size_t err_cap);

#ifdef __cplusplus
}
#endif
