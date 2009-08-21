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

#include <sys/types.h>
#include <sys/uio.h>

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
/* 
 * Handle escape sequences; e.g. "\\\n" becomes "\
 * ". Returns the size of the resulting data (which is guaranteed to be
 * NULL-terminated, but may *contain* NULLs...)
 */
int unescape(char *str);
int main(int argc, char **argv);

const char *usage = "randomize [-h] [-i input_delim [-i input_delim ...]] [-o output_delim] [file [file ...]]";

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
unescape(char *str)
{
	char		*p, *pw, ch;
	const struct {
		char	 ch, stands_for;
	} escape_sequences[] = {
		{ '0',	'\0' },
		{ 'a',	'\a' },
		{ 'b',	'\b' },
		{ 'f',	'\f' },
		{ 'n',	'\n' },
		{ 'r',	'\r' },
		{ 't',	'\t' },
		{ 'v',	'\v' },
		{ '\\',	'\\' },
		{ '?', 	'\?' }
	};
	int		 i, len;

	p = pw = str;
	len = 0;
	for (p = pw = str, len = 0; *p != NULL; len++) {
		/*
		 * Parse input, looking ahead if we see a backslash. We produce
		 * one output character per loop through the cycle.
		 */
		if (*p != '\\') {
			*pw++ = *p++;
		} else if (strspn(p + 1, "01234567") >= 3 || (p[1] == 'x' && strspn(p + 2, "01234567890ABCDEF") >= 2)) {
			/* Octal or hex specification */
			ch = p[4];
			p[4] = '\0';
			if (p[1] == 'x')
				*pw++ = strtoul(p + 1, NULL, 8);
			else
				*pw++ = strtoul(p + 2, NULL, 16);
			*(p += 4) = ch;
		} else {
			for (i = 0; i < sizeof(escape_sequences) / sizeof(*escape_sequences); i++) {
				/* \n or something similar */
				if (p[1] == escape_sequences[i].ch) {
					*pw++ = escape_sequences[i].stands_for;
					p += 2;

					continue;
				}
			}

			/* Some unidentified escape sequence, e.g. \l. */
			if (p[1] == '\0')
				*pw++ = *p++;
			else {
				*pw++ = p[1];
				p += 2;
			}
		}
	}

	*pw = '\0';

	return len;
}

int
main(int argc, char **argv)
{
	char		*buf;
	struct {
		const char
			*chars;
		int	 size;
	}		*input_delimiter, output_delimiter;
	int		 ch, fd;
	uint32_t	 nlines, i, r;
	ssize_t		 bytes_read, bytes_written;
	struct {
		size_t	 start, len;
	}		*line;
	size_t		 buf_size, j, oldj, total_bytes_written, input_delimiters_size, k;
	void		*tmp;
	struct iovec	 iov[2];
#ifdef HAVE_SIGINFO
	struct sigaction act;
#endif

	/* "Unitialized" */
	input_delimiter = NULL;
	input_delimiters_size = 0;
	output_delimiter.chars = (const char []){'\n'};
	output_delimiter.size = 1;

	while ((ch = getopt(argc, argv, "hi:o:")) != -1) {
		switch (ch) {
		case 'h':
			errx(0, usage);
			/* NOTREACHED */
		case 'i':
			if (input_delimiters_size + 1 > SIZE_MAX / sizeof(*input_delimiter)) {
				errno = ENOMEM;
				errx(1, "Too many input delimiters - something went horribly wrong!");
			}
			if ((tmp = realloc(input_delimiter, ++input_delimiters_size * sizeof(*input_delimiter))) == NULL)
				err(1, "Failed to allocate more input delimiters");
			input_delimiter = tmp;
			buf = strdup(optarg);
			input_delimiter[input_delimiters_size - 1].size = unescape(buf);
			input_delimiter[input_delimiters_size - 1].chars = buf;
			break;
		case 'o':
			buf = strdup(optarg);
			output_delimiter.size = unescape(buf);
			output_delimiter.chars = buf;
			break;
		default:
			errx(127, usage);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	/* Use defaults (\0, \n) if not initalized */
	if (input_delimiter == NULL) {
		if ((input_delimiter = malloc((input_delimiters_size = 2) * sizeof(*input_delimiter))) == NULL)
			err(1, "Failed to allocate default input delimiters");
		input_delimiter[0].chars = (const char []){'\0'};
		input_delimiter[1].chars = (const char []){'\n'};
		input_delimiter[0].size = input_delimiter[1].size = 1;
	}

	if (argv[0] == NULL) {
		if ((argv = malloc(2 * sizeof(*argv))) == NULL)
			err(1, "Failed to allocate synthetic argument vector");

		/* XXX This is ugly. */
		/* strdup() is not necessary, but does keep gcc happy */
		argv[0] = strdup("-");
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
	line[i = 0].start = 0;
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
				for (k = 0; k < input_delimiters_size; k++) {
					if (j - oldj >= input_delimiter[k].size && memcmp(&buf[oldj], input_delimiter[k].chars, input_delimiter[k].size) == 0) {
						if (i == nlines - 1) {
							if ((tmp = realloc(line, nlines <= SIZE_MAX / sizeof(*line) / 2 ? nlines * 2 * sizeof(*line) : SIZE_MAX / sizeof(*line) * sizeof(*line))) == NULL)
								err(1, "Failed to enlarge line buffer");

							line = tmp;
							nlines = nlines <= SIZE_MAX / sizeof(*line) / 2 ? nlines * 2 : SIZE_MAX / sizeof(*line);
						}

						line[i].len = oldj - line[i].start;

						/* May point past bufp, but we'll fix that below */
						oldj += input_delimiter[k].size - 1;
						line[++i].start = oldj + 1;

						break;
					}
				}
			}
		}

		while (close(fd) != 0 && errno != EINTR);
	}

	/* Set nlines to actual number of lines used */
	line[i].len = j - line[i].start;
	if (line[i].start >= j)
		nlines = i;
	else
		nlines = i + 1;

	/* Print in random order */
	total_bytes_written = 0;
	for (i = nlines; i > 0; i--) {
		r = RANDOM(i);
		iov[0].iov_base = &buf[line[r].start];
		iov[0].iov_len = line[r].len;
		iov[1].iov_base = output_delimiter.chars;
		iov[1].iov_len = output_delimiter.size;

		/* Debug */
		while ((bytes_written = writev(1, iov, sizeof(iov) / sizeof(*iov))) != 0) {
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

			if (bytes_written == iov[0].iov_len + iov[1].iov_len)
				break;
			else if (bytes_written > iov[0].iov_len) {
				iov[0].iov_len = 0;
				bytes_written -= iov[0].iov_len;
				iov[1].iov_base = ((char *) iov[1].iov_base) + bytes_written;
				iov[1].iov_len -= bytes_written;
			} else {
				iov[0].iov_base = ((char *) iov[0].iov_base) + bytes_written;
				iov[0].iov_len -= bytes_written;
			}
		}

		line[r] = line[i - 1];
	}

	exit(0);
}
