/*
 * anchor.c - find the byte ranges that provably do not need comparing.
 *
 * A physical address is treated as an identity for the content stored there,
 * not as a location.  So if a range of A and a range of B resolve to the same
 * address, those bytes are equal -- and it does not matter whether the two
 * ranges sit at the same offset in their files.
 *
 * That distinction is what lets this handle insertions.  Inserting bytes into
 * a file shifts everything after them, so the tail of the new file is still
 * the old file's data, just further along.  Matching by address finds it
 * anyway, and the difference between the two offsets *is* the insertion: an
 * anchor at offset delta 0 before the edit and one at delta +n after it
 * immediately say "n bytes were inserted", with none of the tail ever read.
 *
 * Finding these is an intersection of two sets of physical ranges, followed
 * by picking a subset that never goes backwards in either file.  Any such
 * subset is a correct alignment, and a worse choice only means larger gaps
 * left to compare -- so the greedy selection below is safe for the gaps it
 * leaves.  It was not safe for the *verdict*, and the claim that it was is
 * what this file got wrong: a match at differing offsets proves the content
 * is equal, not that those bytes agree where they sit, and the gaps a shifted
 * chain leaves are one-sided insertions, which deltas_find reports as
 * differences without reading anything.  Two files of equal length whose
 * shares are crossed came out "different".  anchors_keep_same_offset() below
 * is what removes that case, and its comment says why.
 */
#include "cowdiff.h"

#include <stdlib.h>
#include <string.h>

#include <linux/fiemap.h>

/* One physical range of one file, ready for the intersection sweep. */
struct prange {
	uint64_t phys;
	uint64_t off;
	uint64_t len;
};

struct cand {
	uint64_t a_off;
	uint64_t b_off;
	uint64_t len;
};

static int prange_cmp(const void *pa, const void *pb)
{
	const struct prange *a = pa, *b = pb;

	if (a->phys != b->phys)
		return a->phys < b->phys ? -1 : 1;
	if (a->off != b->off)
		return a->off < b->off ? -1 : 1;
	return 0;
}

/* For extents whose length is not a physical length, only address and
 * length together identify the extent, so sort on those. */
static int prange_cmp_pl(const void *pa, const void *pb)
{
	const struct prange *a = pa, *b = pb;

	if (a->phys != b->phys)
		return a->phys < b->phys ? -1 : 1;
	if (a->len != b->len)
		return a->len < b->len ? -1 : 1;
	if (a->off != b->off)
		return a->off < b->off ? -1 : 1;
	return 0;
}

static int cand_cmp(const void *pa, const void *pb)
{
	const struct cand *a = pa, *b = pb;

	if (a->a_off != b->a_off)
		return a->a_off < b->a_off ? -1 : 1;
	/*
	 * When two proofs start at the same place prefer the longer one: it
	 * leaves less to compare and costs nothing to choose.
	 */
	if (a->len != b->len)
		return a->len > b->len ? -1 : 1;
	if (a->b_off != b->b_off)
		return a->b_off < b->b_off ? -1 : 1;
	return 0;
}

