/* SPDX-License-Identifier: Apache-2.0 */
/* Standalone P010 split-layer EGL/GLES numerical probe, no VA driver changes.
 * Build: make probe-p010-egl (optional EGL/GLES dependencies for this target).
 * Or: cc -O2 -g -std=c11 -Wall -Wextra -Werror tools/probe_p010_egl.c \
 *     $(pkg-config --cflags --libs egl glesv2 libdrm) -lm -o probe_p010_egl
 * Run as the desktop user with MESA_LOADER_DRIVER_OVERRIDE=msm.
 * --selftest needs no device/display. Real GPU use is explicit, never automatic.
 * See tools/probe_p010_egl.md for scope, dependencies and interpretation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct probe {
    int drm_fd, dma_fd;
    uint32_t handle, pitch, slice, uv_offset;
    size_t size;
    uint8_t *map;
    unsigned width, height;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLImageKHR image[2];
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image;
    GLuint texture[2], framebuffer, output_texture, program, vao;
    int current;
};

static int extension(const char *list, const char *name)
{
    size_t n = strlen(name);
    if (!list) return 0;
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

static unsigned sample10(unsigned channel, unsigned x, unsigned y)
{
    static const unsigned values[] = {
        0, 1, 2, 3, 4, 63, 64, 65, 128, 255, 256, 511,
        512, 513, 767, 940, 941, 960, 1022, 1023
    };
    return values[(x + 7u*y + 11u*channel) % (sizeof(values)/sizeof(values[0]))];
}

static void put_word(uint8_t *p, unsigned word)
{
    p[0] = (uint8_t)word;
    p[1] = (uint8_t)(word >> 8);
}

static unsigned get_word(const uint8_t *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

static int selftest(void)
{
    unsigned checked = 0;
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned y = 0; y < 34; ++y)
            for (unsigned x = 0; x < 64; ++x) {
                unsigned word = sample10(c, x, y) << 6;
                uint8_t bytes[2];
                put_word(bytes, word);
                float normalized = (float)word / 65535.0f;
                unsigned restored = (unsigned)floorf(normalized * 65535.0f + .5f);
                if (get_word(bytes) != word || restored != word || (word & 63u)) return 1;
                ++checked;
            }
    printf("SELFTEST PASS: %u little-endian high-10-bit words; "
           "white=0x%04x normalized=%.9f (low-aligned wrong value=%.9f)\n",
           checked, 1023u << 6, (1023.0*64.0)/65535.0, 1023.0/65535.0);
    return 0;
}

static int gl_error(const char *stage)
{
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return 0;
    fprintf(stderr, "FAIL %s: GL error 0x%04x\n", stage, e);
    return -1;
}

static int sync_dma(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    int r;
    do r = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); while (r < 0 && errno == EINTR);
    if (r < 0) fprintf(stderr, "FAIL DMA_BUF_IOCTL_SYNC(0x%" PRIx64 "): %s\n",
                       flags, strerror(errno));
    return r;
}

static int allocate_pattern(struct probe *p, const char *device)
{
    p->drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (p->drm_fd < 0) { perror("FAIL open DRM node"); return -1; }
    struct drm_version version = {0};
    char name[128] = {0};
    version.name = name; version.name_len = sizeof(name)-1;
    if (ioctl(p->drm_fd, DRM_IOCTL_VERSION, &version) == 0)
        printf("DRM node=%s driver=%s version=%d.%d.%d\n", device, name,
               version.version_major, version.version_minor, version.version_patchlevel);
    struct drm_mode_create_dumb create = {0};
    create.width = (p->width * 2u + 127u) & ~127u;
    p->slice = (p->height + 31u) & ~31u;
    create.height = p->slice * 3u / 2u;
    create.bpp = 8; /* width is bytes, matching the VA driver's allocation. */
    if (ioctl(p->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        perror("FAIL CREATE_DUMB"); return -1;
    }
    p->handle = create.handle;
    p->pitch = create.pitch;
    uint64_t offset = (uint64_t)p->pitch * p->slice;
    uint64_t needed = offset + (uint64_t)p->pitch * p->slice / 2u;
    if (p->pitch < p->width*2u || (p->pitch & 1u) ||
        create.size < needed || create.size > SIZE_MAX || offset > INT_MAX) {
        fprintf(stderr, "FAIL unexpected dumb geometry pitch=%u size=%" PRIu64 "\n",
                p->pitch, (uint64_t)create.size); return -1;
    }
    p->uv_offset = (uint32_t)offset;
    p->size = (size_t)create.size;
    struct drm_mode_map_dumb map = { .handle = p->handle };
    if (ioctl(p->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("FAIL MAP_DUMB"); return -1;
    }
    p->map = mmap(NULL, p->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  p->drm_fd, (off_t)map.offset);
    if (p->map == MAP_FAILED) { p->map = NULL; perror("FAIL mmap dumb"); return -1; }
    struct drm_prime_handle prime = {
        .handle = p->handle, .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1
    };
    if (ioctl(p->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
        perror("FAIL PRIME_HANDLE_TO_FD"); return -1;
    }
    p->dma_fd = prime.fd;
    if (sync_dma(p->dma_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) return -1;
    memset(p->map, 0xa5, p->size); /* padding sentinel, never part of visible pixels */
    for (unsigned y = 0; y < p->height; ++y)
        for (unsigned x = 0; x < p->width; ++x)
            put_word(p->map + (size_t)y*p->pitch + x*2u, sample10(0,x,y) << 6);
    for (unsigned y = 0; y < p->height/2u; ++y)
        for (unsigned x = 0; x < p->width/2u; ++x) {
            uint8_t *uv = p->map + p->uv_offset + (size_t)y*p->pitch + x*4u;
            put_word(uv, sample10(1,x,y) << 6);
            put_word(uv+2, sample10(2,x,y) << 6);
        }
    if (sync_dma(p->dma_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) return -1;
    printf("P010 allocation=%ux%u pitch=%u slice=%u uv_offset=%u size=%zu "
           "word=sample10<<6 little-endian\n", p->width, p->height, p->pitch,
           p->slice, p->uv_offset, p->size);
    return 0;
}

static int initialize_egl(struct probe *p, const char *platform)
{
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL client extensions=%s\n", client ? client : "(none)");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLint major = 0, minor = 0;
    if ((!strcmp(platform,"auto") || !strcmp(platform,"surfaceless")) && get_platform &&
        extension(client,"EGL_MESA_platform_surfaceless")) {
        p->display = get_platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (p->display != EGL_NO_DISPLAY && eglInitialize(p->display,&major,&minor))
            printf("EGL platform=surfaceless\n");
        else {
            fprintf(stderr,"surfaceless initialization failed EGL=0x%x\n",eglGetError());
            p->display = EGL_NO_DISPLAY;
        }
    }
    if (p->display == EGL_NO_DISPLAY && strcmp(platform,"surfaceless")) {
        p->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (p->display == EGL_NO_DISPLAY || !eglInitialize(p->display,&major,&minor)) {
            fprintf(stderr,"FAIL default EGL initialization: 0x%x\n",eglGetError());
            p->display = EGL_NO_DISPLAY; return -1;
        }
        printf("EGL platform=default (EGL_PLATFORM/DISPLAY/WAYLAND_DISPLAY selection)\n");
    }
    if (p->display == EGL_NO_DISPLAY) { fprintf(stderr,"FAIL no EGL display\n"); return -1; }
    const char *extensions = eglQueryString(p->display,EGL_EXTENSIONS);
    printf("EGL version=%d.%d vendor=%s\nEGL extensions=%s\n",major,minor,
           eglQueryString(p->display,EGL_VENDOR),extensions ? extensions : "(none)");
    if (!extension(extensions,"EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr,"FAIL EGL_EXT_image_dma_buf_import missing\n"); return -1;
    }
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE,EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_NONE
    };
    EGLConfig config;
    EGLint count;
    int pbuffer = 1;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr,"FAIL bind ES API EGL=0x%x\n",eglGetError()); return -1;
    }
    if (!eglChooseConfig(p->display,attributes,&config,1,&count) || count < 1) {
        (void)eglGetError();
        /* Wayland displays may expose only window configs. Still keep the
         * probe windowless by rendering to our own FBO with a surfaceless
         * context on that already initialized desktop display. */
        const EGLint fallback[] = {
            EGL_SURFACE_TYPE,0,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE
        };
        if (!extension(extensions,"EGL_KHR_surfaceless_context") ||
            !eglChooseConfig(p->display,fallback,&config,1,&count) || count < 1) {
            fprintf(stderr,"FAIL no ES3 offscreen config EGL=0x%x\n",eglGetError()); return -1;
        }
        pbuffer = 0;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    p->context = eglCreateContext(p->display,config,EGL_NO_CONTEXT,context_attributes);
    const EGLint surface_attributes[] = {
        EGL_WIDTH,(EGLint)p->width,EGL_HEIGHT,(EGLint)p->height,EGL_NONE
    };
    if (pbuffer) p->surface = eglCreatePbufferSurface(p->display,config,surface_attributes);
    if (p->context == EGL_NO_CONTEXT || (pbuffer && p->surface == EGL_NO_SURFACE) ||
        !eglMakeCurrent(p->display,p->surface,p->surface,p->context)) {
        fprintf(stderr,"FAIL ES3 pbuffer context EGL=0x%x\n",eglGetError()); return -1;
    }
    p->current = 1;
    printf("EGL drawable=%s\n",pbuffer ? "pbuffer" : "surfaceless-context/FBO on desktop display");
    printf("GL vendor=%s\nGL renderer=%s\nGL version=%s\nGL shading_language=%s\n",
           glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION),
           glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint number = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS,&number);
    int norm16 = 0;
    printf("GL extensions=");
    for (GLint i = 0; i < number; ++i) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS,(GLuint)i);
        printf("%s%s",i ? " " : "",e);
        if (!strcmp(e,"GL_EXT_texture_norm16")) norm16 = 1;
    }
    putchar('\n');
    if (!norm16) { fprintf(stderr,"FAIL GL_EXT_texture_norm16 missing\n"); return -1; }
    p->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p->bind_image = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!p->create_image || !p->destroy_image || !p->bind_image) {
        fprintf(stderr,"FAIL missing EGL image entry points\n"); return -1;
    }
    return gl_error("initialize ES3");
}

