/*
 * Copyright (c) 2009, 2010 Joachim Schipper <joachim@joachimschipper.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Various randomization algorithms.
 */

#ifndef HAVE_ARC4RANDOM
#ifndef HAVE_SRANDOMDEV
#include <sys/types.h>
#endif
#endif

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#ifdef HAVE_SIGINFO
#include <signal.h>
#endif
#include <stdio.h>
#include <string.h>
#ifndef HAVE_ARC4RANDOM
#ifndef HAVE_SRANDOMDEV
#include <time.h>
#endif
#endif
#include <unistd.h>

#include <pcre.h>

#include "compat.h"
#include "record.h"

#ifndef __GNUC__
#ifndef __attribute__
#define __attribute__(x) /* Not supported by non-GCC compilers */
#endif
#endif
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#ifdef HAVE_SIGINFO
static volatile sig_atomic_t got_siginfo = 0;
static void handle_siginfo(int sig);
#endif

int main(int argc, char **argv);

static void usage(void) __attribute__((noreturn));

static const size_t memory_cache_initial = 16 * 1024;

static void
usage(void)
{
	fprintf(stderr, "randomize [-a | -e regex] [-o str] [-n number] [arg [arg ...]]\n");
	exit(127);
}

#ifdef HAVE_SIGINFO
static void
handle_siginfo(int sig)
{
	assert(sig == SIGINFO);

	got_siginfo = 1;
}
#endif

