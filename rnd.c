/*
 * Copyright (c) 2011 Joachim Schipper <joachim@joachimschipper.nl>
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

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>

#include "rnd.h"

uintmax_t
random_uniform(const uintmax_t upper_bound)
{
	uintmax_t		 r, r_max, reroll_at;
	size_t			 i;
#ifndef HAVE_ARC4RANDOM
	int			 extra_bits;
#endif

	r_max = UINTMAX_MAX;

	do {
		/* Optionally reduce r_max; set r to a random value in 0..r_max */
#ifdef HAVE_ARC4RANDOM
		if (upper_bound > UINT32_MAX) {
			r = 0;
			for (i = 0; i < sizeof(r) / sizeof(uint32_t); i++)
				r += (uintmax_t)arc4random() << (i * 32);
		} else
			return arc4random_uniform(upper_bound);
#else
		if (upper_bound > INT32_MAX) {
			assert(sizeof(r) / sizeof(uint32_t) < 31);
			extra_bits = random();

			r = 0;
			for (i = 0; i < sizeof(r) / sizeof(uint32_t); i++) {
				r += (uintmax_t)random() << i * 32;
				/* Fill in 32nd bit from extra_bits */
				r += (extra_bits & (uintmax_t)1 << i) >> i <<
				    i * 32 << 31;
			}
			r += (uintmax_t) random() << i * 32;
		} else if (upper_bound == 0) {
			/* Prevent % 0 below */
			return 0;
		} else {
			r_max = INT32_MAX;
			r = random();
		}
#endif

		assert(r < r_max);

		/* If r < reroll_at, we can just return the modulus */
		reroll_at = r_max - ((r_max % upper_bound + 1) % upper_bound);
		assert(reroll_at % upper_bound == upper_bound - 1);
		assert(upper_bound <= reroll_at && reroll_at <= r_max);
	} while (r >= reroll_at);

	return r % upper_bound;
}

#ifdef TESTSUITE
#include <limits.h>
#include <stdio.h>

#define N_OUTPUT 10

int main(void);

int
main(void)
{
	uintmax_t	 i;
	int		 j;

	for (i = 0; i < 16; i++) {
		/* gcc complains about this; it's wrong */
		printf("%02ju:", i);
		for (j = 0; j < N_OUTPUT; j++)
			printf(" %02jd", (intmax_t)random_uniform(i));
		printf("\n");
	}

	if (sizeof(uintmax_t) * CHAR_BIT > 32) {
		for (i = (uintmax_t)UINT32_MAX + 1;
		    i < (uintmax_t)(1 << 16) * UINT32_MAX;
		    i *= 2) {
			printf("%012jx:", (uintmax_t)i);
			for (j = 0; j < N_OUTPUT; j++)
				printf(" %012jx", (uintmax_t)random_uniform(i));
			printf("\n");
		}
	} else
		printf("sizeof(uintmax_t) = %zu\n", sizeof(uintmax_t));

	return 0;
}
#endif