static int cand_add(struct cand **v, size_t *n, size_t *cap, uint64_t a_off,
		    uint64_t b_off, uint64_t len)
{
	if (*n == *cap) {
		size_t ncap = *cap ? *cap * 2 : 256;
		struct cand *nv = realloc(*v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		*v = nv;
		*cap = ncap;
	}
	(*v)[(*n)++] = (struct cand){a_off, b_off, len};
	return 0;
}

void anchors_free(struct anchorlist *al)
{
	free(al->v);
	al->v = NULL;
	al->n = 0;
	al->cap = 0;
}

/*
 * Append an anchor.  Anchors arrive in ascending order and never overlap, so
 * two that are contiguous in both files are really one longer proof.
 */
static int anchors_push(struct anchorlist *al, uint64_t a_off, uint64_t b_off,
			uint64_t len)
{
	if (al->n) {
		struct anchor *last = &al->v[al->n - 1];

		if (last->a_off + last->len == a_off &&
		    last->b_off + last->len == b_off) {
			last->len += len;
			return 0;
		}
	}

	if (al->n == al->cap) {
		size_t ncap = al->cap ? al->cap * 2 : 128;
		struct anchor *nv = realloc(al->v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		al->v = nv;
		al->cap = ncap;
	}

	al->v[al->n++] = (struct anchor){
		.a_off = a_off,
		.b_off = b_off,
		.len = len,
	};
	return 0;
}

/*
 * Physical ranges whose address can be trusted.  Everything else -- inline
 * data, delayed allocation, unwritten extents, filesystems that have no
 * extent list to report -- is left out, which simply means those bytes end up
 * in a gap and get compared.
 *
 * Compressed extents are collected separately, because for them fe_length is
 * the length the data will have once decompressed, not the number of bytes
 * the extent occupies on disk.  Two such extents can therefore report
 * overlapping [address, address+length) ranges while occupying quite
 * separate disk space, and intersecting those ranges invents matches that do
 * not exist.  They are matched by exact identity instead.
 */
static int collect(const struct extmap *m, bool want_encoded,
		   struct prange **out, size_t *n_out, bool by_len)
{
	struct prange *v;
	size_t n = 0, i;

	v = malloc((m->n ? m->n : 1) * sizeof *v);
	if (!v)
		return -1;

	for (i = 0; i < m->n; i++) {
		const struct ext *e = &m->v[i];
		bool encoded = (e->flags & FIEMAP_EXTENT_ENCODED) != 0;

		if (!e->trusted || e->len == 0 || encoded != want_encoded)
			continue;
		v[n++] = (struct prange){
			.phys = e->phys,
			.off = e->off,
			.len = e->len,
		};
	}

	qsort(v, n, sizeof *v, by_len ? prange_cmp_pl : prange_cmp);
	*out = v;
	*n_out = n;
	return 0;
}

int anchors_find(const struct extmap *a, const struct extmap *b,
		 bool addresses_comparable, struct anchorlist *out)
{
	struct prange *pa = NULL, *pb = NULL;
	struct cand *cands = NULL;
	size_t na = 0, nb = 0, nc = 0, cap = 0;
	size_t i = 0, j = 0;
	int rc = -1;

	out->v = NULL;
	out->n = 0;
	out->cap = 0;

	/*
	 * Addresses from two different filesystems come from two different
	 * spaces.  Equality between them would be a coincidence, and acting
	 * on it would mean reporting equal files that are not.
	 */
	if (!addresses_comparable)
		return 0;

	/* Uncompressed extents: fe_length really is a physical length, so the
	 * two sets of ranges mean what they say and can be intersected. */
	if (collect(a, false, &pa, &na, false) < 0 ||
	    collect(b, false, &pb, &nb, false) < 0)
		goto out;

	while (i < na && j < nb) {
		uint64_t as = pa[i].phys, ae = as + pa[i].len;
		uint64_t bs = pb[j].phys, be = bs + pb[j].len;
		uint64_t lo, hi;

		if (ae <= bs) {
			i++;
			continue;
		}
		if (be <= as) {
			j++;
			continue;
		}

		lo = as > bs ? as : bs;
		hi = ae < be ? ae : be;

		if (cand_add(&cands, &nc, &cap, pa[i].off + (lo - as),
			     pb[j].off + (lo - bs), hi - lo) < 0)
			goto out;

		/* Move past whichever range ended first. */
		if (ae <= be)
			i++;
		if (be <= ae)
			j++;
	}

	/*
	 * Compressed extents, matched on address and length together.  That
	 * pair names one extent on disk and the same extent holds the same
	 * bytes wherever it is pointed at from, so the file offsets are
	 * deliberately not required to match -- an insertion whose tail was
	 * re-shared still lines up.
	 */
	free(pa);
	free(pb);
	pa = pb = NULL;
	na = nb = 0;
	i = j = 0;

	if (collect(a, true, &pa, &na, true) < 0 ||
	    collect(b, true, &pb, &nb, true) < 0)
		goto out;

	while (i < na && j < nb) {
		if (pa[i].phys != pb[j].phys || pa[i].len != pb[j].len) {
			if (pa[i].phys < pb[j].phys ||
			    (pa[i].phys == pb[j].phys && pa[i].len < pb[j].len))
				i++;
			else
				j++;
			continue;
		}
		if (cand_add(&cands, &nc, &cap, pa[i].off, pb[j].off,
			     pa[i].len) < 0)
			goto out;
		i++;
		j++;
	}

	if (nc == 0) {
		rc = 0;
		goto out;
	}

	qsort(cands, nc, sizeof *cands, cand_cmp);

	/*
	 * Keep a chain that never moves backwards in either file.  Skipping a
	 * candidate is always safe: whatever it would have proven simply ends
	 * up in a gap and gets compared the ordinary way.
	 */
	{
		uint64_t last_a = 0, last_b = 0;
		size_t k;

		for (k = 0; k < nc; k++) {
			if (cands[k].a_off < last_a || cands[k].b_off < last_b)
				continue;
			if (anchors_push(out, cands[k].a_off, cands[k].b_off,
					 cands[k].len) < 0)
				goto out;
			last_a = cands[k].a_off + cands[k].len;
			last_b = cands[k].b_off + cands[k].len;
		}
	}

	rc = 0;
out:
	free(pa);
	free(pb);
	free(cands);
	if (rc < 0)
		anchors_free(out);
	return rc;
}

void anchors_keep_same_offset(struct anchorlist *al)
{
	size_t i, k = 0;

	for (i = 0; i < al->n; i++) {
		if (al->v[i].a_off != al->v[i].b_off)
			continue;
		al->v[k++] = al->v[i];
	}
	al->n = k;
}
