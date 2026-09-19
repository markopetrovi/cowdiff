/*
 * linediff.c - turn byte ranges that differ into line ranges that differ.
 *
 * By the time anything gets here the byte-level work is done: these ranges
 * are known to differ, and they are usually small, because everything the
 * filesystem could vouch for was already taken out as anchors.  So the
 * common case is a handful of lines and gets an exact answer.
 *
 * For the case where no sharing exists at all -- two ordinary files that were
 * written independently -- the range is the whole file, and an exact O(n*m)
 * answer is out of reach.  That falls back to splitting the range at a line
 * that occurs exactly once on each side, which is what makes ordinary text
 * line up.  Failing even that -- a file where every line is the same leaves
 * nothing to split on -- the middle of the range is tried instead, and only
 * if the lines there are not equal is the range reported as wholly replaced.
 * That last is coarse but never wrong; it just used to be reached far more
 * often than it should have been.
 */
#include "cowdiff.h"

#include <stdlib.h>
#include <string.h>

/* Ranges up to this many lines on both sides are diffed exactly. */
#define SMALL 256

static uint64_t hash_line(const unsigned char *p, size_t len)
{
	uint64_t h = 1469598103934665603ULL;

	while (len--) {
		h ^= *p++;
		h *= 1099511628211ULL;
	}
	return h;
}

void lineset_free(struct lineset *ls)
{
	free(ls->buf);
	free(ls->v);
	memset(ls, 0, sizeof *ls);
}

int lineset_build(struct lineset *ls, int fd, uint64_t off, uint64_t len)
{
	size_t nlines = 0, i, k = 0, start = 0;

	memset(ls, 0, sizeof *ls);
	if (len == 0)
		return 0;

	ls->buf = malloc(len);
	if (!ls->buf)
		return -1;
	if (pread_full(fd, ls->buf, len, off) < 0) {
		lineset_free(ls);
		return -1;
	}
	ls->buflen = len;

	for (i = 0; i < len; i++)
		if (ls->buf[i] == '\n')
			nlines++;
	/* A trailing fragment with no newline is still a line -- and it is a
	 * *different* line from the same text with a newline, which is how
	 * diff reports a missing newline at end of file. */
	if (ls->buf[len - 1] != '\n')
		nlines++;

	ls->v = malloc((nlines ? nlines : 1) * sizeof *ls->v);
	if (!ls->v) {
		lineset_free(ls);
		return -1;
	}

	for (i = 0; i < len; i++) {
		if (ls->buf[i] != '\n')
			continue;
		ls->v[k++] = (struct lline){
			.boff = start,
			.len = (uint32_t)(i - start + 1),
			.hash = hash_line(ls->buf + start, i - start + 1),
		};
		start = i + 1;
	}
	if (start < len)
		ls->v[k++] = (struct lline){
			.boff = start,
			.len = (uint32_t)(len - start),
			.hash = hash_line(ls->buf + start, len - start),
		};

	ls->n = k;
	return 0;
}

static bool lline_eq(const struct lineset *a, size_t i,
		     const struct lineset *b, size_t j)
{
	const struct lline *x = &a->v[i], *y = &b->v[j];

	return x->len == y->len && x->hash == y->hash &&
	       memcmp(a->buf + x->boff, b->buf + y->boff, x->len) == 0;
}

/*
 * Equivalence classes.
 *
 * Every distinct line gets a number, and two lines are equal exactly when
 * their numbers are.  That matters because of what the search does with it:
 * the prefix and suffix trimming, the small-range table, and the anchor scan
 * all run over the whole input many times, and comparing class numbers makes
 * each of those comparisons an integer compare rather than a memcmp call.
 * Only assigning the classes costs a byte comparison, and that happens once.
 *
 * A class number is only ever handed out after the text has been compared,
 * so unlike a bare hash it cannot make two different lines look equal, and
 * two lines that collide still get separate classes.
 *
 * The assignment sorts; it does not hash.  A hash table large enough for the
 * lines of both files is probed once per line at a random address, and it is
 * far bigger than any cache: on the shape this code exists for -- two files
 * with nothing shared, so the whole of each becomes one range -- it is 400 MB
 * and costs a cache and TLB miss per line.  Sorting the same lines by hash
 * instead walks memory in order, and the only table left is small enough to
 * stay in cache.
 */

