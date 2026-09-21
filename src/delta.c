/*
 * delta.c - compare the parts that could not be proven equal.
 *
 * Anchors account for everything the filesystem vouched for; whatever is
 * left over is a gap, and a gap is the only thing this file ever reads.  Even
 * inside a gap there is usually nothing to read: a hole reads as zeros, an
 * unwritten extent reads as zeros, and two holes obviously agree.  Only when
 * both sides really hold data does anything get read, and even then a hole on
 * one side is settled by testing the other side for zeros rather than by
 * comparing two buffers.
 */
#include "cowdiff.h"

#include <stdlib.h>
#include <string.h>

/* Bytes compared per read.  Big enough to keep syscalls rare, small enough
 * that memory does not scale with the size of the files. */
#define CHUNK (1u << 20)

/* Differing runs closer together than this are reported as one range.  A
 * changed field in the middle of a struct should read as one edit, not as
 * several dozen. */
#define MERGE_GAP 16

/*
 * Refuse to refine forever.  Two unrelated large files differ almost
 * everywhere, and one delta per run of differing bytes would be millions of
 * entries saying very little; past this point the rest of the gap is reported
 * as a single substitution, which is still true, just coarser.
 */
#define DELTA_MAX (1u << 18)

void deltas_free(struct deltalist *dl)
{
	free(dl->v);
	dl->v = NULL;
	dl->n = 0;
	dl->cap = 0;
}

static bool deltas_full(const struct deltalist *dl)
{
	return dl->n >= DELTA_MAX;
}

