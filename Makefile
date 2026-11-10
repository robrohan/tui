GMAKE = gmake

.SUFFIXES:
CC = cc

C_ERRS = -Wall -Wextra -Wpedantic \
	-Wformat=2 -Wno-unused-parameter -Wshadow \
	-Wwrite-strings -Wstrict-prototypes -Wold-style-definition \
	-Wredundant-decls -Wnested-externs -Wmissing-include-dirs \
	-Wno-unused

help:
	@echo "BSD make builds these directly:"
	@echo "  tui   - build the plain windowing app, no editor (bin/tui_app)"
	@echo "  clean - remove build artifacts"
	@echo "  fmt   - format code"
	@echo "Any other target (test, test_clang, ...) needs gmake: pkg install gmake"

tui:
	mkdir -p bin
	$(CC) -std=c11 $(C_ERRS) -O2 \
		-I./vendor/r2 \
		-I./vendor/r2/tui \
		src/vt.c src/pty.c src/main.c \
		-o bin/tui

clean:
	rm -rf bin

fmt:
	clang-format -i ./src/*.[ch]

.DEFAULT:
	@$(GMAKE) CC='$(CC)' $@