static int import_planes(struct probe *p)
{
    const uint32_t formats[] = {DRM_FORMAT_R16,DRM_FORMAT_GR1616};
    glGenTextures(2,p->texture);
    for (unsigned i = 0; i < 2; ++i) {
        const EGLint attributes[] = {
            EGL_WIDTH,(EGLint)(p->width >> i), EGL_HEIGHT,(EGLint)(p->height >> i),
            EGL_LINUX_DRM_FOURCC_EXT,(EGLint)formats[i],
            EGL_DMA_BUF_PLANE0_FD_EXT,p->dma_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,i ? (EGLint)p->uv_offset : 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)p->pitch,EGL_NONE
        };
        p->image[i] = p->create_image(p->display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,
                                       NULL,attributes);
        if (p->image[i] == EGL_NO_IMAGE_KHR) {
            fprintf(stderr,"FAIL import %s EGL=0x%x\n",i ? "GR1616" : "R16",eglGetError());
            return -1;
        }
        glActiveTexture(GL_TEXTURE0+i);
        glBindTexture(GL_TEXTURE_2D,p->texture[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        p->bind_image(GL_TEXTURE_2D,(GLeglImageOES)p->image[i]);
        if (gl_error(i ? "bind GR1616 as GL_TEXTURE_2D" : "bind R16 as GL_TEXTURE_2D") < 0)
            return -1;
        printf("Imported layer%u DRM=%s fourcc=0x%08x %ux%u offset=%u pitch=%u "
               "sampler=sampler2D expected_GL=%s\n",i,i ? "GR1616" : "R16",formats[i],
               p->width >> i,p->height >> i,i ? p->uv_offset : 0,p->pitch,
               i ? "RG16_EXT (0x822c)" : "R16_EXT (0x822a)");
        /* ES3.1+ exposes the actual imported internal format. Numerical sample
         * verification remains mandatory even if only an ES3.0 context exists. */
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION,&major); glGetIntegerv(GL_MINOR_VERSION,&minor);
        if (major > 3 || (major == 3 && minor >= 1)) {
            typedef void (GL_APIENTRYP level_proc)(GLenum,GLint,GLenum,GLint *);
            level_proc query = (level_proc)eglGetProcAddress("glGetTexLevelParameteriv");
            if (query) {
                GLint actual = 0;
                query(GL_TEXTURE_2D,0,0x1003 /* GL_TEXTURE_INTERNAL_FORMAT */,&actual);
                if (gl_error("query imported internal format") < 0) return -1;
                printf("Actual GL layer%u internal_format=0x%x\n",i,actual);
            }
        }
    }
    return 0;
}

