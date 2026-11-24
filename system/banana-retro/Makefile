# libretro.h is the unmodified MIT header from RetroArch v1.22.2.
# https://github.com/libretro/RetroArch/blob/v1.22.2/libretro-common/include/libretro.h
# SHA-256: bd3398d29c3763d18617087020ff56b5450a48a63123403a4b674f8e79947acb

CC ?= cc
CFLAGS ?= -O2
OUT ?= .

strict_cflags = -std=c99 -Wall -Wextra -Werror -pedantic

.PHONY: all check check-binaries
all: $(OUT)/banana-retro

$(OUT)/banana-retro: src/main.c src/core.c src/core.h include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) $(LDFLAGS) -o $@ src/main.c src/core.c -ldl $(LDLIBS)

$(OUT)/fixture.so: tests/fixture_core.c include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -shared -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/fixture-wrong-api.so: tests/fixture_core.c include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -DFIXTURE_WRONG_API -shared -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/fixture-missing-export.so: tests/fixture_core.c include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -DFIXTURE_MISSING_EXPORT -shared -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

check-binaries: $(OUT)/banana-retro $(OUT)/fixture.so $(OUT)/fixture-wrong-api.so $(OUT)/fixture-missing-export.so

check:
	@set -eu; \
	work=$$(mktemp -d "$${TMPDIR:-/tmp}/banana-retro-check.XXXXXX"); \
	trap 'rm -rf -- "$$work"' 0; \
	trap 'exit 1' HUP INT TERM; \
	$(MAKE) -s OUT="$$work" check-binaries; \
	sh tests/check-core.sh "$$work"
