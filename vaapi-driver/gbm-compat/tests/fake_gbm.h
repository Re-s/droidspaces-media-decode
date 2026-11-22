/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DMD_TEST_FAKE_GBM_H
#define DMD_TEST_FAKE_GBM_H

#include <stdint.h>

struct gbm_bo;

/* Test controls are exported only by the fake real-library implementation.
 * Statistics are local to the calling thread; set controls before workers run. */
struct fake_gbm_stats {
    unsigned creates;
    unsigned creates_with_modifiers;
    unsigned creates_with_modifiers2;
    unsigned imports;
    unsigned native_nv12_imports;
    unsigned unsupported_nv12_maps;
    unsigned destroys;
    unsigned maps;
    unsigned unmaps;
    unsigned writes;
    unsigned format_queries;
    unsigned modifier_plane_queries;
    unsigned surfaces_created;
    unsigned surfaces_destroyed;
    unsigned surface_locks;
    unsigned surface_releases;
    unsigned get_fds;
    unsigned get_plane_fds;
    unsigned user_callbacks;
    uint32_t create_width;
    uint32_t create_height;
    uint32_t create_format;
    uint32_t create_flags;
    uint64_t create_modifier;
    unsigned create_modifier_count;
    uint32_t query_format;
    uint32_t query_flags;
    uint32_t import_type;
    uint32_t import_flags;
    uint32_t import_width;
    uint32_t import_height;
    uint32_t import_format;
    uint32_t import_num_fds;
    uint32_t import_stride;
    uint32_t import_offset;
    uint64_t import_modifier;
    uint32_t map_x;
    uint32_t map_y;
    uint32_t map_width;
    uint32_t map_height;
    uint32_t map_flags;
    void *map_token;
    void *unmap_token;
    struct gbm_bo *last_bo;
};

void fake_gbm_reset(void);
void fake_gbm_set_native_nv12(int enabled);
void fake_gbm_set_native_nv12_import_only(int enabled);
void fake_gbm_set_fail_rgb_create(int enabled);
void fake_gbm_set_fail_map(int enabled);
const struct fake_gbm_stats *fake_gbm_get_stats(void);

#endif
