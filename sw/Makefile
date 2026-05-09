# Makefile for No Man's Land userspace game binary.
#
# Targets:
#   make            -- cross-compile for the DE1-SoC ARM HPS (default).
#                      Output: nml_game (ELF, armhf).
#   make native     -- compile for the host (laptop / lab machine).
#                      Output: nml_game (host arch). Linked against the real
#                      driver; will fail at runtime if /dev/mem mapping is
#                      blocked. Useful only on the board's own gcc.
#   make terminal   -- host build with the terminal renderer (no FPGA needed).
#                      Output: nml_game_term. Logs sprite slots to stdout.
#   make clean      -- remove build artefacts.
#
# Cross-compile prerequisite: gcc-arm-linux-gnueabihf (Debian/Ubuntu pkg name).
# The Quartus SoC EDS toolchain ships an equivalent compiler if you prefer.

CC_HOST    ?= gcc
CC_ARM     ?= arm-linux-gnueabihf-gcc

CFLAGS_COMMON := -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS       :=
TARGET_FPGA   := nml_game
TARGET_TERM   := nml_game_term

SRC_GAME      := game.c main.c
SRC_INPUT     := input_fake.c
SRC_RENDER    := render.c nml_gpu.c
SRC_TERMINAL  := render_terminal.c

ALL_FPGA      := $(SRC_GAME) $(SRC_INPUT) $(SRC_RENDER)
ALL_TERMINAL  := $(SRC_GAME) $(SRC_INPUT) $(SRC_TERMINAL)

# ---- default: cross-compile for ARM (DE1-SoC HPS) -----------------------
.PHONY: all
all: $(TARGET_FPGA)

$(TARGET_FPGA): $(ALL_FPGA)
	$(CC_ARM) $(CFLAGS_COMMON) -static -o $@ $(ALL_FPGA) $(LDFLAGS)
	@echo "Built $@ for armhf (DE1-SoC HPS). Copy to the board with scp."

# ---- native: compile on the board itself --------------------------------
.PHONY: native
native:
	$(CC_HOST) $(CFLAGS_COMMON) -o $(TARGET_FPGA) $(ALL_FPGA) $(LDFLAGS)
	@echo "Built $(TARGET_FPGA) for native arch. Useful only on the board."

# ---- terminal: host build, no FPGA needed -------------------------------
.PHONY: terminal
terminal: $(TARGET_TERM)

$(TARGET_TERM): $(ALL_TERMINAL)
	$(CC_HOST) $(CFLAGS_COMMON) -DNML_TERMINAL_BUILD -o $@ $(ALL_TERMINAL) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(TARGET_FPGA) $(TARGET_TERM) *.o
