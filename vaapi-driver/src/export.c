/* vExportSurfaceHandle: Export the surface to dmabuf (DRM_PRIME_2).
 *
 * Why must it be done: **Firefox takes the frame only this way**.
 * `FFmpegVideoDecoder: :CreateImageVAAPI ' (FFmpegVideoDecoder.cpp:1649)
 * `GetVAAPIsureDescriptor '  ⁇ ExportSurfaceHandle ' ,
 * Failed returns `NS_ERROR_DOM_MEDIA_DECODE_ERR ', and the player will then be enabled to perform the processFlush()
 * and recreated as **Soft **. There was no error log, ** no return to the copy path** ** **
 * The symptoms are "hardly uncollaborate a frame and then permanently softly decipher" and extremely difficult to locate (we only find it by adding logs to the unrealized treasuries).
 *
 * ffmpeg command line does not need it: `hwdownload ' Walk vaDeriveImage + vaMapBuffer.
 * So it's been "ffmpeg all green, browser still."
 *
 * This is not a zero copy. MediaCodec, that output memory we can't get.
 * No /dev/dma_head, NDK does not reveal an output buffer fd. The way it is distributed is by itself
 * inside an msm_drm dumb buffer (see surface_alloc_dumb in decode.c),
 * The frame of the return of the daemon is directly copied into this box to export the memory -- the number of copies is the same as before.
 * But there's more of the key "capable of being imported into GL/Synthesizer."
 */

#include <stdlib.h>
#include <fcntl.h> /* O_CLOEXEC — drm.h DRM_CLOEXEC is its alias */
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm_fourcc.h>

#include "driver.h"

