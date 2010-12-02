/*
 * Copyright (c) 2010 Joachim Schipper <joachim@joachimschipper.nl>
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

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pcre.h>

#include "compat.h"
#include "record.h" /* vis.h */

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) < (y) ? (x) : (y))
#endif

static struct {
	off_t		 offset;	/* Current offset into tmp. If this is
					 * -1, the struct is unused. */
	pcre		*re;		/* The regex for this file */
	pcre_extra	*re_extra;
	size_t		*memory_cache;	/* How much more memory can we use? */
	/*
	 * At any moment, for any i between 0 and f_last,
	 * - f[i].buf_p[f[i].buf_first_read] to f[i].buf_p[f[i].buf_last] is
	 *   valid, unprocessed data;
	 * - f[i].buf_p[f[i].buf_first_write] to
	 *   f[i].buf_p[f[i].buf_first_read] is valid data that has been
	 *   processed and should be flushed to f[i].tmp (if tmp != fd);
	 * - f[i].buf_p[0] to f[i].buf_p[f[i].buf_first_read] and
	 *   f[i].buf_p[f[i].buf_last] to f[i].buf_p[f[i].buf_size] is garbage.
	 *
	 * Note that f[i].buf_first_write is not used (and always 0) if
	 * f[i].fd == f[i].tmp.
	 *
	 * Note that buf_p may be NULL, e.g. after rec_fadvise(f[i].
	 * REC_FADV_DONTNEED).
	 */
	char		*buf_p;
	/* Limited to int instead of size_t by pcre_exec() */
	int		 buf_first_write, buf_first_read, buf_last, buf_size;
	/*
	 * fd is the file descriptor passed to rec_open() and tmp is either
	 * equal to fd (if fd is seekable) or a file descriptor pointing to a
	 * temporary file.
	 */
	int		 fd, tmp;
}		*f = NULL;
static int	 f_size = 0, f_last = 0;

/*
 * Using struct rec.
 *
 * Note: if rec->internal_only.len <= 0, loc.p should be used; otherwise, use
 * loc.offset.
 *
 * Note: if rec->internal_only.f_idx < 0, this record is the last record in the
 * file; the actual f_idx is -rec->internal_only.f_idx if this is positive, or
 * 0 for INT_MIN. Note that the converse may not be true, i.e. the last record
 * in the file may not be last as determined by REC_IS_LAST(). This does not
 * cause problems for the current code, as it's used only to determine whether
 * or not to pass PCRE_NOTEOL to pcre_exec().
 *
 * XXX Is there any regex that can abuse this?
 */
#define REC_IS_OFFSET(rec) ((rec)->internal_only.len > 0)
#define REC_IS_LAST(rec) ((rec)->internal_only.f_idx < 0)
#define REC_LEN(rec) abs((rec)->internal_only.len)
/*
 * Estimated memory use, i.e. memory we actually use plus malloc overhead. Note
 * that this cannot overflow since REC_LEN(rec) <= INT_MAX.
 */
#define REC_ESTIMATED_MEMORY_USE(rec) (REC_LEN(rec) + 2 * sizeof(void *) + 2 * sizeof(size_t))
#define REC_OFFSET(rec) (assert(REC_IS_OFFSET(rec)), (rec)->internal_only.loc.offset)
#define REC_P(rec) (assert(!REC_IS_OFFSET(rec)), (rec)->internal_only.loc.p)
#define REC_F(rec) f[(rec)->internal_only.f_idx == INT_MIN ? 0 : abs((rec)->internal_only.f_idx)]

/*
 * Some helper variables, automatically deallocated when all rfd's are closed.
 *
 * ovector* is used by pcre_exec(), and rec_open() makes sure it's long enough.
 * buf is used by rec_write(), which handles allocation/resizing.
 */
static int	*ovector = NULL, ovector_size = 0;
static char	*w_buf = NULL;
static size_t	 w_buf_size = 0;

/* Used by rec_write() */
static char	 errstr[128];

/* Helper function for the rec_write*() functions */
static const char *rec_write_raw(const char *delim, const char *p, int ovector_valid, FILE *file) __attribute__((nonnull(1, 4)));