static GLuint shader(GLenum type, const char *text)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s,1,&text,NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        char log[4096]; GLsizei length = 0;
        glGetShaderInfoLog(s,sizeof(log),&length,log);
        fprintf(stderr,"FAIL compile shader: %.*s\n",(int)length,log);
        glDeleteShader(s); return 0;
    }
    return s;
}

static int prepare_draw(struct probe *p)
{
    const char *vertex =
        "#version 300 es\n"
        "void main(){vec2 v=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(v*2.0-1.0,0.0,1.0);}\n";
    const char *fragment =
        "#version 300 es\nprecision highp float; precision highp int;\n"
        "uniform highp sampler2D yplane; uniform highp sampler2D uvplane;\n"
        "uniform int channel; out vec4 result;\n"
        "void main(){ivec2 p=ivec2(gl_FragCoord.xy);"
        "float v=channel==0?texelFetch(yplane,p,0).r:"
        "(channel==1?texelFetch(uvplane,p/2,0).r:texelFetch(uvplane,p/2,0).g);"
        "uint word=uint(floor(clamp(v,0.0,1.0)*65535.0+0.5));"
        "result=vec4(float(word&255u)/255.0,float(word>>8)/255.0,0.0,1.0);}\n";
    GLuint vs = shader(GL_VERTEX_SHADER,vertex), fs = shader(GL_FRAGMENT_SHADER,fragment);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return -1; }
    p->program = glCreateProgram();
    glAttachShader(p->program,vs); glAttachShader(p->program,fs);
    glLinkProgram(p->program); glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p->program,GL_LINK_STATUS,&ok);
    if (!ok) {
        char log[4096]; GLsizei length = 0;
        glGetProgramInfoLog(p->program,sizeof(log),&length,log);
        fprintf(stderr,"FAIL link program: %.*s\n",(int)length,log); return -1;
    }
    glUseProgram(p->program);
    glUniform1i(glGetUniformLocation(p->program,"yplane"),0);
    glUniform1i(glGetUniformLocation(p->program,"uvplane"),1);
    glGenVertexArrays(1,&p->vao); glBindVertexArray(p->vao);
    glGenTextures(1,&p->output_texture);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,p->output_texture);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,(GLsizei)p->width,(GLsizei)p->height);
    glGenFramebuffers(1,&p->framebuffer); glBindFramebuffer(GL_FRAMEBUFFER,p->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,p->output_texture,0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,"FAIL output RGBA8 framebuffer incomplete\n"); return -1;
    }
    glViewport(0,0,(GLsizei)p->width,(GLsizei)p->height);
    glDisable(GL_DITHER); glDisable(GL_BLEND);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    return gl_error("prepare RGBA8 readback");
}

