/*
 * renderer.c - EGL + OpenGL ES 3.2 渲染模块
 *
 * 使用 X11 创建窗口，通过 EGL 获取 OpenGL ES 3.2 上下文，
 * 利用 GPU fragment shader 将 NV12 数据转换为 RGB 并显示。
 *
 * NV12 内存布局：
 *   Y  平面: width * height 字节（每像素 1 字节亮度）
 *   UV 平面: width * height / 2 字节（每 2x2 像素一组 U,V 交织）
 *   总计: width * height * 3 / 2 字节
 *
 * 着色器策略：
 *   - Y 平面上传为 GL_R8 纹理（单通道）
 *   - UV 平面上传为 GL_RG8 纹理（双通道 U+V）
 *   - Fragment shader 用 BT.601 标准做 YUV→RGB 转换
 */
#include "renderer.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 顶点着色器 - 全屏四边形
 * ============================================================ */
static const char *vertex_shader_src =
    "#version 300 es\n"
    "in vec2 aPos;\n"
    "in vec2 aTexCoord;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

/* ============================================================
 * 片段着色器 - NV12 → RGB (BT.601)
 *
 * Y  采样自纹理 0（R8），范围 [0,1] 映射到 [16,235]
 * UV 采样自纹理 1（RG8），范围 [0,1] 映射到 [16,240]
 *
 * BT.601 转换公式（full range 近似）:
 *   R = Y + 1.402 * (V - 0.5)
 *   G = Y - 0.344 * (U - 0.5) - 0.714 * (V - 0.5)
 *   B = Y + 1.772 * (U - 0.5)
 * ============================================================ */
static const char *fragment_shader_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D texY;\n"
    "uniform sampler2D texUV;\n"
    "void main() {\n"
    "    float y = texture(texY, vTexCoord).r;\n"
    "    vec2 uv = texture(texUV, vTexCoord).rg;\n"
    "    float u = uv.r - 0.5;\n"
    "    float v = uv.g - 0.5;\n"
    "    y = 1.1643 * (y - 0.0625);\n"
    "    float r = y + 1.5958 * v;\n"
    "    float g = y - 0.39173 * u - 0.81290 * v;\n"
    "    float b = y + 2.017 * u;\n"
    "    fragColor = vec4(clamp(r, 0.0, 1.0),\n"
    "                     clamp(g, 0.0, 1.0),\n"
    "                     clamp(b, 0.0, 1.0), 1.0);\n"
    "}\n";

/* ============================================================
 * 内部上下文结构
 * ============================================================ */
struct RendererContext {
    /* X11 */
    Display *x_display;
    Window   x_window;
    int      x_width;
    int      x_height;

    /* EGL */
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLContext  egl_context;

    /* OpenGL ES */
    GLuint program;
    GLuint tex_y;
    GLuint tex_uv;
    GLuint vao;
    GLuint vbo;
    GLint  loc_tex_y;
    GLint  loc_tex_uv;

    /* 当前渲染的纹理尺寸 */
    int tex_width;
    int tex_height;
};

/* ============================================================
 * 全屏四边形顶点数据
 * 格式: x, y, u, v
 * ============================================================ */
static const float quad_vertices[] = {
    /* 位置        纹理坐标 */
    -1.0f, -1.0f,  0.0f, 1.0f,  /* 左下 */
     1.0f, -1.0f,  1.0f, 1.0f,  /* 右下 */
    -1.0f,  1.0f,  0.0f, 0.0f,  /* 左上 */
     1.0f,  1.0f,  1.0f, 0.0f,  /* 右上 */
};

/* ============================================================
 * 编译着色器
 * ============================================================ */
static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[renderer] 着色器编译失败:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* ============================================================
 * 链接着色器程序
 * ============================================================ */
static GLuint create_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[renderer] 程序链接失败:\n%s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* ============================================================
 * 初始化渲染器
 * ============================================================ */