int
rec_open(int fd, pcre *re, pcre_extra *re_extra, size_t *memory_cache)
{
	sigset_t	 set, oset;
	struct stat	 sb;
	char		*template;
	const char	*prefix;
	void		*tmp;
	int		 capturecount, prefix_len, rfd, new_files_size;
#ifndef NDEBUG
	int		 rv;
#endif

	template = NULL;

	/* Find free entry in f[] */
	for (rfd = 0; rfd < f_last && f[rfd].offset != -1; rfd++);
	if (rfd == f_last) {
		if (f_last == f_size) {
			if (f_size < 4)
				new_files_size = 4;
			else if (f_size <= INT_MAX / 2)
				for (new_files_size = 1; new_files_size <= f_size; new_files_size *= 2);
			else
				new_files_size = INT_MAX;
			assert(new_files_size > f_size);
			/* LINTED converting new_files_size to size_t works */
			if ((tmp = realloc(f, new_files_size * sizeof(*f))) == NULL) {
				rfd = -1;
				goto err;
			}
			f = tmp;
			f_size = new_files_size;
		}
		assert(f_last < f_size);
		rfd = f_last++;
	}
	
	;; /* LINTED conversion of 4096 to size_t is fine */
	if ((f[rfd].buf_p = malloc(f[rfd].buf_size = 4096)) == NULL)
		goto err;

	f[rfd].fd = f[rfd].tmp = fd;
	f[rfd].buf_last = f[rfd].buf_first_read = f[rfd].buf_first_write = 0;
	f[rfd].offset = 0;
	f[rfd].memory_cache = memory_cache;
	if (pcre_fullinfo(f[rfd].re = re, f[rfd].re_extra = re_extra, PCRE_INFO_CAPTURECOUNT, &capturecount) != 0)
		goto err;

	assert(capturecount >= 0);
	/* Make sure that ovector is usable */
	/* LINTED converting capturecount to unsigned works */
	if (capturecount > INT_MAX / sizeof(*ovector) / 3 - 1) {
		errno = ENOMEM;
		goto err;
	}
	if ((capturecount + 1) * 3 > ovector_size) {
		/* LINTED converting capturecount to unsigned works, again */
		if ((tmp = realloc(ovector, (capturecount + 1) * 3 * sizeof(*ovector))) == NULL)
			goto err;

		ovector = tmp;
		ovector_size = (capturecount + 1) * 3;
	}

	/* If the file is not seek()able, open a temporary file */
#ifndef NDEBUG
	rv =
#endif
		fstat(f[rfd].fd, &sb);
	assert(rv == 0);
	/* XXX Is there a way to check for "seek works in a sane fashion"? */
	if (!S_ISREG(sb.st_mode)) {
		if ((prefix = getenv("TMPDIR")) == NULL || prefix[0] == '\0')
			prefix = "/tmp";
		;; /* LINTED conversion from strlen(prefix) to int is ok */
		for (prefix_len = MIN(strlen(prefix), INT_MAX); prefix_len > 0 && prefix[prefix_len - 1] == '/'; prefix_len--);
		if (asprintf(&template, "%.*s/randomize.XXXXXX", prefix_len, prefix) == -1)
			goto err;

		/*
		 * Create temporary file; block signals to make it more likely
		 * that unlink() succeeds.
		 */
		sigfillset(&set);
		sigprocmask(SIG_BLOCK, &set, &oset);
		f[rfd].tmp = mkstemp(template);
		if (f[rfd].tmp != -1)
			unlink(template);
		sigprocmask(SIG_SETMASK, &oset, NULL);

		if (f[rfd].tmp == -1)
			goto err;
	}

	free(template);

	return rfd;

err:
	if (rfd != -1)
		rec_close(rfd);
	free(template);

	return -1;
}

int
rec_close(int rfd)
{
	int		 rv, rv2, rv_errno;
	void		*tmp;

	rv = 0;
	rv_errno = 0;

	if (f[rfd].tmp != -1 && f[rfd].tmp != f[rfd].fd)
		if ((rv = close(f[rfd].tmp)) != 0)
			rv_errno = errno;

	if ((rv2 = close(f[rfd].fd)) != 0 && rv == 0) {
		rv = rv2;
		rv_errno = errno;
	}

	free(f[rfd].buf_p);
	f[rfd].offset = -1;

	assert(f_last > 0);
	if (rfd == f_last - 1) {
		for ( ; rfd >= 0 && f[rfd].offset == -1; rfd--);
		/* Note that rfd points one *past* the last valid rfd */
		rfd++;
		assert(rfd >= 0);
		if (rfd < f_size / 4) {
			/* Shrink f[] */
			/* LINTED converting rfd to size_t works */
			if ((tmp = realloc(f, rfd * sizeof(*f))) != NULL) {
				f = tmp;
				f_last = f_size = rfd;
			}
		}
	}

	if (f_size == 0) {
		/* Clean up */
		free(f);
		f = NULL;
		assert(f_last == 0);
		free(ovector);
		ovector = NULL;
		ovector_size = 0;
		free(w_buf);
		w_buf = NULL;
		w_buf_size = 0;
	}

	errno = rv_errno;
	return rv;
}

