CC      = cc
AR      = ar
WARN    = -std=c11 -Wall -Wextra -Wpedantic -Werror
CFLAGS  = $(WARN) -O2 -MMD -MP
# DQS=0 makes SQLite treat "..." strictly as identifiers, never string literals
SQLITE_CFLAGS = -O2 -DSQLITE_DQS=0

BUILD      = build
LIB        = $(BUILD)/libperkdb.a
SRCS       = perkdb.c perk_util.c perk_where.c perk_crud.c perk_result.c perk_tx.c
OBJS       = $(SRCS:%.c=$(BUILD)/%.o)
SQLITE_OBJ = $(BUILD)/sqlite3.o

LDLIBS = -lpthread -lm
ifeq ($(shell uname -s),Linux)
LDLIBS += -ldl
endif

.PHONY: all shell test test-addon bench clean

# Static library containing PerkDB and SQLite; link apps with: build/libperkdb.a -lpthread -lm
all: $(LIB)

$(LIB): $(OBJS) $(SQLITE_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SQLITE_OBJ): sqlite3-src/sqlite3.c | $(BUILD)
	$(CC) $(SQLITE_CFLAGS) -c $< -o $@

# The sqlite3 command-line shell, handy for inspecting database files
shell: $(BUILD)/sqlite3

$(BUILD)/sqlite3: sqlite3-src/shell.c $(SQLITE_OBJ)
	$(CC) $(SQLITE_CFLAGS) $^ -o $@ $(LDLIBS)

# Test suites: every tests/test_*.c is built with AddressSanitizer and
# UndefinedBehaviorSanitizer and run. All suites run even if one fails; the
# target exits non-zero if any of them failed.
TESTS = $(patsubst tests/%.c,%,$(wildcard tests/test_*.c))
SAN   = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

# Optional per-suite command-line arguments (suites without an entry get none)
ARGS_test_injection = 5000
ARGS_test_rw        = 2000 $(BUILD)/test_rw.db

test: $(TESTS:%=$(BUILD)/%)
	@failed=""; \
	$(foreach t,$(TESTS),echo "./$(BUILD)/$(t) $(ARGS_$(t))"; \
	    ./$(BUILD)/$(t) $(ARGS_$(t)) || failed="$$failed $(t)";) \
	if [ -n "$$failed" ]; then echo "FAILED test suites:$$failed"; exit 1; fi; \
	echo "All test suites passed: $(TESTS)"

# Node addon smoke test. Needs the addon built first (`npm run build`); it is
# deliberately not part of `make test`, which must not require node.
test-addon:
	@if [ ! -f $(BUILD)/Release/perkdb.node ]; then \
	    echo "test-addon: $(BUILD)/Release/perkdb.node is missing; run 'npm install' or 'npm run build' first" >&2; \
	    exit 1; \
	fi
	node tests/test_addon.js

$(BUILD)/test_%: tests/test_%.c tests/test_harness.h $(SRCS) perkdb.h perk_internal.h $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(WARN) $(SAN) -I. $< $(SRCS) $(SQLITE_OBJ) -o $@ $(LDLIBS)

# Optimized write/read benchmark (no sanitizers) on a file-backed database
BENCH_RECORDS = 100000

bench: $(BUILD)/bench_rw
	./$(BUILD)/bench_rw $(BENCH_RECORDS) $(BUILD)/bench_rw.db

$(BUILD)/bench_rw: tests/test_rw.c $(LIB)
	$(CC) $(WARN) -O2 -I. $< $(LIB) -o $@ $(LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
