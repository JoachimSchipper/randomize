.PHONY: all clean lint

# Define HAVE_ARC4RANDOM on platforms that have arc4random for better
# performance. Define HAVE_SIGINFO on platforms that support SIGINFO to enable
# printing data on the console on receipt of SIGINFO.
DEFINES=-DHAVE_ARC4RANDOM -DHAVE_SIGINFO
# Warns on pretty much everything, except two conditions (signed compare and
# unused parameters) that are not necessarily errors. Lint catches those, and
# we can suppress lints warnings.
#
# -Wpadded or the OpenBSD extension -Wlarger-than-X may occasionally be useful
#  as well. -Wcast-qual may give useful warnings, but those are duplicated by
#  lint.
CFLAGS=-std=c99 -pedantic -W -Wall -Wno-sign-compare -Wno-unused-parameter -Wbad-function-cast -Wcast-align -Wchar-subscripts -Wfloat-equal -Wmissing-declarations -Wmissing-format-attribute -Wmissing-noreturn -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wredundant-decls -Wshadow -Wstrict-prototypes -Wwrite-strings -Wundef -Werror -g -O2 -I/usr/local/include ${DEFINES}
LDFLAGS=-L/usr/local/lib -lpcre
LINT=lint -ceFHrx -DLINT ${DEFINES}
OBJS=record.o randomize.o

all: randomize randomize.cat1

clean:
	rm -f randomize randomize.cat1 ${OBJS}

lint:
	${LINT} ${OBJS}

randomize: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o randomize ${OBJS}

${OBJS}: record.h

randomize.cat1: randomize.1
	groff -Tascii -mandoc randomize.1 > randomize.cat1
