/*
 * renderer.h - EGL + OpenGL ES 渲染模块
 *
 * 使用 EGL + OpenGL ES 3.2 将 NV12 格式的视频帧渲染到 X11 窗口。
 * NV12→RGB 转换在 GPU fragment shader 中完成。
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

/* 渲染器上下文（前向声明） */
typedef struct RendererContext RendererContext;

/*
 * 初始化渲染器，创建 X11 窗口和 EGL 上下文。
 * width/height: 窗口初始尺寸。
 * title: 窗口标题。
 * 返回渲染器上下文，NULL 表示失败。
 */
RendererContext *renderer_init(int width, int height, const char *title);

/*
 * 渲染一帧 NV12 数据到窗口。
 * y_data/uv_data: NV12 的 Y 平面和 UV 交织平面数据。
 * width/height: 帧的尺寸。
 * 返回 0 成功，-1 失败。
 *
 * width/height 是要显示的区域尺寸；stride 是源缓冲的行距，
 * 解码器输出常按 128 对齐而大于 width（1080p 输出 stride 可能为 1920 而
 * slice_height 为 1088）。传 0 表示 stride 等于 width。
 * 用 width 当行距会导致画面逐行横向错位。
 */
int renderer_draw_frame(RendererContext *ctx,
                        const uint8_t *y_data, const uint8_t *uv_data,
                        int width, int height, int stride);

/*
 * 处理窗口事件（非阻塞），返回是否应该退出。
 * 1 = 应该退出（收到关闭事件），0 = 继续。
 */
int renderer_poll_events(RendererContext *ctx);

/*
 * 获取窗口宽度/高度（处理 resize 后可能变化）。
 */
void renderer_get_size(RendererContext *ctx, int *width, int *height);

/*
 * 设置窗口标题。
 */
void renderer_set_title(RendererContext *ctx, const char *title);

/*
 * 销毁渲染器并释放资源。
 */
void renderer_destroy(RendererContext *ctx);

#endif /* RENDERER_H */