/* Adjacent deltas that touch in both files are really one bigger delta. */
static int deltas_push(struct deltalist *dl, uint64_t a_off, uint64_t a_len,
		       uint64_t b_off, uint64_t b_len)
{
	if (dl->n) {
		struct delta *last = &dl->v[dl->n - 1];

		if (last->a_off + last->a_len == a_off &&
		    last->b_off + last->b_len == b_off) {
			last->a_len += a_len;
			last->b_len += b_len;
			return 0;
		}
	}

	if (dl->n == dl->cap) {
		size_t ncap = dl->cap ? dl->cap * 2 : 128;
		struct delta *nv = realloc(dl->v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		dl->v = nv;
		dl->cap = ncap;
	}

	dl->v[dl->n++] = (struct delta){
		.a_off = a_off,
		.a_len = a_len,
		.b_off = b_off,
		.b_len = b_len,
	};
	return 0;
}

/*
 * Read both sides of an equal-length span and record the runs that differ.
 * This is the only place the tool reads two buffers and compares them.
 *
 * Returns 0 having walked the span, 1 having stopped because `stop` was set
 * and a difference had been recorded, or -1 on error.  Stopping is sound
 * because a difference that has been *compared* proves the files differ,
 * while the opposite verdict is the one that has to be earned.
 */
static int compare_range(int fd_a, int fd_b, uint64_t off, uint64_t len,
			 unsigned char *ba, unsigned char *bb, size_t cap,
			 struct deltalist *out, bool stop)
{
	uint64_t p = off;

	while (p < off + len) {
		uint64_t left = off + len - p;
		size_t n = left < cap ? (size_t)left : cap;
		size_t i;

		if (pread_full(fd_a, ba, n, p) < 0)
			return -1;
		if (pread_full(fd_b, bb, n, p) < 0)
			return -1;

		if (memcmp(ba, bb, n) == 0) {
			p += n;
			continue;
		}

		if (deltas_full(out)) {
			if (deltas_push(out, p, left, p, left) < 0)
				return -1;
			return stop ? 1 : 0;
		}

		i = 0;
		while (i < n) {
			size_t start, end;
			unsigned int gap = 0;

			while (i < n && ba[i] == bb[i])
				i++;
			if (i == n)
				break;

			start = i;
			end = i;
			while (i < n) {
				if (ba[i] != bb[i]) {
					i++;
					end = i;
					gap = 0;
				} else {
					if (++gap > MERGE_GAP)
						break;
					i++;
				}
			}

			if (deltas_push(out, p + start, end - start,
					p + start, end - start) < 0)
				return -1;
			if (stop)
				return 1;
		}

		p += n;
	}
	return 0;
}

/*
 * Walk an equal-length span, cutting it at every extent boundary of either
 * file so that each piece has one settled answer on each side.
 */
static int gap_equal_range(int fd_a, const struct extmap *ma,
			   int fd_b, const struct extmap *mb,
			   uint64_t off, uint64_t len,
			   unsigned char *ba, unsigned char *bb, size_t cap,
			   struct iobuf *scratch, struct deltalist *out,
			   bool stop)
{
	uint64_t p = off, end_all = off + len;
	int rc;

	while (p < end_all) {
		const struct ext *ea = extmap_at(ma, p);
		const struct ext *eb = extmap_at(mb, p);
		bool za = !ea || ea->zero;
		bool zb = !eb || eb->zero;
		uint64_t next = end_all;

		if (ea && ea->off + ea->len < next)
			next = ea->off + ea->len;
		if (eb && eb->off + eb->len < next)
			next = eb->off + eb->len;
		if (next <= p)
			next = end_all;

		if (za && zb) {
			/* Both read as zeros; nothing to do and nothing to read. */
		} else if (za || zb) {
			/*
			 * One side is certain zeros.  The other side agrees
			 * only if it is zeros too, so read it and find out --
			 * and note this is why "an extent here, a hole there"
			 * cannot be assumed to be a difference.
			 */
			int fd = za ? fd_b : fd_a;
			const struct extmap *m = za ? mb : ma;
			bool z;

			if (range_is_zero(fd, m, p, next - p, scratch, &z) < 0)
				return -1;
			if (!z) {
				if (deltas_push(out, p, next - p, p, next - p) < 0)
					return -1;
				if (stop)
					return 1;
			}
		} else {
			rc = compare_range(fd_a, fd_b, p, next - p, ba, bb, cap,
					   out, stop);
			if (rc != 0)
				return rc;
		}

		p = next;
	}
	return 0;
}

static int resolve_gap(int fd_a, const struct extmap *ma,
		       int fd_b, const struct extmap *mb,
		       uint64_t as, uint64_t ae, uint64_t bs, uint64_t be,
		       unsigned char *ba, unsigned char *bb, size_t cap,
		       struct iobuf *scratch, struct deltalist *out, bool stop)
{
	uint64_t alen = ae - as, blen = be - bs;

	if (alen == 0 && blen == 0)
		return 0;

	/* One side has nothing here: a pure insertion or deletion. */
	if (alen == 0) {
		if (deltas_push(out, as, 0, bs, blen) < 0)
			return -1;
		return stop ? 1 : 0;
	}
	if (blen == 0) {
		if (deltas_push(out, as, alen, bs, 0) < 0)
			return -1;
		return stop ? 1 : 0;
	}

	if (alen == blen && as == bs)
		return gap_equal_range(fd_a, ma, fd_b, mb, as, alen, ba, bb, cap,
				       scratch, out, stop);

	/*
	 * Different lengths, or content that sits at different offsets on
	 * the two sides.  Report the whole span as replaced; for text output
	 * this is what a line diff then refines, and for binary output it is
	 * already the honest answer.
	 */
	if (deltas_push(out, as, alen, bs, blen) < 0)
		return -1;
	return stop ? 1 : 0;
}

int deltas_find(int fd_a, const struct extmap *a, uint64_t size_a,
		int fd_b, const struct extmap *b, uint64_t size_b,
		const struct anchorlist *al, struct deltalist *out,
		bool stop_at_first)
{
	unsigned char *ba = NULL, *bb = NULL;
	struct iobuf scratch;
	uint64_t prev_a = 0, prev_b = 0;
	size_t k;
	int rc = -1;

	memset(&scratch, 0, sizeof scratch);
	out->v = NULL;
	out->n = 0;
	out->cap = 0;

	ba = malloc(CHUNK);
	bb = malloc(CHUNK);
	if (!ba || !bb)
		goto out;
	if (iobuf_init(&scratch, CHUNK) < 0)
		goto out;

	/*
	 * Everything not covered by an anchor is a gap.  There is one before
	 * the first anchor, one between each pair, and one after the last --
	 * which is also where the two files' tails meet when they are
	 * different lengths.
	 */
	for (k = 0; k <= al->n; k++) {
		uint64_t a_end = (k < al->n) ? al->v[k].a_off : size_a;
		uint64_t b_end = (k < al->n) ? al->v[k].b_off : size_b;
		int gr;

		gr = resolve_gap(fd_a, a, fd_b, b, prev_a, a_end, prev_b, b_end,
				 ba, bb, CHUNK, &scratch, out, stop_at_first);
		if (gr < 0)
			goto out;
		if (gr > 0) {
			/* One difference is all the caller asked for. */
			rc = 0;
			goto out;
		}

		if (k < al->n) {
			prev_a = al->v[k].a_off + al->v[k].len;
			prev_b = al->v[k].b_off + al->v[k].len;
		}
	}

	rc = 0;
out:
	free(ba);
	free(bb);
	iobuf_free(&scratch);
	if (rc < 0)
		deltas_free(out);
	return rc;
}