static int verify_samples(struct probe *p)
{
    size_t bytes = (size_t)p->width*p->height*4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) { perror("FAIL allocate readback"); return -1; }
    size_t total = 0, errors = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        glUniform1i(glGetUniformLocation(p->program,"channel"),(GLint)channel);
        glDrawArrays(GL_TRIANGLES,0,3);
        glReadPixels(0,0,(GLsizei)p->width,(GLsizei)p->height,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        if (gl_error("draw/readPixels") < 0) { free(rgba); return -1; }
        size_t bad = 0;
        for (unsigned y = 0; y < p->height; ++y)
            for (unsigned x = 0; x < p->width; ++x) {
                const uint8_t *pixel = rgba+((size_t)y*p->width+x)*4u;
                unsigned expected = sample10(channel,channel ? x/2u : x,channel ? y/2u : y) << 6;
                unsigned actual = get_word(pixel);
                if (actual != expected || pixel[2] != 0 || pixel[3] != 255) {
                    if (errors+bad < 8)
                        fprintf(stderr,"MISMATCH %c x=%u y=%u expected_word=%u got=%u "
                                "RGBA=%u,%u,%u,%u\n","YUV"[channel],x,y,expected,actual,
                                pixel[0],pixel[1],pixel[2],pixel[3]);
                    ++bad;
                }
                ++total;
            }
        printf("Channel %c samples=%u mismatches=%zu\n","YUV"[channel],p->width*p->height,bad);
        errors += bad;
    }
    free(rgba);
    printf("RESULT %s checked=%zu mismatches=%zu "
           "storage=P010-high10 normalized_denominator=65535 layer_mapping=Y:R,U:R,V:G\n",
           errors ? "FAIL" : "PASS",total,errors);
    return errors ? -1 : 0;
}

