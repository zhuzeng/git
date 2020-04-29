/*
Copyright 2020 Google LLC

Use of this source code is governed by a BSD-style
license that can be found in the LICENSE file or at
https://developers.google.com/open-source/licenses/bsd
*/

#ifndef BLOCK_H
#define BLOCK_H

#include "basics.h"
#include "record.h"
#include "reftable.h"

/*
  Writes reftable blocks. The block_writer is reused across blocks to minimize
  allocation overhead.
*/
struct block_writer {
	byte *buf;
	uint32_t block_size;

	/* Offset ofof the global header. Nonzero in the first block only. */
	uint32_t header_off;

	/* How often to restart keys. */
	int restart_interval;
	int hash_size;

	/* Offset of next byte to write. */
	uint32_t next;
	uint32_t *restarts;
	uint32_t restart_len;
	uint32_t restart_cap;

	struct slice last_key;
	int entries;
};

/*
  initializes the blockwriter to write `typ` entries, using `buf` as temporary
  storage. `buf` is not owned by the block_writer. */
void block_writer_init(struct block_writer *bw, byte typ, byte *buf,
		       uint32_t block_size, uint32_t header_off, int hash_size);

/*
  returns the block type (eg. 'r' for ref records.
*/
byte block_writer_type(struct block_writer *bw);

/* appends the record, or -1 if it doesn't fit. */
int block_writer_add(struct block_writer *w, struct record rec);

/* appends the key restarts, and compress the block if necessary. */
int block_writer_finish(struct block_writer *w);

/* clears out internally allocated block_writer members. */
void block_writer_clear(struct block_writer *bw);

/* Read a block. */
struct block_reader {
	/* offset of the block header; nonzero for the first block in a
	 * reftable. */
	uint32_t header_off;

	/* the memory block */
	struct reftable_block block;
	int hash_size;

	/* size of the data, excluding restart data. */
	uint32_t block_len;
	byte *restart_bytes;
	uint16_t restart_count;

	/* size of the data in the file. For log blocks, this is the compressed
	 * size. */
	uint32_t full_block_size;
};

/* Iterate over entries in a block */
struct block_iter {
	/* offset within the block of the next entry to read. */
	uint32_t next_off;
	struct block_reader *br;

	/* key for last entry we read. */
	struct slice last_key;
};

/* initializes a block reader */
int block_reader_init(struct block_reader *br, struct reftable_block *bl,
		      uint32_t header_off, uint32_t table_block_size,
		      int hash_size);

/* Position `it` at start of the block */
void block_reader_start(struct block_reader *br, struct block_iter *it);

/* Position `it` to the `want` key in the block */
int block_reader_seek(struct block_reader *br, struct block_iter *it,
		      struct slice want);

/* Returns the block type (eg. 'r' for refs) */
byte block_reader_type(struct block_reader *r);

/* Decodes the first key in the block */
int block_reader_first_key(struct block_reader *br, struct slice *key);

void block_iter_copy_from(struct block_iter *dest, struct block_iter *src);
int block_iter_next(struct block_iter *it, struct record rec);

/* Seek to `want` with in the block pointed to by `it` */
int block_iter_seek(struct block_iter *it, struct slice want);

/* deallocate memory for `it`. The block reader and its block is left intact. */
void block_iter_close(struct block_iter *it);

/* size of file header, depending on format version */
int header_size(int version);

/* size of file footer, depending on format version */
int footer_size(int version);

#endif
