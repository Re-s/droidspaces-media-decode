CC ?= cc
BUILD ?= build
CFLAGS ?= -O2 -g
COMMON = -std=c11 -Wall -Wextra -Werror
LIB = $(BUILD)/libdmd-firefox-heap.so

.PHONY: all compat check kernel-test passthrough-test clean
all: $(LIB)
compat: $(BUILD)/libdmd-firefox-compat.so

$(BUILD):
	mkdir -p $@

$(LIB): firefox-system-heap.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) -fPIC -fvisibility=hidden \
	 -shared -Wl,-z,defs -Wl,-z,relro,-z,now \
	 -Wl,-soname,libdmd-firefox-heap.so $(LDFLAGS) $< -o $@

$(BUILD)/libdmd-firefox-compat.so: firefox-system-heap.c firefox-kgsl.c kgsl-filter.c kgsl-filter.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) -fPIC -fvisibility=hidden \
	 -DDMD_FIREFOX_KGSL_BUILD -shared -Wl,-z,defs -Wl,-z,relro,-z,now \
	 -Wl,-soname,libdmd-firefox-compat.so $(LDFLAGS) \
	 firefox-system-heap.c firefox-kgsl.c kgsl-filter.c -ldl -o $@

$(BUILD)/test-kgsl-filter: test-kgsl-filter.c kgsl-filter.c kgsl-filter.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) $(LDFLAGS) test-kgsl-filter.c kgsl-filter.c -o $@

$(BUILD)/test_kernel: test_kernel.c kgsl-filter.c kgsl-filter.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) $(LDFLAGS) test_kernel.c kgsl-filter.c -o $@

kernel-test: $(BUILD)/test_kernel

$(BUILD)/test_passthrough: test_passthrough.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) $(LDFLAGS) $< -ldl -pthread -o $@

# Compile only. Run with the native AArch64 compat preload in parent/content
# roles on the device; host check must not execute this target.
passthrough-test: $(BUILD)/test_passthrough

$(BUILD)/test-system-heap: test-system-heap.c firefox-system-heap.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) -pthread $(LDFLAGS) $< -o $@

$(BUILD)/libdmd-firefox-heap-test.so: firefox-system-heap.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) -fPIC -fvisibility=hidden \
	 -DDMD_FIREFOX_HEAP_TEST_PATH='"/dev/null"' -shared \
	 $(LDFLAGS) $< -o $@

$(BUILD)/test-preload: test-preload.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) $(LDFLAGS) $< -ldl -o $@

check: all $(BUILD)/test-system-heap $(BUILD)/test-preload $(BUILD)/libdmd-firefox-heap-test.so $(BUILD)/test-kgsl-filter
	$(BUILD)/test-system-heap
	$(BUILD)/test-kgsl-filter
	LD_PRELOAD=$(abspath $(BUILD))/libdmd-firefox-heap-test.so $(BUILD)/test-preload rdd
	LD_PRELOAD=$(abspath $(BUILD))/libdmd-firefox-heap-test.so $(BUILD)/test-preload -contentproc tab
	LD_PRELOAD=$(abspath $(BUILD))/libdmd-firefox-heap-test.so $(BUILD)/test-preload -contentproc forkserver
	LD_PRELOAD=$(abspath $(BUILD))/libdmd-firefox-heap-test.so $(BUILD)/test-preload -contentproc rdd tab
	DMD_TEST_EXPECT_OPEN=1 LD_PRELOAD=$(abspath $(BUILD))/libdmd-firefox-heap-test.so $(BUILD)/test-preload -contentproc -sandboxReporter 8 rdd

clean:
	rm -rf $(BUILD)
