# Optional loader-scoped compatibility library. Never installs over system GBM.
CC ?= cc
CFLAGS ?= -O2 -g
REAL_GBM ?= /usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0
BUILD := build
GBM_CFLAGS := $(shell pkg-config --cflags gbm 2>/dev/null)
COMMON_FLAGS := -std=c11 -fPIC -fvisibility=hidden -Wall -Wextra -Werror $(GBM_CFLAGS)
GBM_CPPFLAGS := -DDMD_GBM_REAL_LIBRARY_PATH='"$(REAL_GBM)"'

.PHONY: all clean check probe
all: $(BUILD)/libgbm.so.1

$(BUILD):
	mkdir -p $@

$(BUILD)/libgbm.so.1: gbm_compat.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(GBM_CPPFLAGS) $(CFLAGS) $(COMMON_FLAGS) -shared -Wl,-soname,libgbm.so.1 \
		-Wl,-z,defs -Wl,-z,relro,-z,now $< -o $@ -ldl -lpthread

$(BUILD)/libgbm-fake.so: tests/fake_gbm.c tests/fake_gbm.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON_FLAGS) -fvisibility=default -shared $< -o $@

$(BUILD)/test_gbm_compat: tests/test_gbm_compat.c tests/fake_gbm.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(GBM_CFLAGS) $< -ldl -lpthread -o $@

probe: $(BUILD)/probe_gbm_compat

$(BUILD)/probe_gbm_compat: probe_gbm_compat.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(GBM_CFLAGS) $< -ldl -o $@

check: $(BUILD)/libgbm.so.1 $(BUILD)/libgbm-fake.so $(BUILD)/test_gbm_compat $(BUILD)/probe_gbm_compat
	$(BUILD)/test_gbm_compat "$(abspath $(BUILD)/libgbm.so.1)" \
		"$(abspath $(BUILD)/libgbm-fake.so)"
	DMD_GBM_REAL_LIBRARY="$(abspath $(BUILD)/libgbm-fake.so)" \
		$(BUILD)/probe_gbm_compat /dev/null "$(abspath $(BUILD)/libgbm.so.1)" 10 6

clean:
	rm -rf $(BUILD)