void
rec_assert_released(void)
{
	assert(f_size == 0);
	assert(f_last == 0);
	assert(ovector == NULL);
	assert(ovector_size == 0);
	assert(w_buf == NULL);
	assert(w_buf_size == 0);
}

int
rec_next(int rfd, struct rec *rec)
{
	void		*tmp;
	ssize_t		 nbytes;
	int		 rv, eof, rec_len;
#ifndef NDEBUG
	int		 capturecount;

	assert(pcre_fullinfo(f[rfd].re, f[rfd].re_extra, PCRE_INFO_CAPTURECOUNT, &capturecount) == 0);
	/* LINTED converting capturecount to unsigned works */
	assert(capturecount <= SIZE_MAX / sizeof(*ovector) / 3 - 1);
	assert((capturecount + 1) * 3 <= ovector_size);
#endif

	/*
	 * Look for regular expression.
	 *
	 * Read the documentation for f[rfd].buf_p before trying to understand
	 * this code.
	 */
	eof = 0;
	while ((rv = pcre_exec(f[rfd].re, f[rfd].re_extra, f[rfd].buf_p, f[rfd].buf_last - f[rfd].buf_first_read, f[rfd].buf_first_read, eof ? 0 : PCRE_NOTEOL, ovector, ovector_size)) < 0) {
		if (rv != PCRE_ERROR_NOMATCH) {
			errno = EINVAL;
			goto err;
		}

		if (eof) {
			if (f[rfd].buf_first_read < f[rfd].buf_last) {
				/* Unterminated final record */
				ovector[0] = f[rfd].buf_first_read;
				ovector[1] = f[rfd].buf_last;
				break;
			}

			/*
			 * All data processed - everything should have been
			 * flushed to disk. Notify the caller of EOF.
			 */
			assert(f[rfd].buf_first_read == 0);
			assert(f[rfd].buf_last == 0);

			errno = 0;
			goto err;
		}

		/*
		 * Get more data
		 */
		assert(f[rfd].buf_first_write >= 0);
		assert(f[rfd].buf_first_read >= f[rfd].buf_first_write);
		assert(f[rfd].buf_last >= f[rfd].buf_first_read);
		assert(f[rfd].buf_size >= f[rfd].buf_last);
		assert(INT_MAX >= f[rfd].buf_size);
		if (f[rfd].tmp != f[rfd].fd) {
			/* Flush processed data to disk */
			/* LINTED converting (f[rfd].buf_first_read - i) to unsigned works */
			for (nbytes = 0;
			     f[rfd].buf_first_write < f[rfd].buf_first_read;
			     nbytes = write(f[rfd].tmp, &f[rfd].buf_p[f[rfd].buf_first_write], f[rfd].buf_first_read - f[rfd].buf_first_write)) {
				if (nbytes == -1)
					goto err;

				assert(nbytes >= 0);
				/* LINTED truncating nbytes works, since nbytes <= f[rfd].buf_first_read <= INT_MAX */
				f[rfd].buf_first_write += nbytes;
			}
			assert(f[rfd].buf_first_write == f[rfd].buf_first_read);
		} else
			assert(f[rfd].buf_first_write == 0);

		if (f[rfd].buf_size - (f[rfd].buf_last - f[rfd].buf_first_read) >= MAX(f[rfd].buf_size / 4, BUFSIZ)) {
			/* Just move unprocessed data to front */
			/* LINTED f[rfd].buf_last - f[rfd].buf_first_read >= 0, so can be converted to size_t */
			bcopy(&f[rfd].buf_p[f[rfd].buf_first_read], f[rfd].buf_p, f[rfd].buf_last - f[rfd].buf_first_read);
		} else {
			/* Enlarge buffer */
			if (f[rfd].buf_size > INT_MAX / 2) {
				errno = ENOMEM;
				goto err;
			}

			;; /* LINTED f[rfd].buf_size * 2 fits in INT_MAX per above */
			if ((tmp = malloc(f[rfd].buf_size * 2)) == NULL)
				goto err;

			;; /* LINTED f[rfd].buf_last - f[rfd].buf_first_read >= 0 as above */
			memcpy(tmp, &f[rfd].buf_p[f[rfd].buf_first_read], f[rfd].buf_last - f[rfd].buf_first_read);

			free(f[rfd].buf_p);
			f[rfd].buf_p = tmp;
			f[rfd].buf_size *= 2;
		}
		f[rfd].buf_last -= f[rfd].buf_first_read;
		f[rfd].buf_first_write = f[rfd].buf_first_read = 0;
		assert(f[rfd].buf_size > f[rfd].buf_last);

		/* Read additional data */
		/* LINTED f[rfd].buf_last - f[rfd].buf_first_read >= 0 as above */
		if ((nbytes = read(f[rfd].fd, &f[rfd].buf_p[f[rfd].buf_last], f[rfd].buf_size - f[rfd].buf_last)) == -1)
			goto err;
		if (nbytes == 0)
			eof = 1;
		;; /* LINTED nbytes <= f[rfd].buf_size <= INT_MAX, and no overflow can happen here */
		f[rfd].buf_last += nbytes;
	}

	assert(ovector[0] >= f[rfd].buf_first_read);
	assert(ovector[1] >= ovector[0]);
	assert(f[rfd].buf_last >= ovector[1]);

	rec_len = ovector[1] - f[rfd].buf_first_read;
	if (rec != NULL) {
		rec->internal_only.len = rec_len;

		if (!eof)
			rec->internal_only.f_idx = rfd;
		else {
			if (rfd == 0)
				rec->internal_only.f_idx = INT_MIN;
			else
				rec->internal_only.f_idx = -rfd;

			assert(REC_IS_LAST(rec));
		}
		assert(&REC_F(rec) == &f[rfd]);

		;; /* LINTED converting rec_len to unsigned works fine */
		if (f[rfd].fd != f[rfd].tmp &&
		    f[rfd].buf_first_read == f[rfd].buf_first_write &&
		    (*f[rfd].memory_cache >= REC_ESTIMATED_MEMORY_USE(rec) &&
		     (rec->internal_only.loc.p = malloc(rec_len)) != NULL)) {
			/* Keep record in memory */
			/* LINTED converting REC_LEN(rec) to size_t works */
			memcpy(rec->internal_only.loc.p, &f[rfd].buf_p[f[rfd].buf_first_read], REC_LEN(rec));
			/* Don't write it to disk */
			f[rfd].buf_first_write += rec_len;
			/* LINTED converting REC_LEN(rec) to unsigned still works fine */
			/* Mark as in-memory record */
			rec->internal_only.len = -rec->internal_only.len;
			assert(!REC_IS_OFFSET(rec));
			/* LINTED converting REC_EST... to size_t works */
			*f[rfd].memory_cache -= REC_ESTIMATED_MEMORY_USE(rec);
		} else {
			rec->internal_only.loc.offset = f[rfd].offset;
			assert(REC_IS_OFFSET(rec));
			f[rfd].offset += rec_len;
		}
	} else {
		if (f[rfd].fd != f[rfd].tmp &&
		    f[rfd].buf_first_read == f[rfd].buf_first_write) {
			/* Don't write it to disk */
			/* XXX We'd prefer *never* writing to disk */
			f[rfd].buf_first_write += rec_len;
		} else
			f[rfd].offset += rec_len;
	}

	assert(f[rfd].buf_first_read + rec_len == ovector[1]);
	f[rfd].buf_first_read = ovector[1];
	assert(f[rfd].buf_first_write <= f[rfd].buf_first_read);

	return 0;

err:
	return -1;
}