int
main(int argc, char **argv)
{
	const char	*re_str, *delim, *errstr;
	int		 ch, fd, rfd, error_offset, rv, process_options;
	unsigned int	 i, j;
	uint_fast32_t	 r, nrecords, rec_size, rec_no;
	struct rec	*rec, to_free;
	void		*tmp;
	pcre		*re;
	pcre_extra	*re_extra;
	size_t		 memory_cache;
#ifndef NDEBUG
	int		 to_free_valid;
#endif
#ifdef HAVE_SIGINFO
	struct sigaction act;

	/*
	 * Enable SIGINFO handler
	 */
	act.sa_handler = handle_siginfo;
	/* We do *not* wish to restart read() and the like on SIGINFO */
	act.sa_flags = 0;
	/* Do not block any other signals while handling this signal */
	act.sa_mask = 0;

	sigaction(SIGINFO, &act, NULL);
#endif
#ifndef HAVE_ARC4RANDOM
#ifdef HAVE_SRANDOMDEV
	/* Initialize random number generator */
	srandomdev();
#else
	/* ...badly... */
	srandom((unsigned int) getpid() ^ (unsigned int) time(NULL));
#endif
#endif

#ifndef NDEBUG
	to_free_valid = 0;
#endif
	memory_cache = memory_cache_initial;
	re = NULL;
	re_extra = NULL;
	tmp = NULL;
	rfd = 0;

	/* Defaults */
	re_str = "\n";
	delim = "\n";
	nrecords = UINT32_MAX;
	process_options = 1;

	while ((ch = getopt(argc, argv, "ae:n:o:")) != -1) {
		/*
		 * Note: option processing is *partially* duplicated below,
		 * search for "== '-'"
		 */
		switch (ch) {
		case 'a':
			re_str = NULL;
			break;
		case 'e':
			re_str = optarg;
			break;
		case 'n':
			/* LINTED conversion clearly works */
			nrecords = strtonum(optarg, 1, UINT32_MAX - 1, &errstr);
			if (errstr)
				errx(1, "number of records is %s: %s", errstr, optarg);
			break;
		case 'o':
			delim = optarg;
			break;
		default:
			assert(ch == '?');
			usage();
			/* NOTREACHED */
		}
	}
	assert(optind > 0);
	if (strcmp(argv[optind - 1], "--") == 0)
		/* Stop option processing */
		process_options = 0;
	argc -= optind;
	argv += optind;

	if (re_str == NULL) {
		/*
		 * Skip the usual regex-based stuff and randomize the arguments
		 * (instead of treating them as file names).
		 */
		/* LINTED argc is nonnegative, so this works */
		j = argc;
		/* LINTED idem */
		while (j > (nrecords > argc ? 0 : argc - nrecords)) {
			r = random_uniform(j);

			if (printf("%s", argv[r]) == -1)
				err(1, "Failed to print");
			if ((errstr = rec_write_str(delim, stdout)) != NULL)
				errx(1, "%s", errstr);

			argv[r] = argv[--j];
		}

		exit(0);
	}

	if ((rec = malloc((rec_size = 128) * sizeof(*rec))) == NULL)
		err(1, "Failed to allocate memory for records");

	rec_no = 0;
	/* LINTED argc is still nonnegative */
	for (i = 0; i < MAX(argc, 1); i++) {
		/* Process -e, -o */
		/* LINTED idem */
		if (process_options && i < argc && argv[i][0] == '-' &&
		    argv[i][1] != '\0' && argv[i][2] == '\0') {
			switch(argv[i][1]) {
			case 'e':
				/* LINTED idem */
				if (i == MAX(argc, 1) - 1)
					usage();
				re_str = argv[++i];
				if (re != NULL && pcre_refcount(re, 0) == 0) {
					pcre_free(re);
					pcre_free(re_extra);
				}
				re = NULL;
				break;
			case 'o':
				/* LINTED idem */
				if (i == MAX(argc, 1) - 1)
					usage();
				delim = argv[++i];
				break;
			case '-':
				process_options = 0;
				break;
			default:
				usage();
			}
			continue;
		}

		if (re == NULL) {
			/*
			 * Reference-counted and freed by the rec_* functions.
			 */
			/* XXX Do we need to call maketables()? */
			if ((re = pcre_compile(re_str, PCRE_DOTALL | PCRE_EXTRA | PCRE_MULTILINE, &errstr, &error_offset, NULL)) == NULL)
				errx(1, "Failed to parse regular expression %s: %s at %d", re_str, errstr, error_offset);
			re_extra = pcre_study(re, 0, &errstr);
			if (errstr != NULL)
				errx(1, "Failed to study regular expression %s: %s", re_str, errstr);
		}

		/* Open file */
		if (argc == 0 || strcmp(argv[i], "-") == 0)
			fd = 0;
		else
			if ((fd = open(argv[i], O_RDONLY, 0644)) == -1)
				err(1, "Failed to open %s", argv[i]);

		if ((rfd = rec_open(fd, re, re_extra, delim, &memory_cache)) == -1)
			err(1, "Failed to rec_open %s", strcmp(argv[i], "-") == 0 ? "stdin" : argv[i]);

		/*
		 * The loop below is a Knuth/Fisher-Yates shuffle, i.e.
		 *
		 *	while (!eof) {
		 *		r = random_uniform(rec_no + 1);
		 *		rec[rec_no] = rec[r];
		 *		rec[r] = rec_next();
		 *		rec_no++;
		 *	}
		 *
		 * but we never store more than nrecords records.
		 *
		 * Loop invariant: rec[0] to rec[MIN(rec_no, nrecords)] is a
		 * uniformly random selection of distinct records.
		 */
		while (1) {
			if (MIN(rec_no + 1, nrecords) > rec_size) {
				if (rec_size > MIN(UINT32_MAX / 2, SIZE_MAX / 2 / sizeof(*rec)))
					err(1, "Too many records");
				if ((tmp = realloc(rec, 2 * rec_size * sizeof(*rec))) == NULL)
					err(1, "Failed to allocate memory for more records");

				rec = tmp;
				rec_size *= 2;
			}

			r = random_uniform(rec_no + 1);
			if (r < MIN(rec_no, nrecords)) {
				/* Overwriting valid data */
				if (rec_no < nrecords)
					rec[rec_no] = rec[r];
				else {
					assert(to_free_valid == 0);
					to_free = rec[r];
#ifndef NDEBUG
					to_free_valid = 1;
#endif
				}
			}

try_again:
#ifdef HAVE_SIGINFO
			if (got_siginfo) {
				got_siginfo = 0;
				fprintf(stderr, "Reading %s: read %" PRIuFAST32 " records (in total)\n",
				    argc == 0 || strcmp(argv[i], "-") == 0 ? "stdin" : argv[i],
				    rec_no);
				fflush(stderr);
			}
#endif
			if (rec_next(rfd, r < nrecords ? &rec[r] : NULL) != 0) {
				if (errno == EAGAIN || errno == EINTR)
					goto try_again;
				else if (errno == 0) {
					if (r < MIN(rec_no, nrecords) && rec_no >= nrecords) {
						/*
						 * We don't want to free
						 * to_free = rec[r] after
						 * all...
						 */
						assert(to_free_valid == 1);
#ifndef NDEBUG
						to_free_valid = 0;
#endif
					}
					break;
				} else
					errx(1, "Failed to read from %s: %s%s",
					    argc == 0 || strcmp(argv[i], "-") == 0 ? "stdin" : argv[i],
					    strerror(errno),
					    errno == EINVAL ? ", error in regular expression or zero-length match" : "");
			}

			if (r < MIN(rec_no, nrecords) && rec_no >= nrecords) {
				assert(to_free_valid == 1);
				rec_free(&to_free);
#ifndef NDEBUG
				to_free_valid = 0;
#endif
			}

			if (++rec_no == UINT32_MAX - 1)
				errx(1, "Too many records");
		}
	}
	assert(to_free_valid == 0);

	/* Write out data */
	for (i = 0; i < MIN(rec_no, nrecords); i++) {
try_again2:
#ifdef HAVE_SIGINFO
		if (got_siginfo) {
			got_siginfo = 0;
			fprintf(stderr, "Writing record %u/%" PRIuFAST32 "\n",
			    i + 1,
			    MIN(rec_no, nrecords));
		}
#endif

		if ((errstr = rec_write(&rec[i], NULL, stdout)) != NULL) {
			if (errno == EAGAIN || errno == EINTR)
				goto try_again2;
			else
				errx(1, "%s", errstr);
		}

		rec_free(&rec[i]);
	}

#ifndef NDEBUG
	/*
	 * Deallocate all rfds. Note that these are guaranteed to start at the
	 * lowest descriptor.
	 */
	/* LINTED argc is nonnegative, so this works */
	for (i = 0; i <= rfd; i++) {
		/* LINTED converting i to signed int works */
		while ((rv = rec_close(i)) != 0 && (errno == EINTR || errno == EAGAIN));
		if (rv != 0)
			err(1, "Failed to rec_close %s", argc == 0 ? "stdin" : argv[i]);
	}

	assert(memory_cache == memory_cache_initial);
	rec_assert_released();
#endif

	exit(0);
}
