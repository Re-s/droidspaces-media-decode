/* Minimal NV12 VA video-processing path used by scale_vaapi. */
#include <string.h>

#include "driver.h"

static void scale_plane_bilinear(const uint8_t *src, unsigned src_stride,
                                 unsigned src_x, unsigned src_y,
                                 unsigned src_w, unsigned src_h,
                                 uint8_t *dst, unsigned dst_stride,
                                 unsigned dst_x, unsigned dst_y,
                                 unsigned dst_w, unsigned dst_h,
                                 unsigned components, uint32_t rotation)
{
    for (unsigned y = 0; y < dst_h; y++) {
        for (unsigned x = 0; x < dst_w; x++) {
            double u = ((double)x + 0.5) / dst_w;
            double v = ((double)y + 0.5) / dst_h;
            double fx, fy;
            switch (rotation) {
            case VA_ROTATION_90:  /* clockwise */
                fx = v * src_w - 0.5;
                fy = (1.0 - u) * src_h - 0.5;
                break;
            case VA_ROTATION_180:
                fx = (1.0 - u) * src_w - 0.5;
                fy = (1.0 - v) * src_h - 0.5;
                break;
            case VA_ROTATION_270: /* counter-clockwise */
                fx = (1.0 - v) * src_w - 0.5;
                fy = u * src_h - 0.5;
                break;
            default:
                fx = u * src_w - 0.5;
                fy = v * src_h - 0.5;
                break;
            }
            if (fx < 0.0) fx = 0.0;
            if (fy < 0.0) fy = 0.0;
            unsigned x0 = (unsigned)fx;
            unsigned y0 = (unsigned)fy;
            if (x0 >= src_w) x0 = src_w - 1;
            if (y0 >= src_h) y0 = src_h - 1;
            unsigned x1 = x0 + 1 < src_w ? x0 + 1 : x0;
            unsigned y1 = y0 + 1 < src_h ? y0 + 1 : y0;
            double wx = fx - x0;
            double wy = fy - y0;
            for (unsigned c = 0; c < components; c++) {
                unsigned a = src[(src_y + y0) * src_stride +
                                 (src_x + x0) * components + c];
                unsigned b = src[(src_y + y0) * src_stride +
                                 (src_x + x1) * components + c];
                unsigned d = src[(src_y + y1) * src_stride +
                                 (src_x + x0) * components + c];
                unsigned e = src[(src_y + y1) * src_stride +
                                 (src_x + x1) * components + c];
                double top = a + (b - (double)a) * wx;
                double bottom = d + (e - (double)d) * wx;
                double value = top + (bottom - top) * wy;
                dst[(dst_y + y) * dst_stride +
                    (dst_x + x) * components + c] =
                    (uint8_t)(value + 0.5);
            }
        }
    }
}

VAStatus dmd_vpp_render(VADriverContextP ctx, VAContextID context,
                        VABufferID *buffers, int num_buffers)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv || (num_buffers && !buffers) || num_buffers < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c || c->entrypoint != VAEntrypointVideoProc ||
        c->current_target == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VAStatus status = VA_STATUS_SUCCESS;
    for (int i = 0; i < num_buffers; i++) {
        struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffers[i]);
        if (!b || b->context != context) {
            status = VA_STATUS_ERROR_INVALID_BUFFER;
            break;
        }
        if (b->type != VAProcPipelineParameterBufferType ||
            b->size < sizeof(VAProcPipelineParameterBuffer)) {
            status = VA_STATUS_ERROR_UNIMPLEMENTED;
            break;
        }

        const VAProcPipelineParameterBuffer *p = b->data;
        if (p->num_filters || p->num_forward_references ||
            p->num_backward_references ||
            p->mirror_state != VA_MIRROR_NONE || p->num_additional_outputs) {
            status = VA_STATUS_ERROR_UNIMPLEMENTED;
            break;
        }
        if (p->rotation_state != VA_ROTATION_NONE &&
            p->rotation_state != VA_ROTATION_90 &&
            p->rotation_state != VA_ROTATION_180 &&
            p->rotation_state != VA_ROTATION_270) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER;
            break;
        }
        c->vpp_source = p->surface;
        c->vpp_rotation = p->rotation_state;
        c->vpp_has_source_region = p->surface_region != NULL;
        if (p->surface_region)
            c->vpp_source_region = *p->surface_region;
        c->vpp_has_output_region = p->output_region != NULL;
        if (p->output_region)
            c->vpp_output_region = *p->output_region;
        c->vpp_have_params = 1;
    }
    pthread_mutex_unlock(&drv->lock);
    return status;
}

