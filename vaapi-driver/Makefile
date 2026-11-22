# DroidSpaces V4L2 VA-API driver (decode and encode)
#
# libva derives msm_drm from the render node and looks only for
# msm_drm_drv_video.so in its driver directory. Preserve this filename.
# Build inside the AArch64 container when the host toolchain lacks an AArch64 sysroot.

TARGET  := msm_drm_drv_video.so
SRCDIR  := src
BUILD   := build

# The libva loader opens this module and provides its VA-API entry points.
# Use the headers without linking libva into the driver.
VA_CFLAGS := $(shell pkg-config --cflags libva 2>/dev/null)

# Use libdrm headers for dumb-buffer allocation and DMA-BUF export.
# The driver sends ioctls directly and does not link libdrm.
DRM_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null)

CC      ?= cc
CFLAGS  ?= -O2 -g
# Track header dependencies so changing driver.h rebuilds affected objects.
# Embed the abbreviated Git hash and dirty marker in the driver version.
GITDESC := $(shell cd $(dir $(lastword $(MAKEFILE_LIST))) && \
                   git rev-parse --short=8 HEAD 2>/dev/null || echo "nogit")
GITDIRTY := $(shell cd $(dir $(lastword $(MAKEFILE_LIST))) && \
                    git diff --quiet 2>/dev/null || echo "-dirty")
BUILD_ID := $(GITDESC)$(GITDIRTY)

CFLAGS  += -std=c11 -fPIC -Wall -Wextra -Wno-unused-parameter \
           -fvisibility=hidden -D_GNU_SOURCE $(VA_CFLAGS) $(DRM_CFLAGS) \
           -I$(SRCDIR) -MMD -MP -DDMD_BUILD_ID=\"$(BUILD_ID)\"

# Enable the experimental AV1 profile with AV1=1. The default build hides it.
# Use this option instead of overriding CFLAGS to preserve the shared-library
# options, including -fPIC. CPPFLAGS is not used by these compilation rules.
ifeq ($(AV1),1)
CFLAGS  += -DDMD_ENABLE_AV1
endif
# HEVC10=1 enables only the experimental Main10/P010 decoder. Keep the
# default capability set unchanged while wider platform/app validation continues.
# It neither enables VP9 Profile2 nor adds 10-bit encoding support.
ifeq ($(HEVC10),1)
CFLAGS  += -DDMD_ENABLE_HEVC_MAIN10
endif
LDFLAGS += -shared -Wl,--no-undefined -Wl,-z,defs
LDLIBS  += -lpthread

SESSION_SRCS := $(SRCDIR)/dmd_v4l2_session.c $(SRCDIR)/dmd_v4l2_session_state.c \
                $(SRCDIR)/dmd_v4l2_receive.c

SOURCES := $(SRCDIR)/bitstream.c $(SRCDIR)/driver.c $(SRCDIR)/profiles.c $(SRCDIR)/stubs.c \
           $(SESSION_SRCS) \
          $(SRCDIR)/v4l2_backend.c $(SRCDIR)/decode.c $(SRCDIR)/image.c \
           $(SRCDIR)/h264_bitstream.c $(SRCDIR)/hevc_bitstream.c $(SRCDIR)/hevc_slice.c $(SRCDIR)/export.c \
           $(SRCDIR)/av1_bitstream.c $(SRCDIR)/mpeg2_bitstream.c \
           $(SRCDIR)/encode.c $(SRCDIR)/v4l2_encoder.c $(SRCDIR)/video_stats.c $(SRCDIR)/vpp.c
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

# These unit tests run without hardware or a daemon. The old socket client
# and shared-memory fallback tests were removed with the daemon architecture.
TESTDIR   := tests
TESTBIN   := $(BUILD)/test_av1_bitstream
TEST_SRCS := $(TESTDIR)/test_av1_bitstream.c $(SRCDIR)/av1_bitstream.c \
          $(SRCDIR)/bitstream.c
ENCODE_TESTBIN := $(BUILD)/test_encode_api
ENCODE_HEVC10_TESTBIN := $(BUILD)/test_encode_api_hevc10
ENCODE_FRONT_TESTBIN := $(BUILD)/test_encode_frontend
ENCODE_BACK_TESTBIN := $(BUILD)/test_v4l2_encoder
STATS_TESTBIN := $(BUILD)/test_video_stats
HEVC_TESTBIN := $(BUILD)/test_hevc_slice
IMAGE_INTEROP_TESTBIN := $(BUILD)/test_image_interop
PROFILE_TESTBINS := $(BUILD)/test_profiles_default $(BUILD)/test_profiles_hevc10 $(BUILD)/test_profiles_hevc10_av1
CAPTURE_TESTBIN := $(BUILD)/test_v4l2_capture
RECEIVE_TESTBIN := $(BUILD)/test_v4l2_receive
RECEIVE_TEST_SRCS := $(TESTDIR)/test_v4l2_receive.c \
                    $(SRCDIR)/dmd_v4l2_receive.c $(SRCDIR)/dmd_v4l2_session_state.c \
                    $(SRCDIR)/video_stats.c