/*
 * One line, reduced to something sortable: a fingerprint of its hash and its
 * index in the lineset it came from.  Eight bytes, so that a sort of a few
 * million of them is a few tens of megabytes rather than hundreds.
 *
 * The fingerprint is the *high* half of the hash, because that is the half
 * FNV-1a mixes.  Its multiply carries low bits upward, while the lowest bit
 * of the finished value is only the parity of the bytes fed into it -- so the
 * bottom bits of this hash are the ones not to key on.
 *
 * Equal lines always share a fingerprint, so sorting gathers them together;
 * unequal lines may share one by accident, and grouping compares the text, so
 * a shared fingerprint costs a comparison rather than a wrong answer.
 */
struct lref {
	uint32_t fp;
	uint32_t idx;
};

#define CLASS_NONE ((uint32_t)-1)

/*
 * The classes, and what the search needs to ask about them.
 *
 * Built on first need, not up front.  A diff that the common-prefix and
 * common-suffix trimming resolves never reaches the anchor search at all, and
 * charging it a pass over the whole file plus a table sized to match would be
 * a tax on exactly the easy case.
 */
struct dstate {
	const struct lineset *a, *b;
	uint32_t *ca;		/* class of each line of A */
	uint32_t *cb;		/* class of each line of B */
	uint32_t *count_a;	/* occurrences of each class in A */
	uint32_t *count_b;
	uint32_t *first_b;	/* first line of B in each class */
	bool built;
	bool failed;
};

/*
 * Are these two lines the same?  Class numbers once they exist, bytes before
 * that -- which is what makes building them lazily worth doing, since the
 * trimming usually settles the diff before any class is ever needed.
 */
static bool lines_equal(const struct dstate *ds, const struct lineset *a,
			size_t i, const struct lineset *b, size_t j)
{
	return ds->built && !ds->failed ? ds->ca[i] == ds->cb[j]
					: lline_eq(a, i, b, j);
}

void edits_free(struct editlist *el)
{
	free(el->v);
	memset(el, 0, sizeof *el);
}