const char *
rec_write(const struct rec *rec, const char *delim, FILE *file)
{
	const char	*p;
	void		*tmp;
	int		 i, ovector_valid, nbytes;
	size_t		 new_len;
#ifndef NDEBUG
	int		 capturecount;

	assert(pcre_fullinfo(REC_F(rec).re, REC_F(rec).re_extra, PCRE_INFO_CAPTURECOUNT, &capturecount) == 0);
	/* LINTED capturecount is nonnegative, so comparing with unsigned works */
	assert(capturecount <= SIZE_MAX / sizeof(*ovector) / 3 - 1);
	assert((capturecount + 1) * 3 <= ovector_size);
#endif

	if (!REC_IS_OFFSET(rec))
		/* Already in memory */
		p = REC_P(rec);
	else {
		/* Read into w_buf */
		/* LINTED converting REC_LEN(rec) to unsigned works fine */
		if (w_buf_size < REC_LEN(rec)) {
			/* Enlarge w_buf */
			/* LINTED converting REC_LEN(rec) to unsigned still works fine */
			for (new_len = w_buf_size != 0 ? w_buf_size : BUFSIZ;
			     new_len < REC_LEN(rec);
			     new_len *= 2);
			if ((tmp = realloc(w_buf, new_len)) == NULL) {
				snprintf(errstr, sizeof(errstr), "Failed to allocate buffer space: %s", strerror(errno));
				goto err;
			}

			w_buf = tmp;
			w_buf_size = new_len;
		}

		nbytes = 0;
		/* LINTED everything is limited to an int, so no problems here */
		for (i = 0; i < REC_LEN(rec); nbytes = pread(REC_F(rec).tmp, &w_buf[i], REC_LEN(rec) - i, REC_OFFSET(rec) + i)) {
			if (nbytes == -1) {
				snprintf(errstr, sizeof(errstr), "Failed to read record from file: %s", strerror(errno));
				goto err;
			}

			assert(nbytes >= 0);
			i += nbytes;
		}
		assert(i == REC_LEN(rec));

		p = w_buf;
	}

	/*
	 * We have REC_LEN(rec) bytes of data starting at p.
	 *
	 * Re-run the regular expression to get matches etc.
	 */
	if ((ovector_valid = pcre_exec(REC_F(rec).re, REC_F(rec).re_extra, p, REC_LEN(rec), 0, REC_IS_LAST(rec) ? 0 : PCRE_NOTEOL, ovector, ovector_size)) < 0) {
		/* Unterminated final record */
		assert(ovector_valid == PCRE_ERROR_NOMATCH);
		assert(REC_IS_LAST(rec));

		ovector_valid = 0;
		ovector[0] = ovector[1] = REC_LEN(rec);
	} else {
		assert(ovector_valid >= 1);
		assert(ovector[1] == REC_LEN(rec));
	}

	/* Output anything prior to match */
	;; /* LINTED ovector[0] is nonnegative */
	nbytes = fwrite(p, 1, ovector[0], file);
	/* LINTED as above */
	if (nbytes != ovector[0]) {
		snprintf(errstr, sizeof(errstr), "Failed to write output: %s", strerror(errno));
		goto err;
	}

	return rec_write_raw(delim, p, ovector_valid, file);

err:
	return errstr;
}

