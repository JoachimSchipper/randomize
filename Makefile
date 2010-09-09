.PHONY: all clean lint test

# Define HAVE_ARC4RANDOM on platforms that have arc4random_uniform() to obtain fast and
# decent random numbers.
#
# Define HAVE_SRANDOMDEV for platforms that have srandomdev() to obtain
# semi-decent random numbers via random(3). (The arc4random_uniform() interface
# is preferred if available.)
#
# Define HAVE_SIGINFO on platforms that support SIGINFO to enable printing data
# on the console on receipt of SIGINFO.
#
# Define HAVE_VIS on platforms that have a vis(3) routine; otherwise, a
# replacement is used.
DEFINES=-DHAVE_ARC4RANDOM -DHAVE_SRANDOMDEV -DHAVE_SIGINFO -DHAVE_VIS
# Warns on pretty much everything, except two conditions (signed compare and
# unused parameters) that are not necessarily errors. Lint catches those, and
# we can suppress lints warnings.
#
# -Wpadded or the OpenBSD extension -Wlarger-than-X may occasionally be useful
# as well. -Wcast-qual may give useful warnings, but those are duplicated by
# lint.
CFLAGS=-std=c99 -pedantic -W -Wall -Wno-sign-compare -Wno-unused-parameter -Wbad-function-cast -Wcast-align -Wchar-subscripts -Wfloat-equal -Wmissing-declarations -Wmissing-format-attribute -Wmissing-noreturn -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wshadow -Wstrict-prototypes -Wwrite-strings -Wundef -Werror -g -O2 -I/usr/local/include ${DEFINES}
# -Wredundant-decls
LDFLAGS=-L/usr/local/lib -lpcre
LINT=lint -ceFHrx -DLINT -I/usr/local/include -L/usr/local/lib -lpcre ${DEFINES}
OBJS=compat.o record.o randomize.o

all: randomize randomize.cat1

clean:
	rm -f randomize randomize.cat1 ${OBJS} test/{1,2,3,4,5}.result

lint:
	${LINT} ${OBJS:.o=.c}

randomize: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o randomize ${OBJS}

test: randomize test/1.in test/1.out test/2.in test/2.out test/3a.in test/3b.in test/3c.in test/3.out test/4.in test/4.out test/5.in test/5.out
	# Basic functionality
	./randomize test/1.in | sort > test/1.result &&\
		diff -u test/1.out test/1.result
	cat test/1.in | ./randomize | sort > test/1.result &&\
		diff -u test/1.out test/1.result
	./randomize -e 'ignored' -o '\n' -an 10 `cat test/1.in` | sort > test/1.result &&\
		diff -u test/1.out test/1.result
	# Long lines
	./randomize test/2.in | sort > test/2.result &&\
		diff -u test/2.out test/2.result
	# Multiple files
	./randomize test/3a.in test/3b.in test/3c.in | sort > test/3.result &&\
		diff -u test/3.out test/3.result
	# Regular expression support
	./randomize -e '(.*?)([ \t])' -o '\0\x0\2\1\n' test/4.in | sort > test/4.result &&\
		diff -u test/4.out test/4.result
	# Requesting a few lines
	for i in 1 2 3;\
		do ./randomize -n $$i test/5.in >/dev/null || exit 1;\
	done
	for i in 4 5 6 10000;\
		do ./randomize -n $$i test/5.in | sort > test/5.result &&\
			diff -u test/5.out test/5.result || exit 1;\
	done

${OBJS}: compat.h record.h

randomize.cat1: randomize.1
	groff -Tascii -mandoc randomize.1 > randomize.cat1
