CC ?= gcc
AR ?= ar
CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -O2
CFLAGS += -Iinclude

SRCS = src/coma.c src/dua.c src/tapi.c src/voice.c
OBJS = $(SRCS:.c=.o)

TOOL_SRCS = $(wildcard tools/*.c)
TOOLS = $(TOOL_SRCS:.c=)

.PHONY: all clean tools

all: libcomatose.a

libcomatose.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tools: libcomatose.a $(TOOLS)

tools/%: tools/%.c libcomatose.a
	$(CC) $(CFLAGS) -static -o $@ $< -L. -lcomatose

clean:
	rm -f $(OBJS) libcomatose.a $(TOOLS)
