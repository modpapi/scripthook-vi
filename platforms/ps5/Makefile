PYTHON ?= python3
# Host C compiler for the unit tests. A user-set CC (command line or env) wins;
# otherwise prefer cc, then clang-18/gcc (the WSL toolchain ships clang-18 but
# no `cc` symlink, so make's built-in `cc` default would fail).
ifeq ($(origin CC),default)
CC := $(shell command -v cc 2>/dev/null || command -v clang-18 2>/dev/null || command -v gcc 2>/dev/null || echo cc)
endif

.PHONY: test pytest ctest payload

test: pytest ctest

pytest:
	$(PYTHON) -m unittest discover -s tests -v

# Host-side C unit tests for the injector's pure logic (no PS5 deps).
ctest: build/test_proc
	./build/test_proc

build/test_proc: tests/test_proc.c payload/proc_select.c payload/proc.h
	mkdir -p build
	$(CC) -Wall -Wextra -O2 -o $@ tests/test_proc.c payload/proc_select.c

payload:
	$(MAKE) -C payload
