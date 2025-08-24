# ===== Scope-Acquire Makefile =====
# Usage:
#   make                                # builds default example
#   make acquire_func=example_acquire.c # choose acquisition file
#   make run                            # run the built binary
# Result: build/<basename>

TOP        := $(abspath .)
BUILD_DIR  := build

# ---- Core sources (adjust if your tree differs) ----
CORE_SRCS  := \
    engine/engine.c \
    engine/utils.c \
    scope/scope.c \
    scope/rigol/ds1000ze.c

# ---- Main + per-acquire file ----
MAIN_SRC      := main.c
acquire_func ?= example_acquire.c

# Object lists (map %.c -> build/%.o correctly)
CORE_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
MAIN_OBJ  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(MAIN_SRC))
ACQ_OBJ   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(acquire_func))
OBJS      := $(CORE_OBJS) $(MAIN_OBJ) $(ACQ_OBJ)
DEPS      := $(OBJS:.o=.d)

# Output name
ACQ_BASE := $(notdir $(basename $(acquire_func)))
TARGET   := $(BUILD_DIR)/$(ACQ_BASE)

# ---- Toolchain & flags ----
CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP \
           -I. -Iengine -Iscope -Iscope/rigol
LDFLAGS :=
LDLIBS  :=

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CFLAGS  += -I/Library/Frameworks/VISA.framework/Headers
    LDFLAGS += -F/Library/Frameworks/ -framework VISA
    LDLIBS  += -lpthread
else
    LDLIBS  += -lpthread -lvisa
endif

# ---- Default goal ----
.PHONY: all
all: $(TARGET)

# ---- Link final executable ----
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJS) $(LDLIBS) $(LDFLAGS) -o $@
	@echo "Linked $@"

# ---- Compile .c -> build/*.o ----
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Convenience ----
.PHONY: run clean print
run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

print:
	@echo "CORE_SRCS: $(CORE_SRCS)"
	@echo "OBJS:      $(OBJS)"
	@echo "TARGET:    $(TARGET)"

# ---- Auto-deps ----
-include $(DEPS)
