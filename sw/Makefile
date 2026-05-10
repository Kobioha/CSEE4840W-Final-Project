# Makefile for No Man's Land userspace game binary.
#
# Targets:
#   make            -- cross-compile for the DE1-SoC ARM HPS (default).
#                      Links the real joydev input reader (input_real.c).
#                      Output: nml_game (ELF, armhf).
#   make native     -- compile for the host (laptop / lab machine) with the
#                      real driver + joydev reader. Runs only on Linux with
#                      /dev/mem accessible; useful from the board's own gcc.
#   make fake-input -- cross-compile for ARM but with input_fake.c instead
#                      of the joydev reader. Useful to smoke-test the board
#                      without a controller plugged in.
#                      Output: nml_game_fake.
#   make terminal   -- host build with the terminal renderer (no FPGA needed)
#                      and input_fake.c (no /dev/input/js0 on the host).
#                      Output: nml_game_term.
#   make clean      -- remove build artefacts.
#
# Cross-compile prerequisite: gcc-arm-linux-gnueabihf (Debian/Ubuntu pkg name).
# The Quartus SoC EDS toolchain ships an equivalent compiler if you prefer.

CC_HOST    ?= gcc
CC_ARM     ?= arm-linux-gnueabihf-gcc

CFLAGS_COMMON := -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS       :=
TARGET_FPGA   := nml_game
TARGET_FAKE   := nml_game_fake
TARGET_TERM   := nml_game_term

SRC_GAME      := game.c main.c
SRC_INPUT     := input_real.c
SRC_INPUT_FAKE:= input_fake.c
SRC_RENDER    := render.c nml_gpu.c
SRC_TERMINAL  := render_terminal.c

ALL_FPGA      := $(SRC_GAME) $(SRC_INPUT)      $(SRC_RENDER)
ALL_FAKE      := $(SRC_GAME) $(SRC_INPUT_FAKE) $(SRC_RENDER)
ALL_TERMINAL  := $(SRC_GAME) $(SRC_INPUT_FAKE) $(SRC_TERMINAL)

# ---- default: cross-compile for ARM (DE1-SoC HPS) with joydev input ----
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

# ---- fake-input: cross-compile with input_fake.c (no controller needed) -
.PHONY: fake-input
fake-input: $(TARGET_FAKE)

$(TARGET_FAKE): $(ALL_FAKE)
	$(CC_ARM) $(CFLAGS_COMMON) -static -o $@ $(ALL_FAKE) $(LDFLAGS)
	@echo "Built $@ for armhf with input_fake.c. Copy to the board with scp."

# ---- terminal: host build, no FPGA needed -------------------------------
.PHONY: terminal
terminal: $(TARGET_TERM)

$(TARGET_TERM): $(ALL_TERMINAL)
	$(CC_HOST) $(CFLAGS_COMMON) -DNML_TERMINAL_BUILD -o $@ $(ALL_TERMINAL) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(TARGET_FPGA) $(TARGET_FAKE) $(TARGET_TERM) *.o
