/*
 * Copyright (c) 2009 Joachim Schipper <joachim@joachimschipper.nl>
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
 * Randomly permute the lines given on stdin. See usage for details.
 *
 * Define HAVE_ARC4RANDOM on platforms that have arc4random for better
 * performance. Define HAVE_SIGINFO on platforms that support SIGINFO to enable
 * printing data on the console on receipt of SIGINFO.
 */

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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_SIGINFO
static sig_atomic_t got_siginfo = 0;
void handle_siginfo(int sig);
#endif
int main(int argc, char **argv);

const char *usage = "randomize [-0hz]\n\t-0\t\tAccept null-terminated input (see xargs(1))\n\t-h\t\tShow this help\n\t-z\t\tProduce null-terminated output (again, see xargs(1))";

#ifndef HAVE_ARC4RANDOM
/*
 * Return a random number chosen uniformly from 0, 1, ..., max - 1.
 */
uint32_t random_uniform(uint32_t max);
#define RANDOM(max) random_uniform(max)
uint32_t
random_uniform(uint32_t max)
{
	uint32_t	 r;

	r = random();

	if (r > (UINT32_MAX - (UINT32_MAX % max)))
		/* Try again to make sure we really are random */
		return random_uniform(max);

	return r % max;
}
#else
#define RANDOM(max) arc4random_uniform(max)
#endif

#ifdef HAVE_SIGINFO
void
handle_siginfo(int sig)
{
	assert(sig == SIGINFO);

	got_siginfo = 1;
}
#endif

int
main(int argc, char **argv)
{
	int		 ch, null_terminated_input, null_terminated_output, fd;
	char		*buf, *p;
	uint32_t	 nlines, i, r;
	ssize_t		 bytes_read, bytes_written, len;
	size_t		 buf_size, j, oldj, *line, total_bytes_written;
	void		*tmp;
#ifdef HAVE_SIGINFO
	struct sigaction act;
#endif

	null_terminated_input = null_terminated_output = 0;
	while ((ch = getopt(argc, argv, "0hz")) != -1) {
		switch (ch) {
		case '0':
			null_terminated_input = 1;
			break;
		case 'h':
			errx(0, usage);
			/* NOTREACHED */
		case 'z':
			null_terminated_output = 1;
			break;
		default:
			errx(127, usage);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (argv[0] == NULL) {
		if ((argv = malloc(2 * sizeof(*argv))) == NULL)
			err(1, "Failed to allocate synthetic argument vector");

		argv[0] = "-";
		argv[1] = NULL;
	}

	if ((buf = malloc(buf_size = BUFSIZ)) == NULL)
		err(1, "Failed to allocate memory for buffer\n");
	if ((line = malloc((nlines = 16) * sizeof(*line))) == NULL)
		err(1, "Failed to allocate memory for lines\n");

#ifdef HAVE_SIGINFO
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

	/*
	 * Read all data into buf. Find lines, add them to line[], and
	 * newline-terminate them.
	 */
	oldj = j = 0;
	i = 0;
	line[i++] = 0;
	for ( ; *argv != NULL; argv++) {
		if (strcmp(*argv, "-") == 0)
			fd = 0;
		else {
			if ((fd = open(*argv, O_RDONLY, 0000)) == -1)
				err(1, "Failed to read file %s", *argv);
		}

		while ((bytes_read = read(fd, buf + j, buf_size - j - 1)) != 0) {
#ifdef HAVE_SIGINFO
			if (got_siginfo) {
				fprintf(stderr, "Read %zu bytes containing %" PRIu32 " lines\n", j, i - 1);
				got_siginfo = 0;
			}
#endif

			if (bytes_read == -1) {
				if (errno == EINTR)
					/* Try again */
					continue;
				else
					err(1, "Error while reading from %s", (strcmp(*argv, "-") == 0 ? "stdin" : *argv));
			}

			j += bytes_read;

			if (buf_size - j < BUFSIZ) {
				if ((tmp = realloc(buf, buf_size <= SIZE_MAX / 2 ? 2 * buf_size : SIZE_MAX)) == NULL)
					err(1, "Failed to enlarge buffer");

				buf = tmp;
				buf_size = buf_size <= SIZE_MAX / 2 ? 2 * buf_size : SIZE_MAX;
			}

			for ( ; oldj < j; oldj++) {
				if ((!null_terminated_input && buf[oldj] == '\n') || buf[oldj] == '\0') {
					buf[oldj] = '\0';

					if (i == nlines - 1) {
						if ((tmp = realloc(line, nlines <= SIZE_MAX / sizeof(*line) / 2 ? nlines * 2 * sizeof(*line) : SIZE_MAX / sizeof(*line) * sizeof(*line))) == NULL)
							err(1, "Failed to enlarge line buffer");

						line = tmp;
						nlines = nlines <= SIZE_MAX / sizeof(*line) / 2 ? nlines * 2 : SIZE_MAX / sizeof(*line);
					}

					/* May point past bufp, but we'll fix that below */
					line[i++] = oldj + 1;
				}
			}
		}

		/* Terminate */
		buf[j] = '\n';

		while (close(fd) != 0 && errno != EINTR);
	}

	/* Set nlines to actual number of lines used */
	if (line[i - 1] >= j)
		nlines = i - 1;
	else
		nlines = i;

	/* Print in random order */
	total_bytes_written = 0;
	for (i = nlines; i > 0; i--) {
		r = RANDOM(i);
		p = &buf[line[r]];
		len = strlen(p);
		if (!null_terminated_output)
			/* Change terminating \0 to \n for printing */
			p[len] = '\n';
		len++;

		while ((bytes_written = write(1, p, len)) != 0) {
#ifdef HAVE_SIGINFO
			if (got_siginfo) {
				fprintf(stderr, "Written %zu/%zu bytes, %" PRIu32 "/%" PRIu32 " lines\n", total_bytes_written, j, nlines - i, nlines);
				got_siginfo = 0;
			}
#endif

			if (bytes_written == -1) {
				if (errno == EINTR)
					continue;
				else
					err(1, "Error while writing to stdout");
			}

			total_bytes_written += bytes_written;

			p += bytes_written;
			len -= bytes_written;
		}

		line[r] = line[i - 1];
	}

	exit(0);
}