static void cleanup(struct probe *p)
{
    if (p->current) {
        glFinish();
        glDeleteFramebuffers(1,&p->framebuffer);
        glDeleteTextures(1,&p->output_texture); glDeleteTextures(2,p->texture);
        glDeleteVertexArrays(1,&p->vao); glDeleteProgram(p->program);
    }
    if (p->display != EGL_NO_DISPLAY) {
        if (p->destroy_image)
            for (unsigned i = 0; i < 2; ++i)
                if (p->image[i] != EGL_NO_IMAGE_KHR) p->destroy_image(p->display,p->image[i]);
        eglMakeCurrent(p->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (p->surface != EGL_NO_SURFACE) eglDestroySurface(p->display,p->surface);
        if (p->context != EGL_NO_CONTEXT) eglDestroyContext(p->display,p->context);
        eglTerminate(p->display);
    }
    if (p->map) munmap(p->map,p->size);
    if (p->dma_fd >= 0) close(p->dma_fd);
    if (p->handle && p->drm_fd >= 0) {
        struct drm_mode_destroy_dumb destroy = {.handle=p->handle};
        ioctl(p->drm_fd,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy);
    }
    if (p->drm_fd >= 0) close(p->drm_fd);
}

static unsigned dimension(const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(text,&end,10);
    if (errno || !*text || *end || n < 2 || n > 4096 || (n & 1u)) return 0;
    return (unsigned)n;
}

int main(int argc, char **argv)
{
    struct probe p = { .drm_fd=-1,.dma_fd=-1,.width=64,.height=34,
        .display=EGL_NO_DISPLAY,.context=EGL_NO_CONTEXT,.surface=EGL_NO_SURFACE,
        .image={EGL_NO_IMAGE_KHR,EGL_NO_IMAGE_KHR} };
    const char *device = "/dev/dri/renderD128", *platform = "auto";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i],"--selftest")) return selftest();
        if (!strcmp(argv[i],"--device") && i+1 < argc) device = argv[++i];
        else if (!strcmp(argv[i],"--platform") && i+1 < argc) platform = argv[++i];
        else if (!strcmp(argv[i],"--width") && i+1 < argc) p.width = dimension(argv[++i]);
        else if (!strcmp(argv[i],"--height") && i+1 < argc) p.height = dimension(argv[++i]);
        else {
            fprintf(stderr,"Usage: %s [--device renderD128] [--platform auto|surfaceless|default] "
                    "[--width even] [--height even] | --selftest\n",argv[0]); return 2;
        }
    }
    if (!p.width || !p.height || (strcmp(platform,"auto") && strcmp(platform,"surfaceless") &&
                                strcmp(platform,"default"))) {
        fprintf(stderr,"FAIL dimensions must be even2..4096; invalid platform rejected\n"); return 2;
    }
    setvbuf(stdout,NULL,_IOLBF,0);
    int result = 1;
    if (allocate_pattern(&p,device) == 0 && initialize_egl(&p,platform) == 0 &&
        import_planes(&p) == 0 && prepare_draw(&p) == 0 && verify_samples(&p) == 0)
        result = 0;
    cleanup(&p);
    return result;
}
