/*
Copyright 2020 Google LLC

Use of this source code is governed by a BSD-style
license that can be found in the LICENSE file or at
https://developers.google.com/open-source/licenses/bsd
*/

#include "system.h"

#include "block.h"
#include "iter.h"
#include "record.h"
#include "reftable.h"
#include "tree.h"

struct file_block_source {
	int fd;
	uint64_t size;
};

static uint64_t file_size(void *b)
{
	return ((struct file_block_source *)b)->size;
}

static void file_return_block(void *b, struct reftable_block *dest)
{
	memset(dest->data, 0xff, dest->len);
	reftable_free(dest->data);
}

static void file_close(void *b)
{
	int fd = ((struct file_block_source *)b)->fd;
	if (fd > 0) {
		close(fd);
		((struct file_block_source *)b)->fd = 0;
	}

	reftable_free(b);
}

static int file_read_block(void *v, struct reftable_block *dest, uint64_t off,
			   uint32_t size)
{
	struct file_block_source *b = (struct file_block_source *)v;
	assert(off + size <= b->size);
	dest->data = reftable_malloc(size);
	if (pread(b->fd, dest->data, size, off) != size) {
		return -1;
	}
	dest->len = size;
	return size;
}

struct reftable_block_source_vtable file_vtable = {
	.size = &file_size,
	.read_block = &file_read_block,
	.return_block = &file_return_block,
	.close = &file_close,
};

int reftable_block_source_from_file(struct reftable_block_source *bs,
				    const char *name)
{
	struct stat st = { 0 };
	int err = 0;
	int fd = open(name, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT) {
			return REFTABLE_NOT_EXIST_ERROR;
		}
		return -1;
	}

	err = fstat(fd, &st);
	if (err < 0) {
		return -1;
	}

	{
		struct file_block_source *p =
			reftable_calloc(sizeof(struct file_block_source));
		p->size = st.st_size;
		p->fd = fd;

		bs->ops = &file_vtable;
		bs->arg = p;
	}
	return 0;
}

int reftable_fd_write(void *arg, byte *data, int sz)
{
	int *fdp = (int *)arg;
	return write(*fdp, data, sz);
}
