# ===== minimal split build: core_build + per-acquire build_<name> =====
# Usage:
#   make acquire=/abs/or/rel/path/to/rpi_acquire.c
#   make clean                 # cleans core only
#   make clean acquire=...     # cleans only build_<name> for that acquire

# ---- toolchain ----
CC      := cc
AR      := ar
ARFLAGS := rcs

# ---- flags (shared by core & acquire) ----
CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror
CFLAGS   += -MMD -MP
CPPFLAGS ?= -I. -Iengine -Iscope -Iscope/rigol
LDFLAGS  ?=
LDLIBS   ?= -lpthread -lm

# ---- NI-VISA (platform-specific) ----
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  CPPFLAGS += -I/Library/Frameworks/VISA.framework/Headers
  LDFLAGS  += -F/Library/Frameworks -framework VISA
else
  LDLIBS   += -lvisa
endif

# ---- sources ----
# main is shared; compile once and link explicitly (don't hide it in the archive)
MAIN_SRC   := main.c

CORE_SRCS := \
  engine/engine.c \
  engine/utils.c  \
  scope/scope.c   \
  scope/rigol/ds1000ze.c

# ---- derived ----
TOP         := $(abspath .)
CORE_BUILD  := $(TOP)/core_build
CORE_LIB    := $(CORE_BUILD)/core.a

# main.o compiled to core_build as well (reused by all acquires)
MAIN_OBJ    := $(CORE_BUILD)/$(MAIN_SRC:.c=.o)
MAIN_DEP    := $(MAIN_OBJ:.o=.d)

CORE_OBJS   := $(patsubst %.c,$(CORE_BUILD)/%.o,$(CORE_SRCS))
CORE_DEPS   := $(CORE_OBJS:.o=.d)

# --- Only demand 'acquire=' for build goals, not for clean/help ---
ifeq ($(filter clean help,$(MAKECMDGOALS)),)
  ifeq ($(strip $(acquire)),)
    $(error Please invoke as 'make acquire=path/to/<file>.c' (try 'make help'))
  endif
endif


ACQ_PATH  := $(abspath $(acquire))
ACQ_NAME  := $(notdir $(basename $(ACQ_PATH)))
ACQ_BUILD := $(TOP)/build_$(ACQ_NAME)
ACQ_OBJ   := $(ACQ_BUILD)/$(ACQ_NAME).o
ACQ_DEP   := $(ACQ_OBJ:.o=.d)
ACQ_EXE   := $(ACQ_BUILD)/$(ACQ_NAME)

# ---- default: build core (incl. main.o) if needed, then this acquire ----
all: $(ACQ_EXE)

# ---- core static library (engine/scope/etc.) ----
$(CORE_LIB): $(CORE_OBJS)
	@mkdir -p "$(dir $@)"
	$(AR) $(ARFLAGS) "$@" $^

# Compile core .c -> core_build/<same path>.o
$(CORE_BUILD)/%.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

# Build shared main.o (explicit so it's not archived)
$(MAIN_OBJ): $(MAIN_SRC)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

# ---- acquire: object + link ----
$(ACQ_OBJ): $(ACQ_PATH) | $(ACQ_BUILD)/
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(ACQ_BUILD)/:
	@mkdir -p "$@"

$(ACQ_EXE): $(ACQ_OBJ) $(MAIN_OBJ) $(CORE_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(ACQ_OBJ) $(MAIN_OBJ) $(CORE_LIB) $(LDLIBS)

# ---- clean (scoped) ----
.PHONY: clean
clean:
ifeq ($(strip $(acquire)),)
	@echo "Cleaning core only: $(CORE_BUILD)"
	rm -rf "$(CORE_BUILD)"
else
	@echo "Cleaning acquire only: $(ACQ_BUILD)"
	rm -rf "$(ACQ_BUILD)"
endif

# ---- help ----
.PHONY: help
help:
	@echo "# Usage:"
	@echo "#   make acquire=/abs/or/rel/path/to/rpi_acquire.c"
	@echo "#   make clean                 # cleans core only"
	@echo "#   make clean acquire=...     # cleans only build_<name> for that acquire"

# ---- auto-deps ----
-include $(CORE_DEPS) $(MAIN_DEP) $(ACQ_DEP)