RendererContext *renderer_init(int width, int height, const char *title)
{
    RendererContext *ctx = calloc(1, sizeof(RendererContext));
    if (!ctx) return NULL;

    /* ---- X11 ---- */
    ctx->x_display = XOpenDisplay(NULL);
    if (!ctx->x_display) {
        fprintf(stderr, "[renderer] 无法打开 X11 显示（确保 XWayland 可用）\n");
        goto fail;
    }

    Window root = RootWindow(ctx->x_display, DefaultScreen(ctx->x_display));

    /* 设置窗口属性 */
    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask | ResizeRedirectMask;

    ctx->x_window = XCreateWindow(ctx->x_display, root,
                                   0, 0, width, height, 0,
                                   CopyFromParent, InputOutput,
                                   CopyFromParent,
                                   CWEventMask, &attr);

    if (!ctx->x_window) {
        fprintf(stderr, "[renderer] 无法创建 X11 窗口\n");
        goto fail;
    }

    /* 设置窗口标题 */
    XStoreName(ctx->x_display, ctx->x_window, title ? title : "Decode Client");
    XMapWindow(ctx->x_display, ctx->x_window);
    XFlush(ctx->x_display);
    ctx->x_width = width;
    ctx->x_height = height;

    /* ---- EGL ---- */
    ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->x_display);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "[renderer] eglGetDisplay 失败\n");
        goto fail;
    }

    EGLint major, minor;
    if (!eglInitialize(ctx->egl_display, &major, &minor)) {
        fprintf(stderr, "[renderer] eglInitialize 失败\n");
        goto fail;
    }

    printf("[renderer] EGL %d.%d\n", major, minor);

    /* 选择 EGL 配置 */
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(ctx->egl_display, config_attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        fprintf(stderr, "[renderer] eglChooseConfig 失败\n");
        goto fail;
    }

    /* 创建窗口表面 */
    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, config,
                                               (EGLNativeWindowType)ctx->x_window,
                                               NULL);
    if (ctx->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[renderer] eglCreateWindowSurface 失败\n");
        goto fail;
    }

    /* 创建 OpenGL ES 3.2 上下文 */
    EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE
    };
    ctx->egl_context = eglCreateContext(ctx->egl_display, config,
                                         EGL_NO_CONTEXT, ctx_attribs);
    if (ctx->egl_context == EGL_NO_CONTEXT) {
        /* 回退到 ES 3.0 */
        ctx_attribs[3] = 0;
        ctx->egl_context = eglCreateContext(ctx->egl_display, config,
                                             EGL_NO_CONTEXT, ctx_attribs);
        if (ctx->egl_context == EGL_NO_CONTEXT) {
            fprintf(stderr, "[renderer] eglCreateContext 失败\n");
            goto fail;
        }
    }

    eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context);
    eglSwapInterval(ctx->egl_display, 1); /* VSync */

    printf("[renderer] OpenGL ES: %s\n", glGetString(GL_VERSION));
    printf("[renderer] 渲染器: %s\n", glGetString(GL_RENDERER));

    /* ---- OpenGL ES 着色器 ---- */
    ctx->program = create_program(vertex_shader_src, fragment_shader_src);
    if (!ctx->program) goto fail;

    ctx->loc_tex_y  = glGetUniformLocation(ctx->program, "texY");
    ctx->loc_tex_uv = glGetUniformLocation(ctx->program, "texUV");

    /* ---- VAO / VBO ---- */
    glGenVertexArrays(1, &ctx->vao);
    glGenBuffers(1, &ctx->vbo);

    glBindVertexArray(ctx->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    /* aPos (location 0) */
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    /* aTexCoord (location 1) */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    /* ---- 纹理（初始创建，后续更新尺寸） ---- */
    glGenTextures(1, &ctx->tex_y);
    glGenTextures(1, &ctx->tex_uv);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    printf("[renderer] 渲染器初始化成功 (%dx%d)\n", width, height);
    return ctx;

fail:
    renderer_destroy(ctx);
    return NULL;
}

/* ============================================================
 * 渲染一帧 NV12
 * ============================================================ */
