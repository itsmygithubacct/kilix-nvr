PROJECT := kilix-nvr
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Isrc
# Header dependencies, generated as a side effect of compiling.  Without
# them a changed header leaves stale objects linked against the values it
# used to have, and the result is a test that fails for a reason nothing
# in the source explains.  That has cost real time here more than once.
DEPFLAGS := -MMD -MP
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS)
LDLIBS += -lsqlite3 -lpthread -lm

# Vendored and pinned.  The terminal stack is reached through kilix-rtsp's
# own closure rather than pinned again here: both it and kilix-mask already
# pin the same session, framebuffer, input and soft-raster commits, and a
# second pin of the same module is a second thing to keep in step.
RTSP := third_party/kilix-rtsp
MOTION := third_party/kilix-motion-detect
MASK := third_party/kilix-mask
KSD := third_party/kilix-sound-detect
KOD := third_party/kilix-object-detect
KTS := $(RTSP)/third_party/kitty-terminal-session
KFB := $(KTS)/third_party/kitty-framebuffer
KIN := $(KTS)/third_party/kitty-input
KKB := $(KIN)/third_party/kitty_keyboard
SR := $(RTSP)/third_party/soft-raster

MODULE_CPPFLAGS := \
	-I$(RTSP)/include -I$(MOTION)/include -I$(MASK)/include \
	-I$(KSD)/include -I$(KOD)/include \
	-I$(KTS)/include -I$(KFB)/include -I$(KIN)/include -I$(KKB)/include \
	-I$(SR)/include

# Pinned upstream code, built with the conversion warnings off so their
# output cannot bury ours.
VENDOR_SOURCES := \
	$(RTSP)/src/krtsp_args.c \
	$(RTSP)/src/krtsp_frame.c \
	$(RTSP)/src/krtsp_source.c \
	$(RTSP)/src/krtsp_paths.c \
	$(RTSP)/src/krtsp_config.c \
	$(RTSP)/src/krtsp_exec.c \
	$(MOTION)/src/kilix_motion_detect.c \
	$(MASK)/src/kilix_mask.c \
	$(KSD)/src/kilix_sound_detect.c \
	$(KOD)/src/kilix_object_detect.c \
	$(KTS)/src/kitty_terminal_session.c \
	$(KFB)/src/kitty_framebuffer.c \
	$(KIN)/src/kitty_input.c \
	$(KIN)/src/kitty_input_posix.c \
	$(KKB)/src/kitty_keyboard.c \
	$(KKB)/src/kitty_keyboard_posix.c \
	$(SR)/src/soft_raster.c
VENDOR_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/vendor/%.o,$(notdir $(VENDOR_SOURCES)))
VENDOR_CFLAGS := $(CFLAGS) -Wno-conversion -Wno-sign-conversion
VENDOR_LDLIBS := -lz

OBJECTS := \
	$(BUILD_DIR)/knvr_paths.o \
	$(BUILD_DIR)/knvr_clip.o \
	$(BUILD_DIR)/knvr_command.o \
	$(BUILD_DIR)/knvr_config.o \
	$(BUILD_DIR)/knvr_review.o \
	$(BUILD_DIR)/knvr_run.o \
	$(BUILD_DIR)/knvr_service.o \
	$(BUILD_DIR)/knvr_sqlite.o \
	$(BUILD_DIR)/knvr_store.o \
	$(BUILD_DIR)/knvr_strip.o \
	$(BUILD_DIR)/knvr_track.o \
	$(BUILD_DIR)/knvr_view.o \
	$(BUILD_DIR)/knvr_watch.o \
	$(BUILD_DIR)/knvr_zone.o

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
COMMAND := $(BUILD_DIR)/$(PROJECT)

TESTS := $(BUILD_DIR)/test-clip $(BUILD_DIR)/test-config \
	$(BUILD_DIR)/test-store \
	$(BUILD_DIR)/test-run $(BUILD_DIR)/test-track \
	$(BUILD_DIR)/test-watch \
	$(BUILD_DIR)/test-zone

.DEFAULT_GOAL := all
.PHONY: all test sanitize install clean

all: $(COMMAND)

$(BUILD_DIR) $(BUILD_DIR)/vendor:
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MODULE_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	$(AR) rcs $@ $^

# vpath lets one rule cover vendored sources from several trees.
vpath %.c $(sort $(dir $(VENDOR_SOURCES)))

$(BUILD_DIR)/vendor/%.o: %.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(MODULE_CPPFLAGS) $(VENDOR_CFLAGS) -c $< -o $@

$(COMMAND): src/main.c $(STATIC_LIB) $(VENDOR_OBJECTS) | $(BUILD_DIR)
	@test -f $(RTSP)/include/kilix_rtsp.h || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(CPPFLAGS) $(MODULE_CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
		src/main.c $(STATIC_LIB) $(VENDOR_OBJECTS) \
		$(LDLIBS) $(VENDOR_LDLIBS) -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) $(VENDOR_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MODULE_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< \
		$(STATIC_LIB) $(VENDOR_OBJECTS) $(LDLIBS) $(VENDOR_LDLIBS) -o $@

# The command's own --selftest runs here too: it is the only check available
# on a machine that has the program installed but not this source tree.
test: $(TESTS) $(COMMAND)
	@set -e; for binary in $(TESTS); do \
		printf '\n== %s ==\n' "$$binary"; \
		"$$binary"; \
	done; \
	printf '\n== %s --selftest ==\n' "$(COMMAND)"; \
	$(COMMAND) --selftest; \
	printf '\nall test suites passed\n'

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean
	@$(MAKE) --no-print-directory CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" test

# The detector goes with the binary: knvr_detect spawns it by name, so a
# command installed without it is a recorder that silently never detects.
# The sound side's tools belong to kilix-sound-detect and are installed by
# it, because two copies of a classifier is two things to keep in step.
TOOLS :=
# The detector and the classifier belong to the modules that own them, and
# are installed by them: two copies of a detector is two things to keep in
# step.
KSD_TOOLS := $(KSD)/tools/kilix-listen-classify \
	$(KSD)/tools/kilix-sound-fetch-model \
	$(KOD)/tools/kilix-look-detect
KSD_TOOLS := $(KSD)/tools/kilix-listen-classify \
	$(KSD)/tools/kilix-sound-fetch-model

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 755 $(COMMAND) $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 755 $(KSD_TOOLS) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
