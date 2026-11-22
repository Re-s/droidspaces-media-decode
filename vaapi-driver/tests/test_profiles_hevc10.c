/* Host-only capability contracts: default off, HEVC10 only, and HEVC10+AV1.
 * Device probes are fixed capabilities, not successful hardware frame claims. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver.h"
#include "v4l2_encoder.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)
#define OK(value) CHECK((value) == VA_STATUS_SUCCESS)

struct dmd_driver *dmd_get_driver(VADriverContextP ctx)
{
    return ctx ? ctx->pDriverData : NULL;
}
void dmd_log(const char *format, ...) { (void)format; }
int dmd_v4l2_probe(int codec) { (void)codec; return 1; }
int dmd_v4l2_enc_get_limits(enum dmd_enc_profile profile, unsigned *a,
                           unsigned *b, unsigned *c, unsigned *d)
{
    (void)profile; (void)a; (void)b; (void)c; (void)d;
    return -1;
}

static void check_formats(VADriverContextP va, VAProfile profile, VAEntrypoint entry,
                           uint32_t rt, unsigned pixel_count)
{
    VAConfigAttrib wanted = { .type = VAConfigAttribRTFormat };
    OK(dmd_GetConfigAttributes(va, profile, entry, &wanted, 1));
    CHECK(wanted.value == rt);
    VAConfigID config = VA_INVALID_ID;
    /* The query must remain correct even when the client omits RTFormat. */
    OK(dmd_CreateConfig(va, profile, entry, NULL, 0, &config));
    VAConfigAttrib attrs[DMD_MAX_CONFIG_ATTRIBUTES];
    VAProfile actual_profile;
    VAEntrypoint actual_entry;
    int count = 0, rt_count = 0;
    OK(dmd_QueryConfigAttributes(va, config, &actual_profile, &actual_entry, attrs, &count));
    CHECK(actual_profile == profile && actual_entry == entry);
    for (int i = 0; i < count; i++)
        if (attrs[i].type == VAConfigAttribRTFormat) {
            CHECK(attrs[i].value == rt);
            rt_count++;
        }
    CHECK(rt_count == 1);
    unsigned needed = 0;
    OK(dmd_QuerySurfaceAttributes(va, config, NULL, &needed));
    CHECK(needed == 4 + pixel_count + (entry == VAEntrypointEncSlice));
    VASurfaceAttrib surfaces[8];
    unsigned capacity = needed - 1;
    CHECK(dmd_QuerySurfaceAttributes(va, config, surfaces, &capacity) == VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
    CHECK(capacity == needed);
    capacity = 8;
    OK(dmd_QuerySurfaceAttributes(va, config, surfaces, &capacity));
    unsigned pixels = 0, nv12 = 0, p010 = 0;
    for (unsigned i = 0; i < capacity; i++) {
        if (surfaces[i].type != VASurfaceAttribPixelFormat) continue;
        CHECK(surfaces[i].value.type == VAGenericValueTypeInteger);
        CHECK(surfaces[i].flags == (VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE));
        pixels++;
        nv12 += surfaces[i].value.value.i == VA_FOURCC_NV12;
        p010 += surfaces[i].value.value.i == VA_FOURCC_P010;
    }
    CHECK(pixels == pixel_count);
    CHECK(nv12 == !!(rt & VA_RT_FORMAT_YUV420));
    CHECK(p010 == !!(rt & VA_RT_FORMAT_YUV420_10));
    OK(dmd_DestroyConfig(va, config));
}

