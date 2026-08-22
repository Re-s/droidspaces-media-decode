# DroidSpaces MediaCodec VA-API driver
#
# 产物名不可更改：libva 从 msm render node 推导出的驱动名就是 msm_drm，
# 只会尝试打开 <dridir>/msm_drm_drv_video.so 这一个文件（无 fallback）。
# 详见 README.md 的"无感发现机制"。
#
# 必须在容器内（aarch64）编译：本机 x86_64 交叉工具链缺 aarch64 glibc。

TARGET  := msm_drm_drv_video.so
SRCDIR  := src
BUILD   := build

# libva 只取头文件路径；driver 是被 libva dlopen 的插件，
# 符号由宿主进程提供，不要链接 libva 本体。
VA_CFLAGS := $(shell pkg-config --cflags libva 2>/dev/null)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -fPIC -Wall -Wextra -Wno-unused-parameter \
           -fvisibility=hidden -D_GNU_SOURCE $(VA_CFLAGS) -I$(SRCDIR)
LDFLAGS += -shared -Wl,--no-undefined -Wl,-z,defs
LDLIBS  += -lpthread

SOURCES := $(SRCDIR)/driver.c $(SRCDIR)/profiles.c $(SRCDIR)/stubs.c
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(SOURCES))

# 安装目录：libva 编译进去的 driver 目录（容器实测硬编码此路径）
PREFIX  ?= /usr
LIBDIR  ?= $(PREFIX)/lib/aarch64-linux-gnu
DRIDIR  ?= $(LIBDIR)/dri
DESTDIR ?=

.PHONY: all clean install uninstall check gen

all: $(BUILD)/$(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

# 确认入口符号确实导出（-fvisibility=hidden 下靠 VA_DRIVER_INIT_FUNC 的
# 可见性属性导出，漏了就会被 libva 报 "has no function __vaDriverInit_*"）
check: $(BUILD)/$(TARGET)
	@echo "== 导出的 driver 入口 =="
	@nm -D --defined-only $(BUILD)/$(TARGET) | grep vaDriverInit || \
		{ echo "错误：未导出 __vaDriverInit_* 符号"; exit 1; }

# 重新生成 vtable 桩与装配代码（改动 libva 版本后才需要）
gen:
	python3 tools/gen_vtable.py /usr/include/va/va_backend.h > $(BUILD)/vtable.json
	cd $(BUILD) && mkdir -p out && python3 ../tools/gen_stubs.py vtable.json
	cp $(BUILD)/out/stubs.c $(BUILD)/out/stubs.h $(BUILD)/out/vtable.inc $(SRCDIR)/

install: $(BUILD)/$(TARGET)
	install -d $(DESTDIR)$(DRIDIR)
	install -m 0644 $(BUILD)/$(TARGET) $(DESTDIR)$(DRIDIR)/$(TARGET)
	@echo "已安装到 $(DESTDIR)$(DRIDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(DRIDIR)/$(TARGET)
	@echo "已移除 $(DESTDIR)$(DRIDIR)/$(TARGET)"

clean:
	rm -rf $(BUILD)
