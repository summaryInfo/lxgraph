NAME ?= lxgraph
PERFIX ?= /usr/local
BINDIR ?= /usr/local/bin
MANDIR ?= /usr/local/share/man
SHAREDIR ?= /usr/local/share

CFLAGS ?= -O2 -flto -g

CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CFLAGS += -Wno-unknown-warning -Wno-unknown-warning-option
CFLAGS += -Walloca -Wno-aggressive-loop-optimizations
CFLAGS += -Wdisabled-optimization -Wduplicated-branches -Wduplicated-cond
CFLAGS += -Wignored-attributes  -Wincompatible-pointer-types
CFLAGS += -Winit-self -Wwrite-strings -Wvla
CFLAGS += -Wmissing-attributes -Wmissing-format-attribute -Wmissing-noreturn
CFLAGS += -Wswitch-bool -Wpacked -Wshadow -Wformat-security
CFLAGS += -Wswitch-unreachable -Wlogical-op -Wstringop-truncation
CFLAGS += -Wbad-function-cast -Wnested-externs -Wstrict-prototypes

OBJ := main.o util.o callgraph.o

LDLIBS += -lm -lclang

all: $(NAME)

clean:
	rm -rf *.o $(NAME)

force: clean
	$(MAKE) all

install-strip: install
	strip $(BINDIR)/$(NAME)

install: all
	install -D $(NAME) $(BINDIR)/$(NAME)

uninstall:
	rm -f $(BINDIR)/$(NAME)

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $@

main.o: util.h
uri.o: util.h hashtable.h
callgraph.o: util.h hashtable.h

.PHONY: all clean install install-strip uninstall force
