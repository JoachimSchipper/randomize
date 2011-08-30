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
OBJS=compat.o record.o randomize.o rnd.o
# For systems with groff but no mandoc, use
#MANDOC=groff -mandoc
MANDOC?=mandoc

all: randomize randomize.cat1 rnd

clean:
	rm -f randomize randomize.cat1 ${OBJS} test/{1,2,3,4,5}.result rnd

lint:
	${LINT} ${OBJS:.o=.c}

randomize: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o randomize ${OBJS}

rnd: rnd.c rnd.h
	@# GCC complains about some *stupid* things here (note that we build
	@# the .o with all warnings)
	${CC} ${CFLAGS} -DTESTSUITE -Wno-format -o rnd rnd.c

test: randomize test/1.in test/1.out test/2.in test/2.out test/3.in test/3.out test/4a.in test/4b.in test/4c.in test/4.out test/5.in test/5.out
	# Simplest case, but file has no eol
	./randomize test/1.in | sort > test/1.result &&\
		diff -u test/1.out test/1.result
	# Error on no end of line
	./randomize -e '\n' -o '&' test/1.in >/dev/null &&\
		echo 'This is not supposed to work' >&2 &&\
		exit 1 || true
	# Error on zero-width match
	./randomize -e '.*?' -o '&' test/1.in >/dev/null &&\
		echo 'This is not supposed to work' >&2 &&\
		exit 1 || true
	# Randomize arguments
	./randomize -e 'ignored' -o '\n' -an 10 `cat test/1.in` | sort > test/1.result &&\
		diff -u test/1.out test/1.result
	# Reading from pipe (long file, partially in memory)
	cat test/2.in | ./randomize | sort > test/2.result &&\
		diff -u test/2.out test/2.result
	# Long lines
	./randomize test/3.in | sort > test/3.result &&\
		diff -u test/3.out test/3.result
	# Multiple files
	cat test/4a.in | ./randomize - test/4b.in test/4c.in | sort > test/4.result &&\
		diff -u test/4.out test/4.result
	# Regular expression and escape support
	./randomize -e '(.*?)([ \t])' -o '\0\x0\xb\xB\2\1\n' test/5.in | sort > test/5.result &&\
		diff -u test/5.out test/5.result
	# Multiple files, changing -e/-o midway
	cat test/4a.in | (cd test/ && ../randomize -o '&' - -e '(.*?)([ \t])' -o '\1 [5.in]\n' 5.in -e '\n' -o '\n' -- -e) | sort > test/6.result &&\
		diff -u test/6.out test/6.result
	# Requesting a few lines
	./randomize -n 1 test/2.in >/dev/null || exit 1;
	cat test/2.in | ./randomize -n 1 >/dev/null || exit 1;
	./randomize -n 512 test/2.in >/dev/null || exit 1;
	cat test/2.in | ./randomize -n 512 >/dev/null || exit 1;
	./randomize -n 4096 test/2.in | sort > test/2.result &&\
		diff -u test/2.out test/2.result
	cat test/2.in | ./randomize -n 4096 | sort > test/2.result &&\
		diff -u test/2.out test/2.result
	# XXX Integrate into previous
	./randomize -pn1 test/1.in >/dev/null
	./randomize -pn1 < test/1.in >/dev/null
	for i in $$(for i in $$(jot 120 0 0); do ./randomize -pn1 test/1.in; done | sort | uniq -c | awk '{print $$1}'); do \
		if [ $$i -lt 20 -o $$i -gt 60 ]; then \
			echo "This seems unlikely." >&2; \
			exit 1; \
		fi; \
	done

${OBJS}: compat.h record.h rnd.h

randomize.cat1: randomize.1
	${MANDOC} -Tascii randomize.1 > randomize.cat1
