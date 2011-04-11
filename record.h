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

/*
 * A simple API for treating a file descriptor as a stream of records. Requires
 * <stdio.h> and <pcre.h> (and -lpcre).
 */

#ifndef __GNUC__
#ifndef __attribute__
#define __attribute__(x) /* Not supported by non-GCC compilers */
#endif
#endif

/* A record. For internal use only! */
struct rec {
	struct {
		union {
			off_t	 offset;
			void	*p;
		} loc;
		int		 len, f_idx;
	} internal_only;
};

/*
 * Open a file for reading records (from the current file position).
 *
 * Up to *memory_cache bytes of records will be kept in memory (instead of
 * spooled to disk) by rec_next(); malloc overhead will be estimated, but any
 * buffers used by rec_fopen() will not be tracked.
 *
 * For default_delim, see rec_write().
 *
 * Returns the lowest unused record file descriptor ("rfd") on success, and
 * increases the internal reference count of re by 1 (if not maximal; see
 * pcre_refcount()). Otherwise, returns -1 and sets errno as for malloc(3) or
 * mkstemp(3).
 */
int rec_open(int fd, pcre *re, pcre_extra *re_extra, const char *default_delim, size_t *memory_cache) __attribute__((nonnull(2, 3, 5)));

/*
 * Get next record. If rec is NULL, the data is discarded instead.
 *
 * Returns 0 on success and initializes rec (if non-NULL); otherwise, returns
 * -1 and sets errno as for read(2), write(2), or malloc(3), or to 0 on EOF, or
 * to EINVAL if there was an error while processing the regular expression.
 * Unless rec_next returns 0, rec is unchanged.
 */
int rec_next(int rfd, struct rec *rec);

/*
 * Write record to FILE *.
 *
 * delim is an output template that can contain escape sequences, as described
 * in the man page (for the -o option). If delim is NULL, default_delim from
 * rec_open() will be used instead; in this case, default_delim must have been
 * a valid pointer.
 *
 * Returns NULL on success; otherwise, returns an error message and sets errno
 * as for malloc(3), putc(3), fwrite(3), or read(2). Where appropriate,
 * strerror(errno) is already incorporated in the error message.
 */
const char *rec_write(const struct rec *rec, const char *delim, FILE *file) __attribute__((nonnull(1, 3)));

/*
 * Write a string to FILE *, processing it as the 'delim' argument above.
 *
 * The return values are as above. errno may be set as for putc(3) or
 * fwrite(2).
 */
const char *rec_write_str(const char *str, FILE *file) __attribute__((nonnull(1, 2)));

/*
 * Free all resources associated with rec (but not rec itself).
 *
 * This decreases the internal reference count of the re argument used to open
 * rec by 1 (if not maximal) and pcre_free()s re and re_extra if the reference
 * count reaches zero. It is an error to call rec_free() while the reference
 * count of re is 0.
 *
 * Does nothing if rec is NULL.
 */
void rec_free(struct rec *rec);

/*
 * Free all resources allocated by rec_open(), including the file descriptor
 * passed to rec_open(). This does not free() the memory_cache argument to
 * rec_open().
 *
 * It is an error to call any rec_* function on a struct rec associated with
 * this rfd afterwards.
 *
 * Returns 0 on succcess; otherwise, returns -1 and sets errno as for close(2).
 */
int rec_close(int rfd);

/*
 * assert() that all rec_* resources have been deallocated, for debugging only.
 */
#ifndef NDEBUG
void rec_assert_released(void);
#else
#define rec_assert_released() ((void) 0)
#endif