VAStatus dmd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
                                 uint32_t mem_type, uint32_t flags,
                                 void *descriptor)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !descriptor)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Support only DRM_PRIME_2. Firefox transmits this; the old DRM_PRIME(v1)
     * Different structures, without stating support for security compared to wrong structures. */
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        dmd_log("ExportSurfaceHandle: Unsupported mem_type=0x%x\n", mem_type);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    /* Don't wait here for the frame to be ready.
     *
     * Turns out this is the dmd_survey_wait() for the reason that Firebox was obtained after the decoding frame was obtained.
     * ". The Firefox set up, but the order of **Chrome is the opposite. ** It's:
     * Call the ExportSurfaceHandle after the CreateSurfaces and before submitting any decoding
     * dmabuf fd to build output texture and get fd before it starts feeding the stream.
     *
     * So the surface is still IDLE, dmd_surface_wait() refused by design (it is read out to read one)
     * Uncoded surface, export returns INVALID_SURFACE, Chrame profile
     *     media/gpu/vaapi/vaapi_wrapper.cc:2756
     *     vaExportSurfaceHandle failed, VA error: invalid VASurfaceID
     * The entire hard-line path was then abandoned -- the actual "incoming 0 units, received 0 frames".
     *
     * Key difference: ** Export dmabuf does not read pixel**, only hand over fd with geometry.
     * Buffer life cycle is held by the surface, when writing is decoded; consumers would have wanted it.
     * The real need is to decode the premise that it has to be.
     * GetImage / DeriveImage that read pixel path where IDLE intercepts are unchanged. */

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface_id);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        const struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->derived_from == surface_id &&
            (img->handle_acquired || img->mapped)) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }

    if (!s->exportable || !s->dumb_handle || s->dumb_drm_fd < 0) {
        /* The distribution is returned to normal header (dumb buffer is not available).
         * Do not give a fake ld if you clearly say you don't support it. */
        pthread_mutex_unlock(&drv->lock);
        dmd_log("ExportSurfaceHandle: surface %u Not export memory\n",
                (unsigned)surface_id);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    unsigned int stride = s->stride;
    unsigned int slice_h = s->slice_height;
    unsigned int disp_w = s->width;
    unsigned int disp_h = s->height;
    int ten_bit = s->format == VA_RT_FORMAT_YUV420_10;
    uint32_t handle = s->dumb_handle;
    int drm_fd = s->dumb_drm_fd;
    size_t total = s->dumb_size;
    const unsigned char *allocation = s->data;
    unsigned int format = s->format;
    pthread_mutex_unlock(&drv->lock);

    /* Standard P010 Place valid 10bit at the height of each 16bit LE (15:6),
     * The separation layer is therefore R16/GR161616; this is also the P010 plane for Mesa.
     * Based on 16bit UNORM sample not dark 64 times: neutral UV storage value 0x8000,
     * Not 0x0200. Only a private format that is low-levelly aligned requires another explanation and cannot be assumed as P010.
     * The EGL/GL/Vulkan path, which correctly does not represent the consumer, will support these plane formats. */

    /* Export fd without lock: octl is an internal approval call and the caller gets the fd and it is responsible for close. */
    struct drm_prime_handle prime;
    memset(&prime, 0, sizeof(prime));
    prime.handle = handle;
    /* DRM_CLOEXEC is the alias O_CLOEXEC (drm.h), which is used to avoid extra inclusion of fcntl.h.
     * fd Must CLOEXEC: Drive in browser processes, leaking to subprocess is a security issue. */
    prime.flags = DRM_CLOEXEC;
    if (flags & VA_EXPORT_SURFACE_WRITE_ONLY)
        prime.flags |= DRM_RDWR;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        dmd_log("ExportSurfaceHandle: PRIME_HANDLE_TO_FD failed\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* The ioctl ran without the driver lock. A concurrent destroy/recreate or
     * geometry change must not publish a descriptor or write permission for a
     * different allocation. Keep the lock while reading pixels for diagnostics
     * too: the previous cached surface pointer could otherwise be unmapped. */
    pthread_mutex_lock(&drv->lock);
    s = dmd_find_surface_locked(drv, surface_id);
    if (!s || !s->exportable || s->dumb_handle != handle ||
        s->dumb_drm_fd != drm_fd || s->dumb_size != total ||
        s->data != allocation || s->format != format ||
        s->stride != stride || s->slice_height != slice_h ||
        s->width != disp_w || s->height != disp_h) {
        pthread_mutex_unlock(&drv->lock);
        close(prime.fd);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        const struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->derived_from == surface_id &&
            (img->handle_acquired || img->mapped)) {
            pthread_mutex_unlock(&drv->lock);
            close(prime.fd);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }

    VADRMPRIMESurfaceDescriptor *desc =
        (VADRMPRIMESurfaceDescriptor *)descriptor;
    memset(desc, 0, sizeof(*desc));

    desc->fourcc = ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    desc->width = disp_w;
    desc->height = disp_h;

    /* A single dmabuf object carries two plane planes of Y and UV. */
    desc->num_objects = 1;
    desc->objects[0].fd = prime.fd;
    desc->objects[0].size = (uint32_t)total;
    desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

    /* Firefox and Chrome by VA_EXPORT_SURFACE_SEPARATE_LAYERS (Chrome Factoring)
     * flags=0x5), requesting Y/ UV to form one layer each.
     * Merge into one layer (NV12/P010 single player) only for the distribution of COMPOSED_LAYERS. */
    if (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
        desc->num_layers = 1;
        desc->layers[0].drm_format =
            ten_bit ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = stride;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1] = (uint32_t)stride * slice_h;
        desc->layers[0].pitch[1] = stride;
    } else {
        desc->num_layers = 2;
        /* Y plane: NV12 for R8; MSB for P010 for R16. */
        desc->layers[0].drm_format = ten_bit ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = stride;
        /* UV plane: interwoven Cb/Cr, with a reduction in logic width, with 2/4 bytes per sample,
         * Bytes pitch is the same as Y. The low letter for GR161616 is R(Cb) and the high word is G(Cr).
         * With storage slick_height instead of display height, keep Y-Pick-Up Fill. */
        desc->layers[1].drm_format = ten_bit ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = (uint32_t)stride * slice_h;
        desc->layers[1].pitch[0] = stride;
    }

    /* Diagnosed black frame: The average lightness of the plane is calculated for Y and entered into the log at the time of the sample of DMD_VA_LUMA=1.
     *
     * ⁇  This count** only means to the Firefox path**. Chrame is exported before decoding
     * This is a time when pixels are destined to be zero -- 0.4.7 for live measurements.
     * One MSE pressure, 162 times all "Black Frames," which itself is normal and written.
     * The surface. So don't use it to frame chrome, and it'll be counter-conclusive.
     *
     * Why do you need it? Firefox is not exported until after vSyncSurface, the sampling site is dropped.
     * When the pixels are finished, the following section of the UV is established. */
    if (getenv("DMD_VA_LUMA")) {
        const unsigned char *y = s->data;
        if (y && s->data_size >= (size_t)stride * 16) {
            size_t y_lim = (size_t)stride * slice_h;
            if (y_lim > s->data_size) y_lim = s->data_size;
            double sum = 0; int cnt = 0;
            for (size_t k = 0; k < y_lim; k += 1499) { sum += y[k]; cnt++; }
            double mean = cnt ? sum / cnt : 0;

            /* Only Y cannot be judged green: the pure green verdict is in UV.
             * UV for NV12 is completely bound by BT.601 and RGB gets R ⁇ 0, G ⁇ 135, B ⁇ 0,
             * That's the green color of the sign; and the normal UV should be around 128 fluctuations.
             * (128 means colorless). So here's the UV mean ratio to "approx. 0"
             * "The pixels given to the browser are green."
             * KWin does not have to rely on the screenshot - it is safe to refuse the unauthorized process to capture the screen.
             * XWayland does not get the content of the Wayland original window. */
            double uv_sum = 0; int uv_cnt = 0, uv_zero = 0;
            size_t uv_end = y_lim + (size_t)stride * slice_h / 2;
            if (uv_end > s->data_size) uv_end = s->data_size;
            for (size_t k = y_lim; k < uv_end; k += 1499) {
                uv_sum += y[k];
                if (y[k] < 16) uv_zero++;
                uv_cnt++;
            }
            double uv_mean = uv_cnt ? uv_sum / uv_cnt : 0;
            double uv_zero_pct = uv_cnt ? 100.0 * uv_zero / uv_cnt : 0;
            int greenish = (uv_cnt > 0 && uv_zero_pct > 80.0);

            dmd_log("pixels: surface=%u Y Mean %.1f UV Mean %.1f UV Near zero. %.0f%%%s%s\n",
                    (unsigned)surface_id, mean, uv_mean, uv_zero_pct,
                    mean < 20.0 ? "  ← Black Frame" : "",
                    greenish ? "  ← Pure green!" : "");
        }
    }

    /* The real user's fly-in.
     *
     * Why must you hit Firefox in the drive by libva's dryer vtable to enter here,
     * Do not go public sign vExportSurfaceHandle(), so LD_PRELOAD cannot stop
     * (Reconfirmed that RDD did load the Hook library, but no records were generated).
     * Only here can be sure whether it's going to COMPOSED or SEPARATE branch. */
    dmd_log("ExportSurfaceHandle: surface=%u -> fd=%d %ux%u stride=%u "
            "slice_h=%u uv_offset=%u flags=0x%x%s%s%s layers=%u fmt0=%.4s%s%s\n",
            (unsigned)surface_id, prime.fd, disp_w, disp_h, stride,
            slice_h, (unsigned)(stride * slice_h), flags,
            (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) ? " COMPOSED" : "",
            (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) ? " SEPARATE" : "",
            (flags & VA_EXPORT_SURFACE_READ_ONLY) ? " RO" : "",
            desc->num_layers, (const char *)&desc->layers[0].drm_format,
            desc->num_layers > 1 ? " fmt1=" : "",
            desc->num_layers > 1
                ? (const char *)&desc->layers[1].drm_format : "");

    /* Mark this surface and walk with the "Exit dmabuf to sample yourself".
     * EndPicture therefore decides whether pixels must be synchronized to the surface before returning...
     * Only such consumers (Chrome) need ffmpeg/ Firefox with a cap to go around.
     * WRITE export is a separate producer contract (OBS texture input). It
     * does not change IDLE/PENDING/READY or make undecoded pixels readable. */
    s->exported = 1;
    if (flags & VA_EXPORT_SURFACE_WRITE_ONLY)
        s->external_write_exported = 1;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}
