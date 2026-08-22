/* 本文件由 tools/gen_stubs.py 自动生成，请勿手工修改。
 *
 * libva 在 vaInitialize 中逐个校验 vtable 槽位非 NULL（va/va.c CHECK_VTABLE），
 * 任一为 NULL 会导致整个初始化失败。未实现的入口因此必须存在并返回
 * VA_STATUS_ERROR_UNIMPLEMENTED，而不能留空指针。
 */

#include "driver.h"

VAStatus dmd_QuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info)
{
    (void)ctx;
    (void)render_target;
    (void)error_status;
    (void)error_info;
    dmd_log("未实现入口被调用: vaQuerySurfaceError\n");
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
    dmd_log("未实现入口被调用: vaPutSurface\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette)
{
    (void)ctx;
    (void)image;
    (void)palette;
    dmd_log("未实现入口被调用: vaSetImagePalette\n");
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
    dmd_log("未实现入口被调用: vaPutImage\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateSubpicture(VADriverContextP ctx, VAImageID image, VASubpictureID *subpicture)
{
    (void)ctx;
    (void)image;
    (void)subpicture;
    dmd_log("未实现入口被调用: vaCreateSubpicture\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture)
{
    (void)ctx;
    (void)subpicture;
    dmd_log("未实现入口被调用: vaDestroySubpicture\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image)
{
    (void)ctx;
    (void)subpicture;
    (void)image;
    dmd_log("未实现入口被调用: vaSetSubpictureImage\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture, unsigned int chromakey_min, unsigned int chromakey_max, unsigned int chromakey_mask)
{
    (void)ctx;
    (void)subpicture;
    (void)chromakey_min;
    (void)chromakey_max;
    (void)chromakey_mask;
    dmd_log("未实现入口被调用: vaSetSubpictureChromakey\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha)
{
    (void)ctx;
    (void)subpicture;
    (void)global_alpha;
    dmd_log("未实现入口被调用: vaSetSubpictureGlobalAlpha\n");
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
    dmd_log("未实现入口被调用: vaAssociateSubpicture\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_DeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces)
{
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    dmd_log("未实现入口被调用: vaDeassociateSubpicture\n");
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
    dmd_log("未实现入口被调用: vaLockSurface\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_UnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{
    (void)ctx;
    (void)surface;
    dmd_log("未实现入口被调用: vaUnlockSurface\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_GetSurfaceAttributes(VADriverContextP dpy, VAConfigID config, VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
    (void)dpy;
    (void)config;
    (void)attrib_list;
    (void)num_attribs;
    dmd_log("未实现入口被调用: vaGetSurfaceAttributes\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_AcquireBufferHandle(VADriverContextP ctx, VABufferID buf_id, VABufferInfo *buf_info)
{
    (void)ctx;
    (void)buf_id;
    (void)buf_info;
    dmd_log("未实现入口被调用: vaAcquireBufferHandle\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{
    (void)ctx;
    (void)buf_id;
    dmd_log("未实现入口被调用: vaReleaseBufferHandle\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_CreateMFContext(VADriverContextP ctx, VAMFContextID *mfe_context)
{
    (void)ctx;
    (void)mfe_context;
    dmd_log("未实现入口被调用: vaCreateMFContext\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MFAddContext(VADriverContextP ctx, VAMFContextID mf_context, VAContextID context)
{
    (void)ctx;
    (void)mf_context;
    (void)context;
    dmd_log("未实现入口被调用: vaMFAddContext\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MFReleaseContext(VADriverContextP ctx, VAMFContextID mf_context, VAContextID context)
{
    (void)ctx;
    (void)mf_context;
    (void)context;
    dmd_log("未实现入口被调用: vaMFReleaseContext\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context, VAContextID *contexts, int num_contexts)
{
    (void)ctx;
    (void)mf_context;
    (void)contexts;
    (void)num_contexts;
    dmd_log("未实现入口被调用: vaMFSubmit\n");
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
    dmd_log("未实现入口被调用: vaCreateBuffer2\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_QueryProcessingRate(VADriverContextP ctx, VAConfigID config_id, VAProcessingRateParameter *proc_buf, unsigned int *processing_rate)
{
    (void)ctx;
    (void)config_id;
    (void)proc_buf;
    (void)processing_rate;
    dmd_log("未实现入口被调用: vaQueryProcessingRate\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void *descriptor)
{
    (void)ctx;
    (void)surface_id;
    (void)mem_type;
    (void)flags;
    (void)descriptor;
    dmd_log("未实现入口被调用: vaExportSurfaceHandle\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_SyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns)
{
    (void)ctx;
    (void)buf_id;
    (void)timeout_ns;
    dmd_log("未实现入口被调用: vaSyncBuffer\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus dmd_Copy(VADriverContextP ctx, VACopyObject *dst, VACopyObject *src, VACopyOption option)
{
    (void)ctx;
    (void)dst;
    (void)src;
    (void)option;
    dmd_log("未实现入口被调用: vaCopy\n");
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
