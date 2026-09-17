/*
 * probe - fixture helper for testing cowdiff.
 *
 * It does exactly one thing cowdiff will not: clone a byte range from one
 * file into another with FICLONERANGE.
 *
 * That is needed because btrfs has no FALLOC_FL_INSERT_RANGE, so the only way
 * to build a *shifted* share -- a tail pointing at the original's extents but
 * at file offsets differing by the size of an insertion -- is to insert the
 * bytes the ordinary way and then re-clone the tail from the original.  That
 * construct is the one that exercises cowdiff's shifted-anchor path, where the
 * offset delta is itself the insertion.
 *
 * btrfs requires sector-aligned offsets for clone, so callers must insert a
 * whole block for the shifted tail to be re-sharable.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/fs.h>

static int cmd_clone(const char *src, const char *dst,
		     unsigned long long src_off, unsigned long long len,
		     unsigned long long dst_off)
{
	struct file_clone_range r;
	int sfd, dfd, ret = 0;

	sfd = open(src, O_RDONLY);
	if (sfd < 0) {
		fprintf(stderr, "open %s: %s\n", src, strerror(errno));
		return 1;
	}
	dfd = open(dst, O_WRONLY);
	if (dfd < 0) {
		fprintf(stderr, "open %s: %s\n", dst, strerror(errno));
		close(sfd);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	r.src_fd = sfd;
	r.src_offset = src_off;
	r.src_length = len;
	r.dest_offset = dst_off;

	if (ioctl(dfd, FICLONERANGE, &r) < 0) {
		fprintf(stderr, "FICLONERANGE %s[%llu,+%llu] -> %s[%llu]: %s\n",
			src, src_off, len, dst, dst_off, strerror(errno));
		ret = 1;
	} else {
		printf("cloned %s[%llu,+%llu] -> %s[%llu]\n",
		       src, src_off, len, dst, dst_off);
	}

	close(sfd);
	close(dfd);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc >= 7 && !strcmp(argv[1], "clone"))
		return cmd_clone(argv[2], argv[3], strtoull(argv[4], NULL, 0),
				 strtoull(argv[5], NULL, 0),
				 strtoull(argv[6], NULL, 0));

	fprintf(stderr, "usage: %s clone SRC DST SRCOFF LEN DSTOFF\n", argv[0]);
	return 2;
}