# Install in the driver directory used by libva in the container.
PREFIX  ?= /usr
LIBDIR  ?= $(PREFIX)/lib/aarch64-linux-gnu
DRIDIR  ?= $(LIBDIR)/dri
DESTDIR ?=

.PHONY: all clean install uninstall check gen tests probe-va-encode probe-p010-egl probe-hevc-lag firefox-compat install-firefox-compat

all: $(BUILD)/$(TARGET)

firefox-compat:
	$(MAKE) -C firefox-compat compat

install-firefox-compat: firefox-compat
	install -d $(DESTDIR)/usr/local/lib/droidspaces-firefox
	install -m 0644 firefox-compat/build/libdmd-firefox-compat.so $(DESTDIR)/usr/local/lib/droidspaces-firefox/libdmd-firefox-compat.so

probe-va-encode: $(BUILD)/probe_va_encode

# Optional graphics probe only: all/tests do not require EGL/GLES development
# files or link these libraries. Override for a header bundle/runtime-only host.
P010_EGL_CFLAGS ?= $(shell pkg-config --cflags egl glesv2 2>/dev/null)
P010_EGL_LIBS ?= $(shell pkg-config --libs egl glesv2 2>/dev/null || echo '-lEGL -lGLESv2')

probe-p010-egl: $(BUILD)/probe_p010_egl

$(BUILD)/probe_p010_egl: tools/probe_p010_egl.c | $(BUILD)
	$(CC) $(TESTCFLAGS) $(DRM_CFLAGS) $(P010_EGL_CFLAGS) $< $(P010_EGL_LIBS) -lm -o $@

# Explicit hardware diagnostic. Neither all nor tests runs/depends on this tool.
probe-hevc-lag: $(BUILD)/probe_hevc_lag

$(BUILD)/probe_hevc_lag: tools/probe_hevc_lag.c $(SESSION_SRCS) $(SRCDIR)/v4l2_backend.c $(SRCDIR)/video_stats.c $(SRCDIR)/dmd_client.h $(SRCDIR)/v4l2_backend.h $(SRCDIR)/sandbox_heap.h $(SRCDIR)/video_stats.h | $(BUILD)
	$(CC) $(TESTCFLAGS) -I$(SRCDIR) tools/probe_hevc_lag.c \
		$(SESSION_SRCS) $(SRCDIR)/v4l2_backend.c $(SRCDIR)/video_stats.c -lpthread -o $@

$(BUILD)/probe_va_encode: tools/probe_va_encode.c $(OBJECTS)
	$(CC) $(CFLAGS) $< $(OBJECTS) $(LDLIBS) -o $@

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

# libva must be able to find the visible entry point in the shared object.
check: $(BUILD)/$(TARGET)
	@echo "== Exported driver entry points =="
	@nm -D --defined-only $(BUILD)/$(TARGET) | grep vaDriverInit || \
		{ echo "Error: __vaDriverInit_* symbol not exported"; exit 1; }

# Build and run the unit tests using the driver's warning settings.
tests: $(TESTBIN) $(ENCODE_TESTBIN) $(ENCODE_HEVC10_TESTBIN) $(ENCODE_FRONT_TESTBIN) $(ENCODE_BACK_TESTBIN) $(STATS_TESTBIN) $(HEVC_TESTBIN) $(IMAGE_INTEROP_TESTBIN) $(PROFILE_TESTBINS) $(CAPTURE_TESTBIN) $(RECEIVE_TESTBIN)
	@echo "== Running unit tests =="
	@$(TESTBIN)
	@$(ENCODE_TESTBIN)
	@$(ENCODE_HEVC10_TESTBIN)
	@$(ENCODE_FRONT_TESTBIN)
	@$(ENCODE_BACK_TESTBIN)
	@$(STATS_TESTBIN)
	@$(HEVC_TESTBIN)
	@$(IMAGE_INTEROP_TESTBIN)
	@$(BUILD)/test_profiles_default
	@$(BUILD)/test_profiles_hevc10
	@$(BUILD)/test_profiles_hevc10_av1
	@$(CAPTURE_TESTBIN)
	@$(RECEIVE_TESTBIN)

TESTCFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE

# Pass the libva header path for installations outside the compiler's default search path.
$(TESTBIN): $(TEST_SRCS) | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) -I$(SRCDIR) $(TEST_SRCS) -lpthread -o $@

