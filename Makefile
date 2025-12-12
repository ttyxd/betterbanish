# vim:ts=8

CC	?= cc
CFLAGS	?= -O2
CFLAGS	+= -Wall -Wunused -Wmissing-prototypes -Wstrict-prototypes

PREFIX	?= /usr/local
BINDIR	?= $(PREFIX)/bin
MANDIR	?= $(PREFIX)/man/man1

INSTALL_PROGRAM ?= install -s
INSTALL_DATA ?= install

LIBS	?= x11 xfixes xi xext libsystemd
INCLUDES?= `pkg-config --cflags $(LIBS)`
LDFLAGS	+= `pkg-config --libs $(LIBS)` -ludev

PROG	= betterbanish
OBJS	= betterbanish.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OBJS): *.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(PROG) $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)
	$(INSTALL_DATA) -m 644 betterbanish.1 $(DESTDIR)$(MANDIR)/betterbanish.1

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all install clean