/* Adjacent edits that touch on both sides are one edit. */
static int edit_push(struct editlist *el, size_t alo, size_t ahi,
		     size_t blo, size_t bhi)
{
	if (alo == ahi && blo == bhi)
		return 0;

	if (el->n) {
		struct edit *last = &el->v[el->n - 1];

		if (last->ahi == alo && last->bhi == blo) {
			last->ahi = ahi;
			last->bhi = bhi;
			return 0;
		}
	}

	if (el->n == el->cap) {
		size_t ncap = el->cap ? el->cap * 2 : 32;
		struct edit *nv = realloc(el->v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		el->v = nv;
		el->cap = ncap;
	}

	el->v[el->n++] = (struct edit){alo, ahi, blo, bhi};
	return 0;
}

/*
 * Exact answer for a small range, by the usual longest-common-subsequence
 * table filled from the back.
 */
static int diff_small(const struct lineset *a, size_t alo, size_t ahi,
		      const struct lineset *b, size_t blo, size_t bhi,
		      struct editlist *out, const struct dstate *ds)
{
	size_t na = ahi - alo, nb = bhi - blo;
	size_t w = nb + 1;
	unsigned int *dp;
	size_t i, j;
	int rc = -1;

	dp = malloc((na + 1) * w * sizeof *dp);
	if (!dp)
		return -1;

	for (i = na + 1; i-- > 0;) {
		for (j = nb + 1; j-- > 0;) {
			if (i == na || j == nb) {
				dp[i * w + j] = 0;
			} else if (lines_equal(ds, a, alo + i, b, blo + j)) {
				dp[i * w + j] = 1 + dp[(i + 1) * w + j + 1];
			} else {
				unsigned int x = dp[(i + 1) * w + j];
				unsigned int y = dp[i * w + j + 1];

				dp[i * w + j] = x > y ? x : y;
			}
		}
	}

	i = j = 0;
	while (i < na || j < nb) {
		size_t si, sj;

		if (i < na && j < nb &&
		    lines_equal(ds, a, alo + i, b, blo + j)) {
			i++;
			j++;
			continue;
		}

		si = i;
		sj = j;
		while (i < na || j < nb) {
			if (i < na && j < nb &&
			    lines_equal(ds, a, alo + i, b, blo + j))
				break;
			if (i < na && j < nb) {
				if (dp[(i + 1) * w + j] >= dp[i * w + j + 1])
					i++;
				else
					j++;
			} else if (i < na) {
				i++;
			} else {
				j++;
			}
		}

		if (edit_push(out, alo + si, alo + i, blo + sj, blo + j) < 0)
			goto out;
	}

	rc = 0;
out:
	free(dp);
	return rc;
}

/* Fingerprint and index of every line of a lineset. */
static void refs_fill(struct lref *v, const struct lineset *ls)
{
	size_t i;

	for (i = 0; i < ls->n; i++) {
		v[i].fp = (uint32_t)(ls->v[i].hash >> 32);
		v[i].idx = (uint32_t)i;
	}
}

/*
 * Sort refs by fingerprint, least significant byte first, four times.
 *
 * No comparisons anywhere in it, which is the point.  qsort would call a
 * comparison function some sixty million times over this much data, and every
 * call branches on memory that the previous comparison just left where it
 * was.  A radix pass instead reads forwards and writes forwards -- into 256
 * places at once, but each of them in order -- and the histogram it needs is
 * a kilobyte that stays in L1.
 *
 * Four passes is even, so the sorted refs land back in `v` and `tmp` is only
 * ever scratch.  The check at the end is for whoever changes the count.
 */
static void refs_sort(struct lref *v, struct lref *tmp, size_t n)
{
	struct lref *src = v, *dst = tmp;
	unsigned int pass;

	if (n < 2)
		return;

	for (pass = 0; pass < 4; pass++) {
		unsigned int shift = pass * 8;
		size_t cnt[256], pos[256], i, sum;
		struct lref *swap;

		memset(cnt, 0, sizeof cnt);
		for (i = 0; i < n; i++)
			cnt[(src[i].fp >> shift) & 0xff]++;
		sum = 0;
		for (i = 0; i < 256; i++) {
			pos[i] = sum;
			sum += cnt[i];
		}
		for (i = 0; i < n; i++) {
			unsigned int d = (src[i].fp >> shift) & 0xff;

			dst[pos[d]++] = src[i];
		}

		swap = src;
		src = dst;
		dst = swap;
	}

	if (src != v)
		memcpy(v, src, n * sizeof *v);
}

/*
 * Renumber the classes so that they follow the order the lines appear in,
 * A's lines first and then B's.
 *
 * Merging hands out class numbers in fingerprint order, and that is the wrong
 * order to read them back in.  The anchor search walks a file line by line
 * and for each line looks up count_a[c], count_b[c] and first_b[c] -- three
 * reads indexed by class, over arrays the size of the class count.  Numbering
 * the classes in file order makes those three reads walk forwards together,
 * which is what the class-numbered code this replaced did implicitly; leaving
 * them in fingerprint order makes every one of them a random access, and the
 * anchor scan got five times slower for it.  The lines are the same either
 * way; only the numbering moves.
 */
static void classes_renumber(uint32_t *ca, size_t na, uint32_t *cb, size_t nb,
			     uint32_t ncls, uint32_t *map)
{
	uint32_t next = 0;
	size_t i;

	for (i = 0; i < ncls; i++)
		map[i] = CLASS_NONE;
	for (i = 0; i < na; i++) {
		if (map[ca[i]] == CLASS_NONE)
			map[ca[i]] = next++;
		ca[i] = map[ca[i]];
	}
	for (i = 0; i < nb; i++) {
		if (map[cb[i]] == CLASS_NONE)
			map[cb[i]] = next++;
		cb[i] = map[cb[i]];
	}
}

/*
 * A class opened while merging one run of equal fingerprints.  Only lines in
 * the same run can be equal to each other, because equal lines hash equally
 * and so share a fingerprint.
 */
struct oclass {
	uint32_t cls;
	uint32_t idx;
	bool from_b;
};

/*
 * How many classes one run may open before later lines stop being merged into
 * it.
 *
 * A run normally holds a single distinct line: two lines share a fingerprint
 * only if their hashes collide in 32 bits.  Reaching this many means an
 * adversarial file or a shape this tool has never met, and the answer then is
 * to stop looking rather than compare every line of the run against every
 * class opened in it.  A line past the cap still gets a class, just one of
 * its own -- conservative in the only direction that matters: two lines that
 * are equal but not *known* equal are simply never matched, so the range
 * around them is reported as wholly replaced.  Coarser, never wrong.
 */
#define GROUP_MAX 64

/*
 * Number every line of both linesets.
 *
 * Sort each file's lines by fingerprint, then walk the two sorted runs
 * together: one fingerprint is one candidate group of equal lines, and inside
 * it the text decides.  The byte comparisons happen between lines that the
 * sort just brought together and that the run keeps in cache; everything else
 * is sequential.
 */
static int classes_assign(const struct lineset *a, const struct lineset *b,
			  uint32_t **ca_out, uint32_t **cb_out, uint32_t *ncls)
{
	struct lref *ra, *rb, *tmp;
	struct oclass open[GROUP_MAX];
	uint32_t *ca, *cb, *map = NULL, next = 0;
	size_t i = 0, j = 0, scratch;
	int rc = -1;

	scratch = a->n > b->n ? a->n : b->n;
	ra = malloc((a->n ? a->n : 1) * sizeof *ra);
	rb = malloc((b->n ? b->n : 1) * sizeof *rb);
	tmp = malloc((scratch ? scratch : 1) * sizeof *tmp);
	ca = malloc((a->n ? a->n : 1) * sizeof *ca);
	cb = malloc((b->n ? b->n : 1) * sizeof *cb);
	if (!ra || !rb || !tmp || !ca || !cb)
		goto out;

	refs_fill(ra, a);
	refs_fill(rb, b);
	refs_sort(ra, tmp, a->n);
	refs_sort(rb, tmp, b->n);

	while (i < a->n || j < b->n) {
		uint32_t fp = (i < a->n && (j >= b->n || ra[i].fp <= rb[j].fp))
				      ? ra[i].fp
				      : rb[j].fp;
		size_t nopen = 0;
		int side;

		/* Everything in either file carrying this fingerprint. */
		for (side = 0; side < 2; side++) {
			const struct lineset *ls = side ? b : a;
			const struct lref *r = side ? rb : ra;
			uint32_t *cls = side ? cb : ca;
			size_t *p = side ? &j : &i;

			while (*p < ls->n && r[*p].fp == fp) {
				uint32_t idx = r[*p].idx, c = CLASS_NONE;
				size_t k;

				for (k = 0; k < nopen; k++) {
					const struct lineset *rs =
						open[k].from_b ? b : a;

					if (lline_eq(rs, open[k].idx, ls,
						     idx)) {
						c = open[k].cls;
						break;
					}
				}
				if (c == CLASS_NONE) {
					c = next++;
					if (nopen < GROUP_MAX)
						open[nopen++] =
							(struct oclass){c, idx,
									side != 0};
				}
				cls[idx] = c;
				(*p)++;
			}
		}
	}

	map = malloc((next ? next : 1) * sizeof *map);
	if (!map)
		goto out;
	classes_renumber(ca, a->n, cb, b->n, next, map);

	*ca_out = ca;
	*cb_out = cb;
	*ncls = next;
	ca = NULL;
	cb = NULL;
	rc = 0;
out:
	free(map);
	free(ra);
	free(rb);
	free(tmp);
	free(ca);
	free(cb);
	return rc;
}

/*
 * Which lines occur exactly once in a file.
 *
 * The anchor search needs this at every level of its recursion, and the
 * obvious way to ask -- sort the range and look for runs of one -- costs a
 * sort per level, which is where nearly all of this tool's time used to go.
 * Counting classes once, up front, answers it for every level at once: a line
 * that occurs once in the whole file occurs once in any part of it.
 *
 * A hash is not the line, so a caller that acts on a match still has to
 * compare the text.  Treating two colliding lines as one is not merely
 * imprecise here -- it would mean skipping both from the diff and quietly
 * dropping a real difference -- which is why a class is only ever issued
 * after its bytes have been compared.
 */
static int classes_count(const uint32_t *ca, size_t na, const uint32_t *cb,
			 size_t nb, uint32_t ncls, uint32_t **cnt_a_out,
			 uint32_t **cnt_b_out, uint32_t **first_b_out)
{
	uint32_t *cnt_a, *cnt_b, *first_b;
	size_t i;

	cnt_a = calloc(ncls ? ncls : 1, sizeof *cnt_a);
	cnt_b = calloc(ncls ? ncls : 1, sizeof *cnt_b);
	first_b = malloc((ncls ? ncls : 1) * sizeof *first_b);
	if (!cnt_a || !cnt_b || !first_b) {
		free(cnt_a);
		free(cnt_b);
		free(first_b);
		return -1;
	}
	for (i = 0; i < ncls; i++)
		first_b[i] = CLASS_NONE;

	for (i = 0; i < na; i++)
		cnt_a[ca[i]]++;
	for (i = 0; i < nb; i++) {
		if (!cnt_b[cb[i]])
			first_b[cb[i]] = (uint32_t)i;
		cnt_b[cb[i]]++;
	}

	*cnt_a_out = cnt_a;
	*cnt_b_out = cnt_b;
	*first_b_out = first_b;
	return 0;
}

static int dstate_build(struct dstate *ds)
{
	uint32_t ncls = 0;

	if (ds->built)
		return ds->failed ? -1 : 0;
	ds->built = true;

	if (classes_assign(ds->a, ds->b, &ds->ca, &ds->cb, &ncls) < 0)
		goto fail;
	if (classes_count(ds->ca, ds->a->n, ds->cb, ds->b->n, ncls,
			  &ds->count_a, &ds->count_b, &ds->first_b) < 0)
		goto fail;
	return 0;
fail:
	ds->failed = true;
	return -1;
}


/*
 * Split a large range at a line that is unique on both sides, which is the
 * line most likely to be a real correspondence rather than a coincidence.
 */
static int diff_anchor(const struct lineset *a, size_t alo, size_t ahi,
		       const struct lineset *b, size_t blo, size_t bhi,
		       struct editlist *out, int depth, struct dstate *ds);

static int diff_rec(const struct lineset *a, size_t alo, size_t ahi,
		    const struct lineset *b, size_t blo, size_t bhi,
		    struct editlist *out, int depth, struct dstate *ds)
{
	/* Strip the common ends first: it is cheap, and it usually leaves
	 * very little behind. */
	while (alo < ahi && blo < bhi && lines_equal(ds, a, alo, b, blo)) {
		alo++;
		blo++;
	}
	while (alo < ahi && blo < bhi &&
	       lines_equal(ds, a, ahi - 1, b, bhi - 1)) {
		ahi--;
		bhi--;
	}

	if (alo == ahi && blo == bhi)
		return 0;
	if (alo == ahi || blo == bhi)
		return edit_push(out, alo, ahi, blo, bhi);

	if (ahi - alo <= SMALL && bhi - blo <= SMALL)
		return diff_small(a, alo, ahi, b, blo, bhi, out, ds);

	if (depth <= 0)
		return edit_push(out, alo, ahi, blo, bhi);

	return diff_anchor(a, alo, ahi, b, blo, bhi, out, depth, ds);
}

static int diff_anchor(const struct lineset *a, size_t alo, size_t ahi,
		       const struct lineset *b, size_t blo, size_t bhi,
		       struct editlist *out, int depth, struct dstate *ds)
{
	size_t i, best_i = 0, best_j = 0;
	bool found = false;
	uint64_t best_dist = UINT64_MAX;
	uint64_t mid_a = alo + (ahi - alo) / 2;
	uint64_t mid_b = blo + (bhi - blo) / 2;

	/* Reaching here is what makes the classes worth building. */
	if (dstate_build(ds) < 0)
		return -1;

	/*
	 * Walk this side for a line that occurs exactly once in each file --
	 * the line most likely to be a real correspondence rather than a
	 * coincidence between two common ones -- and take the match nearest
	 * the middle so the recursion stays balanced.
	 *
	 * With classes this is three array lookups per line and no byte
	 * comparison at all: a class number was only issued once its bytes
	 * had been compared, so equal numbers are equal lines.
	 */
	for (i = alo; i < ahi; i++) {
		uint32_t c = ds->ca[i];
		size_t j;
		uint64_t d;

		if (ds->count_a[c] != 1 || ds->count_b[c] != 1)
			continue;
		j = ds->first_b[c];
		if (j < blo || j >= bhi)
			continue;

		d = (i > mid_a ? i - mid_a : mid_a - i) +
		    (j > mid_b ? j - mid_b : mid_b - j);
		if (!found || d < best_dist) {
			best_dist = d;
			best_i = i;
			best_j = j;
			found = true;
		}
	}

	if (!found) {
		/*
		 * Nothing occurs exactly once on both sides.  Fall back to the
		 * middle of each range, which is a guess that the two files are
		 * still line-for-line aligned there.
		 *
		 * A line that is not unique is still usable as a split *if the
		 * two are really equal*: a class number was only issued after
		 * the bytes were compared, so lines_equal is a proof, and a
		 * proof is all the recursion needs.  What is being guessed is
		 * only which of several equal lines corresponds to which; a
		 * wrong guess splits the range in the wrong place, which gives
		 * a coarser answer, never a false one -- both sides are still
		 * diffed exactly.
		 *
		 * This matters more than it looks.  Two files that differ in
		 * one line out of a million, where the common-suffix trim was
		 * stopped by that same line or by a trailing newline, land
		 * here with nothing unique to hold, and reporting the range as
		 * wholly replaced says the million lines changed.  True, and
		 * useless.  Guessing the middle turns that back into the one
		 * line it always was.
		 */
		best_i = alo + (ahi - alo) / 2;
		best_j = blo + (bhi - blo) / 2;
		if (!lines_equal(ds, a, best_i, b, best_j))
			return edit_push(out, alo, ahi, blo, bhi);
	}

	if (diff_rec(a, alo, best_i, b, blo, best_j, out, depth - 1, ds) < 0)
		return -1;
	if (diff_rec(a, best_i + 1, ahi, b, best_j + 1, bhi, out, depth - 1,
		     ds) < 0)
		return -1;
	return 0;
}

int linediff(const struct lineset *a, const struct lineset *b,
	     struct editlist *out)
{
	struct dstate ds;
	int rc;

	out->v = NULL;
	out->n = 0;
	out->cap = 0;

	memset(&ds, 0, sizeof ds);
	ds.a = a;
	ds.b = b;

	/* Depth cap keeps the recursion bounded on pathological input; each
	 * level at least splits the range, so this is generous. */
	rc = diff_rec(a, 0, a->n, b, 0, b->n, out, 64, &ds);

	free(ds.ca);
	free(ds.cb);
	free(ds.count_a);
	free(ds.count_b);
	free(ds.first_b);
	return rc;
}

/* ---- line boundary helpers -------------------------------------------- */

int line_start(int fd, uint64_t off, uint64_t *out)
{
	unsigned char buf[4096];

	while (off > 0) {
		uint64_t n = off < sizeof buf ? off : sizeof buf;
		uint64_t base = off - n;
		ssize_t i;

		if (pread_full(fd, buf, n, base) < 0)
			return -1;
		for (i = (ssize_t)n - 1; i >= 0; i--) {
			if (buf[i] == '\n') {
				*out = base + (uint64_t)i + 1;
				return 0;
			}
		}
		off = base;
	}
	*out = 0;
	return 0;
}

int line_end(int fd, uint64_t off, uint64_t limit, uint64_t *out)
{
	unsigned char buf[4096];

	while (off < limit) {
		uint64_t n = limit - off < sizeof buf ? limit - off : sizeof buf;
		ssize_t i;

		if (pread_full(fd, buf, n, off) < 0)
			return -1;
		for (i = 0; i < (ssize_t)n; i++) {
			if (buf[i] == '\n') {
				*out = off + (uint64_t)i + 1;
				return 0;
			}
		}
		off += n;
	}
	*out = limit;
	return 0;
}
