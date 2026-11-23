# Vektor - GDI vector screensaver.
# Build from MSYS2 UCRT64 (gcc targeting x86_64-w64-mingw32).
#
#   make            vektor.scr (release)
#   make debug      vektor-debug.exe: adds /fakemonitors, /scene, /seed, /shot and /bench
#   make install    copy vektor.scr to System32 (needs an elevated shell)
#   make clean

CC       = gcc
WINDRES  = windres

TARGET   = vektor.scr
DTARGET  = vektor-debug.exe
BUILD    = build
BUILDTMP = $(BUILD)/tmp
RES      = $(BUILD)/vektor.res

# Keep compiler/linker temp files inside the project; do not depend on TMP/TEMP.
export TMP    := $(abspath $(BUILDTMP))
export TEMP   := $(TMP)
export TMPDIR := $(TMP)

CPPFLAGS = -Isrc -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
CFLAGS   = -std=c11 -Wall -Wextra -O2 -ffast-math -MMD -MP -municode
# -static: without it the .scr runs from here and fails to start from System32.
LDFLAGS  = -mwindows -municode -static
LDLIBS   = -lgdi32 -luser32 -lshell32

SRCS     := $(sort $(wildcard src/*.c))
OBJS     := $(SRCS:src/%.c=$(BUILD)/rel/%.o)
DOBJS    := $(SRCS:src/%.c=$(BUILD)/dbg/%.o)

.PHONY: all debug clean install
.SUFFIXES:

all: $(TARGET)
debug: $(DTARGET)

$(BUILD)/rel/%.o: src/%.c Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/dbg/%.o: src/%.c Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DVEKTOR_DEBUG $(CFLAGS) -c -o $@ $<

$(RES): vektor.rc vektor.manifest | $(BUILDTMP)
	$(WINDRES) vektor.rc -O coff -o $@

$(TARGET): $(OBJS) $(RES)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(RES) $(LDFLAGS) $(LDLIBS)

$(DTARGET): $(DOBJS) $(RES)
	$(CC) $(CFLAGS) -o $@ $(DOBJS) $(RES) $(LDFLAGS) $(LDLIBS)

$(BUILDTMP):
	@mkdir -p $@

clean:
	rm -rf $(TARGET) $(DTARGET) $(BUILD)

install: $(TARGET)
	cp $(TARGET) "$(SYSTEMROOT)/System32/"

-include $(OBJS:.o=.d) $(DOBJS:.o=.d)
