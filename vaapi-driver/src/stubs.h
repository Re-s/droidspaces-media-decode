/* 本文件由 tools/gen_stubs.py 自动生成，请勿手工修改。 */

#ifndef DMD_STUBS_H
#define DMD_STUBS_H

#include <va/va_backend.h>

VAStatus dmd_QuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info);
VAStatus dmd_PutSurface(VADriverContextP ctx, VASurfaceID surface, void*draw, short srcx, short srcy, unsigned short srcw, unsigned short srch, short destx, short desty, unsigned short destw, unsigned short desth, VARectangle *cliprects, unsigned int number_cliprects, unsigned int flags);
VAStatus dmd_SetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette);
VAStatus dmd_PutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height);
VAStatus dmd_CreateSubpicture(VADriverContextP ctx, VAImageID image, VASubpictureID *subpicture);
VAStatus dmd_DestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture);
VAStatus dmd_SetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image);
VAStatus dmd_SetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture, unsigned int chromakey_min, unsigned int chromakey_max, unsigned int chromakey_mask);
VAStatus dmd_SetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha);
VAStatus dmd_AssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces, short src_x, short src_y, unsigned short src_width, unsigned short src_height, short dest_x, short dest_y, unsigned short dest_width, unsigned short dest_height, unsigned int flags);
VAStatus dmd_DeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces);
VAStatus dmd_LockSurface(VADriverContextP ctx, VASurfaceID surface, unsigned int *fourcc, unsigned int *luma_stride, unsigned int *chroma_u_stride, unsigned int *chroma_v_stride, unsigned int *luma_offset, unsigned int *chroma_u_offset, unsigned int *chroma_v_offset, unsigned int *buffer_name, void **buffer);
VAStatus dmd_UnlockSurface(VADriverContextP ctx, VASurfaceID surface);
VAStatus dmd_GetSurfaceAttributes(VADriverContextP dpy, VAConfigID config, VASurfaceAttrib *attrib_list, unsigned int num_attribs);
VAStatus dmd_AcquireBufferHandle(VADriverContextP ctx, VABufferID buf_id, VABufferInfo *buf_info);
VAStatus dmd_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id);
VAStatus dmd_CreateMFContext(VADriverContextP ctx, VAMFContextID *mfe_context);
VAStatus dmd_MFAddContext(VADriverContextP ctx, VAMFContextID mf_context, VAContextID context);
VAStatus dmd_MFReleaseContext(VADriverContextP ctx, VAMFContextID mf_context, VAContextID context);
VAStatus dmd_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context, VAContextID *contexts, int num_contexts);
VAStatus dmd_CreateBuffer2(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int width, unsigned int height, unsigned int *unit_size, unsigned int *pitch, VABufferID *buf_id);
VAStatus dmd_QueryProcessingRate(VADriverContextP ctx, VAConfigID config_id, VAProcessingRateParameter *proc_buf, unsigned int *processing_rate);
VAStatus dmd_SyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns);
VAStatus dmd_Copy(VADriverContextP ctx, VACopyObject *dst, VACopyObject *src, VACopyOption option);

#endif /* DMD_STUBS_H */