int renderer_draw_frame(RendererContext *ctx,
                        const uint8_t *y_data, const uint8_t *uv_data,
                        int width, int height, int stride)
{
    if (!ctx || !y_data || !uv_data) return -1;
    if (stride <= 0) stride = width;

    /* 检查窗口是否被 resize */
    XWindowAttributes wattr;
    XGetWindowAttributes(ctx->x_display, ctx->x_window, &wattr);
    if (wattr.width != ctx->x_width || wattr.height != ctx->x_height) {
        ctx->x_width = wattr.width;
        ctx->x_height = wattr.height;
        glViewport(0, 0, ctx->x_width, ctx->x_height);
    }

    /* 如果帧尺寸变化，重新分配纹理 */
    if (width != ctx->tex_width || height != ctx->tex_height) {
        /* Y 纹理: R8 格式, width x height */
        glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, NULL);

        /* UV 纹理: RG8 格式, (width/2) x (height/2) */
        glBindTexture(GL_TEXTURE_2D, ctx->tex_uv);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height / 2, 0,
                     GL_RG, GL_UNSIGNED_BYTE, NULL);

        glBindTexture(GL_TEXTURE_2D, 0);
        ctx->tex_width = width;
        ctx->tex_height = height;
    }

    /* 源缓冲行距可能大于显示宽度（解码器按 128 对齐）。
     * GL_UNPACK_ROW_LENGTH 让 GL 按真实行距读取，只取左上 width x height 区域；
     * 不设置的话 GL 按 width 连续读取，画面会逐行横向错位。 */
    if (stride > width) glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);

    /* 上传 Y 平面数据 */
    glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RED, GL_UNSIGNED_BYTE, y_data);

    /* 上传 UV 平面数据。UV 平面每行有 stride/2 个 RG 像素对，
     * 行长按像素对计而非字节。 */
    if (stride > width) glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 2);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_uv);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2,
                    GL_RG, GL_UNSIGNED_BYTE, uv_data);

    /* 复位，避免影响后续任何上传 */
    if (stride > width) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glBindTexture(GL_TEXTURE_2D, 0);

    /* 渲染 */
    glViewport(0, 0, ctx->x_width, ctx->x_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(ctx->program);

    /* 绑定 Y 纹理到 texture unit 0 */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
    glUniform1i(ctx->loc_tex_y, 0);

    /* 绑定 UV 纹理到 texture unit 1 */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_uv);
    glUniform1i(ctx->loc_tex_uv, 1);

    /* 绘制全屏四边形 */
    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    /* 交换缓冲区 */
    eglSwapBuffers(ctx->egl_display, ctx->egl_surface);

    return 0;
}

/* ============================================================
 * 处理 X11 事件
 * ============================================================ */
int renderer_poll_events(RendererContext *ctx)
{
    if (!ctx || !ctx->x_display) return 1;

    while (XPending(ctx->x_display)) {
        XEvent event;
        XNextEvent(ctx->x_display, &event);

        switch (event.type) {
        case KeyPress: {
            /* 按 q 或 ESC 退出 */
            char buf[8];
            KeySym ks;
            XLookupString(&event.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_q || ks == XK_Escape) {
                return 1;
            }
            break;
        }
        case ConfigureNotify: {
            /* 窗口大小变化 */
            ctx->x_width  = event.xconfigure.width;
            ctx->x_height = event.xconfigure.height;
            break;
        }
        case DestroyNotify:
            return 1;
        default:
            break;
        }
    }
    return 0;
}

void renderer_get_size(RendererContext *ctx, int *width, int *height)
{
    if (width)  *width  = ctx ? ctx->x_width  : 0;
    if (height) *height = ctx ? ctx->x_height : 0;
}

void renderer_set_title(RendererContext *ctx, const char *title)
{
    if (ctx && ctx->x_display && ctx->x_window) {
        XStoreName(ctx->x_display, ctx->x_window, title ? title : "");
        XFlush(ctx->x_display);
    }
}

/* ============================================================
 * 销毁渲染器
 * ============================================================ */
void renderer_destroy(RendererContext *ctx)
{
    if (!ctx) return;

    if (ctx->tex_y)       glDeleteTextures(1, &ctx->tex_y);
    if (ctx->tex_uv)      glDeleteTextures(1, &ctx->tex_uv);
    if (ctx->vao)         glDeleteVertexArrays(1, &ctx->vao);
    if (ctx->vbo)         glDeleteBuffers(1, &ctx->vbo);
    if (ctx->program)     glDeleteProgram(ctx->program);

    if (ctx->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        if (ctx->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglTerminate(ctx->egl_display);
    }

    if (ctx->x_display) {
        if (ctx->x_window) XDestroyWindow(ctx->x_display, ctx->x_window);
        XCloseDisplay(ctx->x_display);
    }

    free(ctx);
}
