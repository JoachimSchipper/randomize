.PHONY: all clean lint test

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
	rm -f randomize randomize.cat1 ${OBJS} test/{1,2a,2b,2c,3,4}.result

lint:
	${LINT} ${OBJS}

randomize: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o randomize ${OBJS}

test: randomize test/1.in test/1.out test/2a.in test/2b.in test/2c.in test/2.out test/3.in test/3.out test/4.in test/4.out
	# Basic functionality, reading from pipe
	cat test/1.in | ./randomize | sort > test/1.result &&\
		diff -u test/1.out test/1.result
	# Multiple files
	./randomize test/2a.in test/2b.in test/2c.in | sort > test/2.result &&\
		diff -u test/2.out test/2.result
	# Regular expression support
	./randomize -e '(.*?)([ \t])' -o '\0\x0\2\1\n' test/3.in | sort > test/3.result &&\
		diff -u test/3.out test/3.result
	# Requesting a few lines
	for i in 1 2 3;\
		do ./randomize -n $$i test/4.in >/dev/null || exit 1;\
	done
	for i in 4 5 6 10000;\
		do ./randomize -n $$i test/4.in | sort > test/4.result &&\
			diff -u test/4.out test/4.result || exit 1;\
	done

${OBJS}: record.h

randomize.cat1: randomize.1
	groff -Tascii -mandoc randomize.1 > randomize.cat1