$(ENCODE_TESTBIN) $(ENCODE_HEVC10_TESTBIN): $(TESTDIR)/test_encode_api.c $(filter-out $(SRCDIR)/v4l2_encoder.c,$(SOURCES)) $(wildcard $(SRCDIR)/*.h) | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) $(DRM_CFLAGS) -I$(SRCDIR) \
		$(if $(filter $(ENCODE_HEVC10_TESTBIN),$@),-DDMD_ENABLE_HEVC_MAIN10) \
		$(TESTDIR)/test_encode_api.c $(filter-out $(SRCDIR)/v4l2_encoder.c,$(SOURCES)) -lpthread -o $@

$(ENCODE_FRONT_TESTBIN): $(TESTDIR)/test_encode_frontend.c $(SRCDIR)/encode.c $(SRCDIR)/video_stats.c $(wildcard $(SRCDIR)/*.h) | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) $(DRM_CFLAGS) -I$(SRCDIR) \
		$(TESTDIR)/test_encode_frontend.c $(SRCDIR)/encode.c $(SRCDIR)/video_stats.c -lpthread -o $@

$(STATS_TESTBIN): $(TESTDIR)/test_video_stats.c $(SRCDIR)/video_stats.c $(SRCDIR)/video_stats.h | $(BUILD)
	$(CC) $(TESTCFLAGS) -I$(SRCDIR) $(TESTDIR)/test_video_stats.c $(SRCDIR)/video_stats.c -lpthread -o $@

$(HEVC_TESTBIN): $(TESTDIR)/test_hevc_slice.c $(SRCDIR)/hevc_slice.c $(SRCDIR)/hevc_bitstream.c $(SRCDIR)/bitstream.c $(wildcard $(SRCDIR)/*.h) | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) $(DRM_CFLAGS) -I$(SRCDIR) \
		$(TESTDIR)/test_hevc_slice.c $(SRCDIR)/hevc_slice.c $(SRCDIR)/hevc_bitstream.c $(SRCDIR)/bitstream.c -lpthread -o $@

$(IMAGE_INTEROP_TESTBIN): $(TESTDIR)/test_image_interop.c $(SRCDIR)/image.c $(SRCDIR)/export.c $(wildcard $(SRCDIR)/*.h) | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) $(DRM_CFLAGS) -I$(SRCDIR) \
		$(TESTDIR)/test_image_interop.c $(SRCDIR)/image.c -lpthread -o $@

# Exercise independent capability gates without opening any device. Explicit
# defines make both the default-off and enabled contracts run in every build.
$(PROFILE_TESTBINS): $(TESTDIR)/test_profiles_hevc10.c $(SRCDIR)/profiles.c $(wildcard $(SRCDIR)/*.h) | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) $(DRM_CFLAGS) -I$(SRCDIR) \
		-UDMD_PROBE_10BIT_PROFILES -UDMD_ENABLE_HEVC_MAIN10 -UDMD_ENABLE_AV1 \
		$(if $(filter $(BUILD)/test_profiles_hevc10 $(BUILD)/test_profiles_hevc10_av1,$@),-DDMD_ENABLE_HEVC_MAIN10) \
		$(if $(filter $(BUILD)/test_profiles_hevc10_av1,$@),-DDMD_ENABLE_AV1) \
		$(TESTDIR)/test_profiles_hevc10.c $(SRCDIR)/profiles.c -lpthread -o $@

$(CAPTURE_TESTBIN): $(TESTDIR)/test_v4l2_capture.c $(SRCDIR)/v4l2_backend.c $(SRCDIR)/v4l2_backend.h $(SRCDIR)/sandbox_heap.h $(SRCDIR)/video_stats.h | $(BUILD)
	$(CC) $(TESTCFLAGS) -I$(SRCDIR) $(TESTDIR)/test_v4l2_capture.c -o $@

$(ENCODE_BACK_TESTBIN): $(TESTDIR)/test_v4l2_encoder.c $(SRCDIR)/v4l2_encoder.c $(SRCDIR)/v4l2_encoder.h | $(BUILD)
	$(CC) $(TESTCFLAGS) -I$(SRCDIR) $(TESTDIR)/test_v4l2_encoder.c -o $@

$(RECEIVE_TESTBIN): $(RECEIVE_TEST_SRCS) $(SRCDIR)/dmd_v4l2_session_private.h \
                    $(SRCDIR)/dmd_client.h $(SRCDIR)/v4l2_backend.h | $(BUILD)
	$(CC) $(TESTCFLAGS) $(VA_CFLAGS) -I$(SRCDIR) $(RECEIVE_TEST_SRCS) -lpthread -o $@

# Regenerate the stubs and vtable bindings after changing libva versions.
gen:
	python3 tools/gen_vtable.py /usr/include/va/va_backend.h > $(BUILD)/vtable.json
	cd $(BUILD) && mkdir -p out && python3 ../tools/gen_stubs.py vtable.json
	cp $(BUILD)/out/stubs.c $(BUILD)/out/stubs.h $(BUILD)/out/vtable.inc $(SRCDIR)/

install: $(BUILD)/$(TARGET)
	install -d $(DESTDIR)$(DRIDIR)
	install -m 0644 $(BUILD)/$(TARGET) $(DESTDIR)$(DRIDIR)/$(TARGET)
	@echo "Installed at $(DESTDIR)$(DRIDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(DRIDIR)/$(TARGET)
	@echo "Removed $(DESTDIR)$(DRIDIR)/$(TARGET)"

clean:
	rm -rf $(BUILD)

# Include generated header dependencies if they exist.
-include $(DEPS)
