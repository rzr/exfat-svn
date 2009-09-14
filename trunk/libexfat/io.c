/*
 *  io.c
 *  exFAT file system implementation library.
 *
 *  Created by Andrew Nayenko on 02.09.09.
 *  This software is distributed under the GNU General Public License 
 *  version 3 or any later.
 */

#include "exfat.h"
#include <sys/types.h>
#include <sys/uio.h>
#define __USE_UNIX98 /* for pread() in Linux */
#include <unistd.h>

#if _FILE_OFFSET_BITS != 64
	#error You should define _FILE_OFFSET_BITS=64
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

void exfat_read_raw(void* buffer, size_t size, off_t offset, int fd)
{
	if (pread(fd, buffer, size, offset) != size)
		exfat_bug("failed to read %zu bytes from file at %llu", size, offset);
}

ssize_t exfat_read(const struct exfat* ef, const struct exfat_node* node,
		void* buffer, size_t size, off_t offset)
{
	cluster_t cluster;
	char* bufp = buffer;
	off_t lsize, loffset, remainder;

	if (offset >= node->size)
		return 0;
	if (size == 0)
		return 0;

	cluster = exfat_advance_cluster(ef, node->start_cluster,
			IS_CONTIGUOUS(*node), offset / CLUSTER_SIZE(*ef->sb));
	if (CLUSTER_INVALID(cluster))
	{
		exfat_error("got invalid cluster");
		return -1;
	}

	loffset = offset % CLUSTER_SIZE(*ef->sb);
	remainder = MIN(size, node->size - offset);
	while (remainder > 0)
	{
		lsize = MIN(CLUSTER_SIZE(*ef->sb) - loffset, remainder);
		exfat_read_raw(bufp, lsize, exfat_c2o(ef, cluster) + loffset, ef->fd);
		bufp += lsize;
		loffset = 0;
		remainder -= lsize;
		cluster = exfat_next_cluster(ef, cluster, IS_CONTIGUOUS(*node));
		if (CLUSTER_INVALID(cluster))
		{
			exfat_error("got invalid cluster");
			return -1;
		}
	}
	return size - remainder;
}