VAStatus dmd_vpp_end(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv) return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c || c->entrypoint != VAEntrypointVideoProc ||
        c->current_target == VA_INVALID_ID || !c->vpp_have_params) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VASurfaceID source_id = c->vpp_source;
    uint32_t rotation = c->vpp_rotation;
    pthread_mutex_unlock(&drv->lock);

    VAStatus ready = dmd_surface_wait(drv, source_id);
    if (ready != VA_STATUS_SUCCESS) return ready;

    pthread_mutex_lock(&drv->lock);
    c = dmd_find_context_locked(drv, context);
    struct dmd_surface *src = dmd_find_surface_locked(drv, source_id);
    struct dmd_surface *dst = c ? dmd_find_surface_locked(drv,
                                                            c->current_target)
                                : NULL;
    if (!c || !src || !dst || src->format != VA_RT_FORMAT_YUV420 ||
        dst->format != VA_RT_FORMAT_YUV420 || !src->data || !dst->data) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    VARectangle sr = { 0, 0, (uint16_t)src->width, (uint16_t)src->height };
    VARectangle dr = { 0, 0, (uint16_t)dst->width, (uint16_t)dst->height };
    if (c->vpp_has_source_region) sr = c->vpp_source_region;
    if (c->vpp_has_output_region) dr = c->vpp_output_region;
    if (sr.x < 0 || sr.y < 0 || dr.x < 0 || dr.y < 0 ||
        !sr.width || !sr.height || !dr.width || !dr.height ||
        (unsigned)sr.x + sr.width > src->width ||
        (unsigned)sr.y + sr.height > src->height ||
        (unsigned)dr.x + dr.width > dst->width ||
        (unsigned)dr.y + dr.height > dst->height ||
        (sr.x & 1) || (sr.y & 1) || (sr.width & 1) || (sr.height & 1) ||
        (dr.x & 1) || (dr.y & 1) || (dr.width & 1) || (dr.height & 1)) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    size_t src_y_size = (size_t)src->stride * src->slice_height;
    size_t dst_y_size = (size_t)dst->stride * dst->slice_height;
    if (src_y_size >= src->data_size || dst_y_size >= dst->data_size) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    memset(dst->data, 16, dst_y_size);
    memset(dst->data + dst_y_size, 128, dst->data_size - dst_y_size);
    scale_plane_bilinear(src->data, src->stride,
                         (unsigned)sr.x, (unsigned)sr.y,
                         sr.width, sr.height,
                         dst->data, dst->stride,
                         (unsigned)dr.x, (unsigned)dr.y,
                         dr.width, dr.height, 1, rotation);
    scale_plane_bilinear(src->data + src_y_size, src->stride,
                         (unsigned)sr.x / 2, (unsigned)sr.y / 2,
                         sr.width / 2, sr.height / 2,
                         dst->data + dst_y_size, dst->stride,
                         (unsigned)dr.x / 2, (unsigned)dr.y / 2,
                         dr.width / 2, dr.height / 2, 2, rotation);

    dst->state = DMD_SURFACE_READY;
    dst->decode_status = VA_STATUS_SUCCESS;
    c->current_target = VA_INVALID_ID;
    c->vpp_have_params = 0;
    pthread_mutex_unlock(&drv->lock);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_vpp_query_filters(VADriverContextP ctx, VAContextID context,
                               VAProcFilterType *filters,
                               unsigned int *num_filters)
{
    if (!dmd_get_driver(ctx) || !num_filters)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    (void)filters;
    *num_filters = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_vpp_query_filter_caps(VADriverContextP ctx, VAContextID context,
                                   VAProcFilterType type, void *filter_caps,
                                   unsigned int *num_filter_caps)
{
    (void)context; (void)type; (void)filter_caps;
    if (!dmd_get_driver(ctx) || !num_filter_caps)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    *num_filter_caps = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

VAStatus dmd_vpp_query_pipeline_caps(VADriverContextP ctx,
                                     VAContextID context,
                                     VABufferID *filters,
                                     unsigned int num_filters,
                                     VAProcPipelineCaps *caps)
{
    static VAProcColorStandardType standards[] = {
        VAProcColorStandardNone, VAProcColorStandardBT601,
        VAProcColorStandardBT709, VAProcColorStandardBT470M,
        VAProcColorStandardBT470BG, VAProcColorStandardSMPTE170M,
        VAProcColorStandardSMPTE240M, VAProcColorStandardGenericFilm,
        VAProcColorStandardSRGB,
    };
    static uint32_t formats[] = { VA_FOURCC_NV12 };
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv || !caps || (num_filters && !filters))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_filters)
        return VA_STATUS_ERROR_UNSUPPORTED_FILTER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    int valid = c && c->entrypoint == VAEntrypointVideoProc;
    pthread_mutex_unlock(&drv->lock);
    if (!valid) return VA_STATUS_ERROR_INVALID_CONTEXT;

    memset(caps, 0, sizeof(*caps));
    caps->filter_flags = VA_FILTER_INTERPOLATION_BILINEAR;
    caps->rotation_flags = (1u << VA_ROTATION_NONE) |
                           (1u << VA_ROTATION_90) |
                           (1u << VA_ROTATION_180) |
                           (1u << VA_ROTATION_270);
    caps->input_color_standards = standards;
    caps->num_input_color_standards = sizeof(standards) / sizeof(standards[0]);
    caps->output_color_standards = standards;
    caps->num_output_color_standards = sizeof(standards) / sizeof(standards[0]);
    caps->input_pixel_format = formats;
    caps->num_input_pixel_formats = 1;
    caps->output_pixel_format = formats;
    caps->num_output_pixel_formats = 1;
    caps->min_input_width = caps->min_input_height = DMD_MIN_WIDTH;
    caps->max_input_width = DMD_MAX_WIDTH;
    caps->max_input_height = DMD_MAX_HEIGHT;
    caps->min_output_width = caps->min_output_height = DMD_MIN_WIDTH;
    caps->max_output_width = DMD_MAX_WIDTH;
    caps->max_output_height = DMD_MAX_HEIGHT;
    return VA_STATUS_SUCCESS;
}