const char *
rec_write_str(const char *str, FILE *file)
{
	return rec_write_raw(str, NULL, 0, file);
}

static const char *
rec_write_raw(const char *delim, const char *p, int ovector_valid, FILE *file)
{
	int		 i, value, nbytes;
	enum {
		NORMAL,
		SEEN_BACKSLASH,
		SEEN_OCTAL1,
		SEEN_OCTAL2,
		SEEN_HEX0,
		SEEN_HEX1
	}		 state;
	char		 vis_buf[5];

	/*
	 * Output delim, handling backreferences and the like.
	 *
	 * This is mostly a finite state machine parsing delim.
	 */
	state = NORMAL;
	value = 0;
	if (strlen(delim) >= INT_MAX) {
		snprintf(errstr, sizeof(errstr), "Delimiting string too long: %zu", strlen(delim));
		goto err;
	}
	;; /* LINTED 0 <= i <= strlen(delim) < INT_MAX, so works */
	for (i = 0; i <= strlen(delim); i++) {
		switch (state) {
		case NORMAL:
			value = 0;
			switch (delim[i]) {
			case '\\':
				state = SEEN_BACKSLASH;
				break;
			case '&':
				/* Output match; note the label! */
				assert(value == 0);
output_match:
				assert(value >= 0);
				assert(value <= 9);
				if (value >= ovector_valid) {
					if (ovector_valid == 0) {
						if (value == 0)
							snprintf(errstr, sizeof(errstr), "The argument to -o contains &, but ");
						else
							snprintf(errstr, sizeof(errstr), "The argument to -o contains \\%d, but ", value);

						if (p != NULL)
							strlcat(errstr, "the last argument is not terminated", sizeof(errstr));
						else
							strlcat(errstr, "you passed -a", sizeof(errstr));

						goto err;
					} else {
						assert(value > 0);
						snprintf(errstr, sizeof(errstr), "Invalid backreference \\%d", value);
						goto err;
					}
				}
				;; /* LINTED the ovector[] - ovector[] expression is between 0 and INT_MAX, so ok */
				nbytes = fwrite(&p[ovector[2 * value]], 1, ovector[2 * value + 1] - ovector[2 * value], file);
				if (nbytes != ovector[2 * value + 1] - ovector[2 * value]) {
					snprintf(errstr, sizeof(errstr), "Failed to write match: %s", strerror(errno));
					goto err;
				}

				state = NORMAL;
				break;
			case '\0':
				/* Ignore */
				break;
			default:
				/* LINTED cast to unsigned somewhere in macro - should work */
				if (putc(delim[i], file) == EOF)
					goto err_char;
				break;
			}
			break;
		case SEEN_BACKSLASH:
			value = 0;
			switch (delim[i]) {
			case '&':
				if (putc('&', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case '\\':
				if (putc('\\', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'a':
				if (putc('\a', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'b':
				if (putc('\b', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'f':
				if (putc('\f', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'n':
				if (putc('\n', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'r':
				if (putc('\r', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 't':
				if (putc('\t', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'v':
				if (putc('\v', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'x':
				value = 0;
				state = SEEN_HEX0;
				break;
			case '0': /* FALLTHROUGH */
			case '1': /* FALLTHROUGH */
			case '2': /* FALLTHROUGH */
			case '3': /* FALLTHROUGH */
			case '4': /* FALLTHROUGH */
			case '5': /* FALLTHROUGH */
			case '6': /* FALLTHROUGH */
			case '7':
				/* Note: may also be a backreference, handled below */
				value = delim[i] - '0';
				state = SEEN_OCTAL1;
				break;
			case '8': /* FALLTHROUGH */
			case '9':
				value = delim[i] - '0';
				goto output_match;
			default:
				vis(vis_buf, delim[i], VIS_CSTYLE | VIS_NOSLASH, ':');
				snprintf(errstr, sizeof(errstr), "Invalid escape sequence \\%s: reserved for future use", vis_buf);
				goto err;
			}
			break;
		case SEEN_HEX0: /* FALLTHROUGH */
		case SEEN_HEX1:
			if (isxdigit(delim[i]))
				value = 16 * value + delim[i] - (isdigit(delim[i]) ? '0' :
								 isupper(delim[i]) ? 'A' : 'a');
			else if (state == SEEN_HEX1) {
				/*
				 * Not a hex digit - print value and process
				 * delim[i] again
				 */
				i--;
			} else {
				vis(vis_buf, delim[i], VIS_CSTYLE, ':');
				snprintf(errstr, sizeof(errstr), "Invalid escape sequence \\x%s: expected a hex digit", vis_buf);
				goto err;
			}

			if (state == SEEN_HEX0)
				state = SEEN_HEX1;
			else {
				/* LINTED cast to unsigned char somewhere in macro - should work */
				if (putc(value, file) == EOF)
					goto err_char;

				value = 0;
				state = NORMAL;
			}
			break;
		case SEEN_OCTAL1: /* FALLTHROUGH */
		case SEEN_OCTAL2:
			if (isdigit(delim[i]) && delim[i] != '8' && delim[i] != '9')
				value = 8 * value + delim[i] - '0';
			else if (state == SEEN_OCTAL1 && value != 0) {
				/*
				 * Not an octal digit - print *backreference*
				 * and process delim[i] again.
				 */
				i--;
				goto output_match;
			} else {
				/*
				 * Not an octal digit - print value and process
				 * delim[i] again.
				 */
				i--;
			}

			if (state == SEEN_OCTAL1)
				state = SEEN_OCTAL2;
			else {
				/* LINTED cast to unsigned char somewhere in macro - should work */
				if (putc(value, file) == EOF)
					goto err_char;

				value = 0;
				state = NORMAL;
			}
			break;
		}
	}

	/* LINTED works per check above loop */
	assert(i == strlen(delim) + 1);

	return NULL;

err_char:
	snprintf(errstr, sizeof(errstr), "Failed to write character: %s", strerror(errno));

err:
	return errstr;
}

void
rec_free(struct rec *rec)
{
	if (rec != NULL && !REC_IS_OFFSET(rec)) {
		/* LINTED converting REC_EST... to size_t works */
		*REC_F(rec).memory_cache += REC_ESTIMATED_MEMORY_USE(rec);
		free(rec->internal_only.loc.p);
	}
}
