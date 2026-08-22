/* 本文件由 tools/gen_stubs.py 自动生成，请勿手工修改。
 *
 * libva 在 vaInitialize 中逐个校验 vtable 槽位非 NULL（va/va.c CHECK_VTABLE），
 * 任一为 NULL 会导致整个初始化失败。未实现的入口因此必须存在并返回
 * VA_STATUS_ERROR_UNIMPLEMENTED，而不能留空指针。
 */

#include "driver.h"

VAStatus dmd_CreateSurfaces(VADriverContextP ctx, int width, int height, int format, int num_surfaces, VASurfaceID *surfaces)
{
    (void)ctx;
    (void)width;
    (void)height;
    (void)format;
    (void)num_surfaces;
    (void)surfaces;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces)
{
    (void)ctx;
    (void)surface_list;
    (void)num_surfaces;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID *render_targets, int num_render_targets, VAContextID *context)
{
    (void)ctx;
    (void)config_id;
    (void)picture_width;
    (void)picture_height;
    (void)flag;
    (void)render_targets;
    (void)num_render_targets;
    (void)context;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DestroyContext(VADriverContextP ctx, VAContextID context)
{
    (void)ctx;
    (void)context;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void *data, VABufferID *buf_id)
{
    (void)ctx;
    (void)context;
    (void)type;
    (void)size;
    (void)num_elements;
    (void)data;
    (void)buf_id;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_BufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements)
{
    (void)ctx;
    (void)buf_id;
    (void)num_elements;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
    (void)ctx;
    (void)buf_id;
    (void)pbuf;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
    (void)ctx;
    (void)buf_id;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
    (void)ctx;
    (void)buffer_id;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_BeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target)
{
    (void)ctx;
    (void)context;
    (void)render_target;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_RenderPicture(VADriverContextP ctx, VAContextID context, VABufferID *buffers, int num_buffers)
{
    (void)ctx;
    (void)context;
    (void)buffers;
    (void)num_buffers;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_EndPicture(VADriverContextP ctx, VAContextID context)
{
    (void)ctx;
    (void)context;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
    (void)ctx;
    (void)render_target;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_QuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status)
{
    (void)ctx;
    (void)render_target;
    (void)status;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_QuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info)
{
    (void)ctx;
    (void)render_target;
    (void)error_status;
    (void)error_info;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_PutSurface(VADriverContextP ctx, VASurfaceID surface, void*draw, short srcx, short srcy, unsigned short srcw, unsigned short srch, short destx, short desty, unsigned short destw, unsigned short desth, VARectangle *cliprects, unsigned int number_cliprects, unsigned int flags)
{
    (void)ctx;
    (void)surface;
    (void)draw;
    (void)srcx;
    (void)srcy;
    (void)srcw;
    (void)srch;
    (void)destx;
    (void)desty;
    (void)destw;
    (void)desth;
    (void)cliprects;
    (void)number_cliprects;
    (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateImage(VADriverContextP ctx, VAImageFormat *format, int width, int height, VAImage *image)
{
    (void)ctx;
    (void)format;
    (void)width;
    (void)height;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
    (void)ctx;
    (void)surface;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DestroyImage(VADriverContextP ctx, VAImageID image)
{
    (void)ctx;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette)
{
    (void)ctx;
    (void)image;
    (void)palette;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_GetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y, unsigned int width, unsigned int height, VAImageID image)
{
    (void)ctx;
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_PutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height)
{
    (void)ctx;
    (void)surface;
    (void)image;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dest_x;
    (void)dest_y;
    (void)dest_width;
    (void)dest_height;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateSubpicture(VADriverContextP ctx, VAImageID image, VASubpictureID *subpicture)
{
    (void)ctx;
    (void)image;
    (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture)
{
    (void)ctx;
    (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image)
{
    (void)ctx;
    (void)subpicture;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture, unsigned int chromakey_min, unsigned int chromakey_max, unsigned int chromakey_mask)
{
    (void)ctx;
    (void)subpicture;
    (void)chromakey_min;
    (void)chromakey_max;
    (void)chromakey_mask;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha)
{
    (void)ctx;
    (void)subpicture;
    (void)global_alpha;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_AssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces, short src_x, short src_y, unsigned short src_width, unsigned short src_height, short dest_x, short dest_y, unsigned short dest_width, unsigned short dest_height, unsigned int flags)
{
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dest_x;
    (void)dest_y;
    (void)dest_width;
    (void)dest_height;
    (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces)
{
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_BufferInfo(VADriverContextP ctx, VABufferID buf_id, VABufferType *type, unsigned int *size, unsigned int *num_elements)
{
    (void)ctx;
    (void)buf_id;
    (void)type;
    (void)size;
    (void)num_elements;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_LockSurface(VADriverContextP ctx, VASurfaceID surface, unsigned int *fourcc, unsigned int *luma_stride, unsigned int *chroma_u_stride, unsigned int *chroma_v_stride, unsigned int *luma_offset, unsigned int *chroma_u_offset, unsigned int *chroma_v_offset, unsigned int *buffer_name, void **buffer)
{
    (void)ctx;
    (void)surface;
    (void)fourcc;
    (void)luma_stride;
    (void)chroma_u_stride;
    (void)chroma_v_stride;
    (void)luma_offset;
    (void)chroma_u_offset;
    (void)chroma_v_offset;
    (void)buffer_name;
    (void)buffer;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_UnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{
    (void)ctx;
    (void)surface;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_GetSurfaceAttributes(VADriverContextP dpy, VAConfigID config, VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
    (void)dpy;
    (void)config;
    (void)attrib_list;
    (void)num_attribs;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height, VASurfaceID *surfaces, unsigned int num_surfaces, VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
    (void)ctx;
    (void)format;
    (void)width;
    (void)height;
    (void)surfaces;
    (void)num_surfaces;
    (void)attrib_list;
    (void)num_attribs;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_AcquireBufferHandle(VADriverContextP ctx, VABufferID buf_id, VABufferInfo *buf_info)
{
    (void)ctx;
    (void)buf_id;
    (void)buf_info;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{
    (void)ctx;
    (void)buf_id;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateMFContext(VADriverContextP ctx, VAMFContextID *mfe_context)
{
    (void)ctx;
    (void)mfe_context;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MFAddContext(VADriverContextP ctx, VAMFContextID mf_context, VAContextID context)
{
    (void)ctx;
    (void)mf_context;
    (void)context;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MFReleaseContext(VADriverContextP ctx, VAMFContextID mf_context, VAContextID context)
{
    (void)ctx;
    (void)mf_context;
    (void)context;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context, VAContextID *contexts, int num_contexts)
{
    (void)ctx;
    (void)mf_context;
    (void)contexts;
    (void)num_contexts;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateBuffer2(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int width, unsigned int height, unsigned int *unit_size, unsigned int *pitch, VABufferID *buf_id)
{
    (void)ctx;
    (void)context;
    (void)type;
    (void)width;
    (void)height;
    (void)unit_size;
    (void)pitch;
    (void)buf_id;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_QueryProcessingRate(VADriverContextP ctx, VAConfigID config_id, VAProcessingRateParameter *proc_buf, unsigned int *processing_rate)
{
    (void)ctx;
    (void)config_id;
    (void)proc_buf;
    (void)processing_rate;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void *descriptor)
{
    (void)ctx;
    (void)surface_id;
    (void)mem_type;
    (void)flags;
    (void)descriptor;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns)
{
    (void)ctx;
    (void)surface;
    (void)timeout_ns;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns)
{
    (void)ctx;
    (void)buf_id;
    (void)timeout_ns;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_Copy(VADriverContextP ctx, VACopyObject *dst, VACopyObject *src, VACopyOption option)
{
    (void)ctx;
    (void)dst;
    (void)src;
    (void)option;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf, uint32_t flags)
{
    (void)ctx;
    (void)buf_id;
    (void)pbuf;
    (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
