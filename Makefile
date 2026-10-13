CC      = cc
AR      = ar
WARN    = -std=c11 -Wall -Wextra -Wpedantic
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

.PHONY: all shell test bench clean

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

# Test suites, built with AddressSanitizer and UndefinedBehaviorSanitizer
TESTS = test_perkdb test_injection test_rw
SAN   = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

test: $(TESTS:%=$(BUILD)/%)
	./$(BUILD)/test_perkdb
	./$(BUILD)/test_injection 5000
	./$(BUILD)/test_rw 2000 $(BUILD)/test_rw.db

$(BUILD)/test_%: tests/test_%.c $(SRCS) perkdb.h perk_internal.h $(SQLITE_OBJ) | $(BUILD)
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
