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

#include <errno.h>
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
 *
 * That substitution covers everything left, so it is made once per gap --
 * see the COARSE return in diff_buffers().
 */
#define DELTA_MAX (1u << 18)

/* Bytes read at a time when looking for the ends two spans share. */
#define TRIM_CHUNK (64 * 1024)

/* How many leading bytes of the two spans are equal, or `limit`. */
int common_prefix(int fd_a, uint64_t a, int fd_b, uint64_t b, uint64_t limit,
		  uint64_t *out)
{
	unsigned char ba[TRIM_CHUNK], bb[TRIM_CHUNK];
	uint64_t k = 0;

	while (k < limit) {
		uint64_t want = limit - k < TRIM_CHUNK ? limit - k : TRIM_CHUNK;
		uint64_t i;

		if (pread_full(fd_a, ba, want, a + k) < 0 ||
		    pread_full(fd_b, bb, want, b + k) < 0) {
			/* A bad block ends the trim where it stands.  The
			 * rest stays in the region, gets compared, and is
			 * reported as unreadable if it cannot be read. */
			if (errno == EIO) {
				*out = k;
				return 0;
			}
			return -1;
		}
		if (memcmp(ba, bb, want) == 0) {
			k += want;
			continue;
		}
		for (i = 0; i < want; i++) {
			if (ba[i] != bb[i]) {
				*out = k + i;
				return 0;
			}
		}
	}
	*out = limit;
	return 0;
}

/* How many trailing bytes of the two spans are equal, or `limit`. */
int common_suffix(int fd_a, uint64_t a_end, int fd_b, uint64_t b_end,
		  uint64_t limit, uint64_t *out)
{
	unsigned char ba[TRIM_CHUNK], bb[TRIM_CHUNK];
	uint64_t k = 0;

	while (k < limit) {
		uint64_t want = limit - k < TRIM_CHUNK ? limit - k : TRIM_CHUNK;
		uint64_t i;

		if (pread_full(fd_a, ba, want, a_end - k - want) < 0 ||
		    pread_full(fd_b, bb, want, b_end - k - want) < 0) {
			if (errno == EIO) {	/* as in common_prefix */
				*out = k;
				return 0;
			}
			return -1;
		}
		if (memcmp(ba, bb, want) == 0) {
			k += want;
			continue;
		}
		for (i = want; i-- > 0;) {
			if (ba[i] != bb[i]) {
				*out = k + (want - 1 - i);
				return 0;
			}
		}
	}
	*out = limit;
	return 0;
}

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

/*
 * Adjacent deltas that touch in both files are really one bigger delta --
 * unless they are different in kind.  A region that was compared and a region
 * that could not be read are both differences, but they are not the same
 * claim, and merging them would make the report say more than was
 * established.
 */
