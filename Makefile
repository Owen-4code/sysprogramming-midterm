CC ?= cc
DEBUG ?= 1
BUILD := build/debug$(DEBUG)
CPPFLAGS += -Iinclude -D_GNU_SOURCE -DSYSPROG_DEBUG=$(DEBUG)
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic
LINK_COMMON := -L$(BUILD) -lsysprogram -Wl,-rpath,'$$ORIGIN'

.PHONY: all test
all: $(BUILD)/server $(BUILD)/client

$(BUILD):
	mkdir -p $@

$(BUILD)/libsysprogram.so: src/common.c include/common.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -fPIC -shared -o $@ $< $(LDLIBS)

$(BUILD)/server: src/server.c include/common.h $(BUILD)/libsysprogram.so
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LINK_COMMON) $(LDLIBS)

$(BUILD)/client: src/client.c include/common.h $(BUILD)/libsysprogram.so
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LINK_COMMON) $(LDLIBS)

$(BUILD)/test_suite: tests/test_suite.c include/common.h $(BUILD)/libsysprogram.so
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LINK_COMMON) $(LDLIBS)

test: all $(BUILD)/test_suite
	timeout 30s $(BUILD)/test_suite $(BUILD)/server $(BUILD)/client