int main(void)
{
    struct dmd_driver *driver = calloc(1, sizeof(*driver));
    CHECK(driver);
    CHECK(pthread_mutex_init(&driver->lock, NULL) == 0);
    driver->next_config_id = 1;
    driver->encode_caps = DMD_ENC_CAP(DMD_ENC_HEVC_MAIN);
    struct VADriverContext va = { .pDriverData = driver };
    VAProfile profiles[DMD_MAX_PROFILES];
    int count = 0, main10 = 0, vp9p2 = 0, av1 = 0;
    OK(dmd_QueryConfigProfiles(&va, profiles, &count));
    for (int i = 0; i < count; i++) {
        main10 += profiles[i] == VAProfileHEVCMain10;
        vp9p2 += profiles[i] == VAProfileVP9Profile2;
        av1 += profiles[i] == VAProfileAV1Profile0;
    }
    CHECK(vp9p2 == 0); /* HEVC10 must not turn on unrelated old probe profiles. */
    VAConfigID id = VA_INVALID_ID;
#ifdef DMD_ENABLE_HEVC_MAIN10
    CHECK(main10 == 1 && dmd_profile_supported(VAProfileHEVCMain10));
    CHECK(dmd_profile_to_codec(VAProfileHEVCMain10) == DMD_CODEC_HEVC);
    VAEntrypoint entries[DMD_MAX_ENTRYPOINTS];
    int n = 0;
    OK(dmd_QueryConfigEntrypoints(&va, VAProfileHEVCMain10, entries, &n));
    CHECK(n == 1 && entries[0] == VAEntrypointVLD);
    const uint32_t main10_rt = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    check_formats(&va, VAProfileHEVCMain10, VAEntrypointVLD, main10_rt, 2);
    const uint32_t valid_rt[] = { VA_RT_FORMAT_YUV420, VA_RT_FORMAT_YUV420_10, main10_rt };
    for (unsigned i = 0; i < sizeof(valid_rt) / sizeof(valid_rt[0]); i++) {
        VAConfigAttrib requested = { VAConfigAttribRTFormat, valid_rt[i] };
        OK(dmd_CreateConfig(&va, VAProfileHEVCMain10, VAEntrypointVLD, &requested, 1, &id));
        VAConfigAttrib attrs[DMD_MAX_CONFIG_ATTRIBUTES];
        VAProfile actual_profile;
        VAEntrypoint actual_entry;
        int actual_count = 0;
        OK(dmd_QueryConfigAttributes(&va, id, &actual_profile, &actual_entry, attrs, &actual_count));
        CHECK(actual_count == 1 && attrs[0].type == VAConfigAttribRTFormat &&
              attrs[0].value == main10_rt); /* Chrome discovers P010 after its RT8 config. */
        OK(dmd_DestroyConfig(&va, id));
    }
    const uint32_t invalid_rt[] = { 0, VA_RT_FORMAT_YUV422, VA_RT_FORMAT_RGB32,
                                   main10_rt | VA_RT_FORMAT_YUV422 };
    for (unsigned i = 0; i < sizeof(invalid_rt) / sizeof(invalid_rt[0]); i++) {
        VAConfigAttrib requested = { VAConfigAttribRTFormat, invalid_rt[i] };
        CHECK(dmd_CreateConfig(&va, VAProfileHEVCMain10, VAEntrypointVLD,
                                &requested, 1, &id) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    }
#else
    CHECK(main10 == 0 && !dmd_profile_supported(VAProfileHEVCMain10));
    CHECK(dmd_profile_to_codec(VAProfileHEVCMain10) < 0);
    CHECK(dmd_CreateConfig(&va, VAProfileHEVCMain10, VAEntrypointVLD, NULL, 0, &id) == VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
#endif
    CHECK(dmd_CreateConfig(&va, VAProfileHEVCMain10, VAEntrypointEncSlice, NULL, 0, &id) != VA_STATUS_SUCCESS);
    check_formats(&va, VAProfileHEVCMain, VAEntrypointVLD, VA_RT_FORMAT_YUV420, 1);
    check_formats(&va, VAProfileHEVCMain, VAEntrypointEncSlice, VA_RT_FORMAT_YUV420, 1);
#ifdef DMD_ENABLE_AV1
    CHECK(av1 == 1);
    check_formats(&va, VAProfileAV1Profile0, VAEntrypointVLD,
                  VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10, 2);
#else
    CHECK(av1 == 0); /* HEVC10 is independent of AV1 too. */
#endif
    pthread_mutex_destroy(&driver->lock);
    free(driver);
    puts("profile capability contracts: PASS");
    return 0;
}