static int deltas_push_flags(struct deltalist *dl, uint64_t a_off,
			     uint64_t a_len, uint64_t b_off, uint64_t b_len,
			     unsigned int unreadable)
{
	if (dl->n) {
		struct delta *last = &dl->v[dl->n - 1];

		if (last->a_off + last->a_len == a_off &&
		    last->b_off + last->b_len == b_off &&
		    last->unreadable == unreadable) {
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
		.unreadable = unreadable,
	};
	return 0;
}

static int deltas_push(struct deltalist *dl, uint64_t a_off, uint64_t a_len,
		       uint64_t b_off, uint64_t b_len)
{
	return deltas_push_flags(dl, a_off, a_len, b_off, b_len, UNREAD_NONE);
}

/* A range that could not be read, so equality cannot be proven. */
static int deltas_push_unreadable(struct deltalist *dl, uint64_t a_off,
				  uint64_t a_len, uint64_t b_off, uint64_t b_len,
				  unsigned int unreadable)
{
	return deltas_push_flags(dl, a_off, a_len, b_off, b_len, unreadable);
}

/* Returned by diff_buffers when the list filled up and the rest of the span
 * has been reported as one coarse substitution.  Nothing of that span is left
 * to refine, so the caller can stop reading it. */
#define DIFF_COARSE 2

/*
 * Record the runs that differ between two buffers already in memory.
 * `p` is the absolute offset the buffers start at and `n` their length;
 * `left` is how much of the span remains, which is what a delta that has to
 * give up (deltas_full) covers.  Returns 0, 1 having stopped because `stop`
 * was set and a difference was recorded, DIFF_COARSE having reported the rest
 * of the span coarsely, or -1 on error.
 */
static int diff_buffers(const unsigned char *ba, const unsigned char *bb,
			uint64_t p, size_t n, struct deltalist *out, bool stop,
			uint64_t left)
{
	size_t i = 0;

	if (memcmp(ba, bb, n) == 0)
		return 0;

	if (deltas_full(out)) {
		/*
		 * The coarse substitution covers everything from here to the end
		 * of the span, so it may only be made once per span.  Making it
		 * per read chunk instead reported the same bytes over and over:
		 * the regions overlapped, and on a single-extent file their
		 * lengths summed to more bytes than the file has.
		 *
		 * `stop` cannot be set here: with it the walk ends at the first
		 * difference it proves, so the list never fills.
		 */
		if (deltas_push(out, p, left, p, left) < 0)
			return -1;
		return DIFF_COARSE;
	}

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
	return 0;
}

/*
 * Compare one chunk whose bulk read failed, a block at a time.
 *
 * A read that returns EIO means the storage underneath is damaged, and a
 * block that cannot be read cannot be shown to be equal -- so it is recorded
 * as a difference, which is the direction every other ambiguity in this tool
 * resolves towards.  Doing it a block at a time is the point of being here:
 * one bad block costs one block of report rather than the whole chunk, and
 * the readable blocks around it are still compared rather than assumed.
 */
#define EIO_BLOCK 4096

static int compare_chunk_with_holes(int fd_a, int fd_b, uint64_t p, size_t n,
				    uint64_t left, unsigned char *ba,
				    unsigned char *bb, struct deltalist *out,
				    bool stop)
{
	size_t k;

	for (k = 0; k < n; k += EIO_BLOCK) {
		size_t m = n - k < EIO_BLOCK ? n - k : EIO_BLOCK;
		int ea = pread_full(fd_a, ba, m, p + k) < 0 ? errno : 0;
		int eb = pread_full(fd_b, bb, m, p + k) < 0 ? errno : 0;
		unsigned int unread = UNREAD_NONE;
		int rc;

		if ((ea && ea != EIO) || (eb && eb != EIO))
			return -1;	/* a real error, not a bad block */
		if (ea)
			unread |= UNREAD_A;
		if (eb)
			unread |= UNREAD_B;
		if (unread) {
			if (deltas_push_unreadable(out, p + k, m, p + k, m,
						  unread) < 0)
				return -1;
			if (stop)
				return 1;
			continue;
		}

		/*
		 * `left` is what remains of the *span*, not of this block:
		 * once the delta list is full, the coarse delta it falls back
		 * to has to cover everything still to come.  Passing the block
		 * length here would turn one coarse delta per megabyte into
		 * one per 4 KB block.
		 */
		rc = diff_buffers(ba, bb, p + k, m, out, stop, left - k);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int compare_range(int fd_a, int fd_b, uint64_t off, uint64_t len,
			 unsigned char *ba, unsigned char *bb, size_t cap,
			 struct deltalist *out, bool stop)
{
	uint64_t p = off;

	while (p < off + len) {
		uint64_t left = off + len - p;
		size_t n = left < cap ? (size_t)left : cap;
		int rc;

		if (pread_full(fd_a, ba, n, p) < 0 ||
		    pread_full(fd_b, bb, n, p) < 0) {
			if (errno != EIO)
				return -1;
			rc = compare_chunk_with_holes(fd_a, fd_b, p, n, left,
						      ba, bb, out, stop);
		} else {
			rc = diff_buffers(ba, bb, p, n, out, stop, left);
		}
		if (rc != 0)
			return rc;

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

		/*
		 * Where the zeros on each side stop, which is what makes the
		 * settled answer settled.  An extent ends at its own end; a hole
		 * ends where the *next* extent begins.  Cutting only at extrinsic
		 * ends leaves a hole running to the end of the range, and then data
		 * that follows the hole is inside a piece whose answer was already
		 * decided to be zeros -- which would report two different files as
		 * identical.  That is the one error this tool must never make.
		 */
		{
			uint64_t ea_end = ea ? ea->off + ea->len
					     : extmap_next_start(ma, p);
			uint64_t eb_end = eb ? eb->off + eb->len
					     : extmap_next_start(mb, p);

			if (ea_end < next)
				next = ea_end;
			if (eb_end < next)
				next = eb_end;
		}
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

			rc = range_is_zero(fd, m, p, next - p, scratch, &z);
			if (rc < 0)
				return -1;
			if (rc > 0) {
				/* The data side could not be read, so whether
				 * it is zeros is unknowable -- a difference. */
				if (deltas_push_unreadable(out, p, next - p,
							  p, next - p,
							  za ? UNREAD_B
							     : UNREAD_A) < 0)
					return -1;
				if (stop)
					return 1;
			} else if (!z) {
				if (deltas_push(out, p, next - p, p, next - p) < 0)
					return -1;
				if (stop)
					return 1;
			}
		} else {
			rc = compare_range(fd_a, fd_b, p, next - p, ba, bb, cap,
					   out, stop);
			if (rc < 0)
				return rc;
			if (rc == 1)
				return 1;	/* one difference is enough */
			/*
			 * rc == DIFF_COARSE: what is left of this piece has
			 * been reported as one substitution, so there is nothing
			 * more to refine here.  The pieces after it are still
			 * separate questions and still get asked -- everything
			 * not proven equal has to end up in some delta.
			 */
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
	 * the two sides, so the spans cannot be lined up and compared as a
	 * whole.  What they do share can still be found, though: a file that
	 * gained or lost bytes in the middle is identical before and after
	 * them, and those ends need not be reported as replaced.
	 *
	 * This matters most for binary output, where there is no line diff
	 * afterwards to refine anything.  Without it, a six-byte change to a
	 * 35 MB file is reported as 35 MB of differing region -- true, and
	 * useless in the same way the text answer in the handoff notes' §9
	 * was, and for the same reason: a coarse answer is indistinguishable
	 * from no answer at all.
	 *
	 * A caller that only wants to know whether the files differ is given
	 * the whole span instead.  Finding the common ends costs a read of
	 * everything up to the first difference, and -q does not need to
	 * know where the difference is.
	 */
	if (!stop) {
		uint64_t limit = alen < blen ? alen : blen;
		uint64_t kp = 0, ks = 0;

		if (common_prefix(fd_a, as, fd_b, bs, limit, &kp) < 0)
			return -1;
		if (common_suffix(fd_a, ae, fd_b, be, limit - kp, &ks) < 0)
			return -1;
		/*
		 * Both trimmed lengths zero means the two spans are the same
		 * bytes at different offsets: the alignment moved, the content
		 * did not.  Reporting that as a difference would name bytes
		 * that were just found equal, and the shift is already reported
		 * by the gap that opened it -- a chain only comes to sit at a
		 * new offset because some gap changed the offset delta, and
		 * that gap's two spans have different lengths, so its trimmed
		 * lengths cannot both vanish and it always reports.
		 *
		 * Both zero is only reachable when the lengths are equal, since
		 * kp + ks is capped by the shorter span, so the test is exact.
		 */
		if (alen - kp - ks == 0 && blen - kp - ks == 0)
			return 0;
		if (deltas_push(out, as + kp, alen - kp - ks,
				bs + kp, blen - kp - ks) < 0)
			return -1;
		return 0;
	}

	/*
	 * Reached with `stop` for any two-sided gap that is not at identical
	 * offsets with equal lengths, including the one above: -q wants the
	 * verdict, not the report, and finding the common ends costs a read of
	 * everything up to the first difference.  A gap that has moved offsets
	 * means some gap changed the delta, so the two files differ either way
	 * and the push is right.
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
