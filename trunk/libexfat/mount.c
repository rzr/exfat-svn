/*
	mount.c (22.10.09)
	exFAT file system implementation library.

	Copyright (C) 2009, 2010  Andrew Nayenko

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "exfat.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#define _XOPEN_SOURCE /* for tzset() in Linux */
#include <time.h>

static uint64_t rootdir_size(const struct exfat* ef)
{
	uint64_t clusters = 0;
	cluster_t rootdir_cluster = le32_to_cpu(ef->sb->rootdir_cluster);

	while (!CLUSTER_INVALID(rootdir_cluster))
	{
		clusters++;
		/* root directory cannot be contiguous because there is no flag
		   to indicate this */
		rootdir_cluster = exfat_next_cluster(ef, ef->root, rootdir_cluster);
	}
	return clusters * CLUSTER_SIZE(*ef->sb);
}

static const char* get_option(const char* options, const char* option_name)
{
	const char* p;
	size_t length = strlen(option_name);

	for (p = strstr(options, option_name); p; p = strstr(p + 1, option_name))
		if ((p == options || p[-1] == ',') && p[length] == '=')
			return p + length + 1;
	return NULL;
}

static int get_int_option(const char* options, const char* option_name,
		int base, int default_value)
{
	const char* p = get_option(options, option_name);

	if (p == NULL)
		return default_value;
	return strtol(p, NULL, base);
}

static int match_option(const char* options, const char* option_name)
{
	const char* p;
	size_t length = strlen(option_name);

	for (p = strstr(options, option_name); p; p = strstr(p + 1, option_name))
		if ((p == options || p[-1] == ',') &&
				(p[length] == ',' || p[length] == '\0'))
			return 1;
	return 0;
}

static void parse_options(struct exfat* ef, const char* options)
{
	int sys_umask = umask(0);
	int opt_umask;

	umask(sys_umask); /* restore umask */
	opt_umask = get_int_option(options, "umask", 8, sys_umask);
	ef->dmask = get_int_option(options, "dmask", 8, opt_umask) & 0777;
	ef->fmask = get_int_option(options, "fmask", 8, opt_umask) & 0777;

	ef->uid = get_int_option(options, "uid", 10, geteuid());
	ef->gid = get_int_option(options, "gid", 10, getegid());

	ef->ro = match_option(options, "ro");
	ef->noatime = match_option(options, "noatime");
}

int exfat_mount(struct exfat* ef, const char* spec, const char* options)
{
	uint16_t fs_version;

	tzset();
	memset(ef, 0, sizeof(struct exfat));

	ef->sb = malloc(sizeof(struct exfat_super_block));
	if (ef->sb == NULL)
	{
		exfat_error("memory allocation failed");
		return -ENOMEM;
	}
	memset(ef->sb, 0, sizeof(struct exfat_super_block));

	parse_options(ef, options);

	ef->fd = open(spec, ef->ro ? O_RDONLY : O_RDWR);
	if (ef->fd < 0)
	{
		free(ef->sb);
		exfat_error("failed to open `%s'", spec);
		return -EIO;
	}

	exfat_read_raw(ef->sb, sizeof(struct exfat_super_block), 0, ef->fd);
	if (memcmp(ef->sb->oem_name, "EXFAT   ", 8) != 0)
	{
		close(ef->fd);
		free(ef->sb);
		exfat_error("exFAT file system is not found");
		return -EIO;
	}
	fs_version = le16_to_cpu(ef->sb->version);
	if (fs_version != 0x0100)
	{
		close(ef->fd);
		free(ef->sb);
		exfat_error("unsupported exFAT version: %hu.%hu",
				fs_version >> 8, fs_version & 0xff);
		return -EIO;
	}
	/* officially exFAT supports cluster size up to 32 MB */
	if ((int) ef->sb->block_bits + (int) ef->sb->bpc_bits > 25)
	{
		close(ef->fd);
		free(ef->sb);
		exfat_error("too big cluster size: 2^%d",
				(int) ef->sb->block_bits + (int) ef->sb->bpc_bits);
		return -EIO;
	}

	ef->zero_block = malloc(BLOCK_SIZE(*ef->sb));
	if (ef->zero_block == NULL)
	{
		close(ef->fd);
		free(ef->sb);
		exfat_error("failed to allocate zero block");
		return -ENOMEM;
	}
	memset(ef->zero_block, 0, BLOCK_SIZE(*ef->sb));

	ef->root = malloc(sizeof(struct exfat_node));
	if (ef->root == NULL)
	{
		free(ef->zero_block);
		close(ef->fd);
		free(ef->sb);
		exfat_error("failed to allocate root node");
		return -ENOMEM;
	}
	memset(ef->root, 0, sizeof(struct exfat_node));
	ef->root->flags = EXFAT_ATTRIB_DIR;
	ef->root->start_cluster = le32_to_cpu(ef->sb->rootdir_cluster);
	ef->root->fptr_cluster = ef->root->start_cluster;
	ef->root->name[0] = cpu_to_le16('\0');
	ef->root->size = rootdir_size(ef);
	/* exFAT does not have time attributes for the root directory */
	ef->root->mtime = 0;
	ef->root->atime = 0;
	/* always keep at least 1 reference to the root node */
	exfat_get_node(ef->root);

	return 0;
}

void exfat_unmount(struct exfat* ef)
{
	exfat_put_node(ef, ef->root);
	exfat_reset_cache(ef);
	free(ef->root);
	ef->root = NULL;
	free(ef->zero_block);
	ef->zero_block = NULL;
	free(ef->cmap.chunk);
	ef->cmap.chunk = NULL;
	close(ef->fd);
	ef->fd = 0;
	free(ef->sb);
	ef->sb = NULL;
	free(ef->upcase);
	ef->upcase = NULL;
	ef->upcase_chars = 0;
}
