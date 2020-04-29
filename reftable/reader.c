/*
Copyright 2020 Google LLC

Use of this source code is governed by a BSD-style
license that can be found in the LICENSE file or at
https://developers.google.com/open-source/licenses/bsd
*/

#include "reader.h"

#include "system.h"

#include "block.h"
#include "constants.h"
#include "iter.h"
#include "record.h"
#include "reftable.h"
#include "tree.h"

uint64_t block_source_size(struct reftable_block_source source)
{
	return source.ops->size(source.arg);
}

int block_source_read_block(struct reftable_block_source source,
			    struct reftable_block *dest, uint64_t off,
			    uint32_t size)
{
	int result = source.ops->read_block(source.arg, dest, off, size);
	dest->source = source;
	return result;
}

void block_source_return_block(struct reftable_block_source source,
			       struct reftable_block *blockp)
{
	source.ops->return_block(source.arg, blockp);
	blockp->data = NULL;
	blockp->len = 0;
	blockp->source.ops = NULL;
	blockp->source.arg = NULL;
}

void block_source_close(struct reftable_block_source *source)
{
	if (source->ops == NULL) {
		return;
	}

	source->ops->close(source->arg);
	source->ops = NULL;
}

static struct reftable_reader_offsets *
reader_offsets_for(struct reftable_reader *r, byte typ)
{
	switch (typ) {
	case BLOCK_TYPE_REF:
		return &r->ref_offsets;
	case BLOCK_TYPE_LOG:
		return &r->log_offsets;
	case BLOCK_TYPE_OBJ:
		return &r->obj_offsets;
	}
	abort();
}

static int reader_get_block(struct reftable_reader *r,
			    struct reftable_block *dest, uint64_t off,
			    uint32_t sz)
{
	if (off >= r->size) {
		return 0;
	}

	if (off + sz > r->size) {
		sz = r->size - off;
	}

	return block_source_read_block(r->source, dest, off, sz);
}

void reader_return_block(struct reftable_reader *r, struct reftable_block *p)
{
	block_source_return_block(r->source, p);
}

uint32_t reftable_reader_hash_id(struct reftable_reader *r)
{
	return r->hash_id;
}

const char *reader_name(struct reftable_reader *r)
{
	return r->name;
}

static int parse_footer(struct reftable_reader *r, byte *footer, byte *header)
{
	byte *f = footer;
	int err = 0;
	if (memcmp(f, "REFT", 4)) {
		err = REFTABLE_FORMAT_ERROR;
		goto exit;
	}
	f += 4;

	if (memcmp(footer, header, header_size(r->version))) {
		err = REFTABLE_FORMAT_ERROR;
		goto exit;
	}

	f++;
	r->block_size = get_be24(f);

	f += 3;
	r->min_update_index = get_be64(f);
	f += 8;
	r->max_update_index = get_be64(f);
	f += 8;

	if (r->version == 1) {
		r->hash_id = SHA1_ID;
	} else {
		r->hash_id = get_be32(f);
		switch (r->hash_id) {
		case SHA1_ID:
			break;
		case SHA256_ID:
			break;
		default:
			err = REFTABLE_FORMAT_ERROR;
			goto exit;
		}
		f += 4;
	}

	r->ref_offsets.index_offset = get_be64(f);
	f += 8;

	r->obj_offsets.offset = get_be64(f);
	f += 8;

	r->object_id_len = r->obj_offsets.offset & ((1 << 5) - 1);
	r->obj_offsets.offset >>= 5;

	r->obj_offsets.index_offset = get_be64(f);
	f += 8;
	r->log_offsets.offset = get_be64(f);
	f += 8;
	r->log_offsets.index_offset = get_be64(f);
	f += 8;

	{
		uint32_t computed_crc = crc32(0, footer, f - footer);
		uint32_t file_crc = get_be32(f);
		f += 4;
		if (computed_crc != file_crc) {
			err = REFTABLE_FORMAT_ERROR;
			goto exit;
		}
	}

	{
		byte first_block_typ = header[header_size(r->version)];
		r->ref_offsets.present = (first_block_typ == BLOCK_TYPE_REF);
		r->ref_offsets.offset = 0;
		r->log_offsets.present = (first_block_typ == BLOCK_TYPE_LOG ||
					  r->log_offsets.offset > 0);
		r->obj_offsets.present = r->obj_offsets.offset > 0;
	}
	err = 0;
exit:
	return err;
}

int init_reader(struct reftable_reader *r, struct reftable_block_source source,
		const char *name)
{
	struct reftable_block footer = { 0 };
	struct reftable_block header = { 0 };
	int err = 0;

	memset(r, 0, sizeof(struct reftable_reader));

	/* Need +1 to read type of first block. */
	err = block_source_read_block(source, &header, 0, header_size(2) + 1);
	if (err != header_size(2) + 1) {
		err = REFTABLE_IO_ERROR;
		goto exit;
	}

	if (memcmp(header.data, "REFT", 4)) {
		err = REFTABLE_FORMAT_ERROR;
		goto exit;
	}
	r->version = header.data[4];
	if (r->version != 1 && r->version != 2) {
		err = REFTABLE_FORMAT_ERROR;
		goto exit;
	}

	r->size = block_source_size(source) - footer_size(r->version);
	r->source = source;
	r->name = xstrdup(name);
	r->hash_id = 0;

	err = block_source_read_block(source, &footer, r->size,
				      footer_size(r->version));
	if (err != footer_size(r->version)) {
		err = REFTABLE_IO_ERROR;
		goto exit;
	}

	err = parse_footer(r, footer.data, header.data);
exit:
	block_source_return_block(r->source, &footer);
	block_source_return_block(r->source, &header);
	return err;
}

struct table_iter {
	struct reftable_reader *r;
	byte typ;
	uint64_t block_off;
	struct block_iter bi;
	bool finished;
};

static void table_iter_copy_from(struct table_iter *dest,
				 struct table_iter *src)
{
	dest->r = src->r;
	dest->typ = src->typ;
	dest->block_off = src->block_off;
	dest->finished = src->finished;
	block_iter_copy_from(&dest->bi, &src->bi);
}

static int table_iter_next_in_block(struct table_iter *ti, struct record rec)
{
	int res = block_iter_next(&ti->bi, rec);
	if (res == 0 && record_type(rec) == BLOCK_TYPE_REF) {
		((struct reftable_ref_record *)rec.data)->update_index +=
			ti->r->min_update_index;
	}

	return res;
}

static void table_iter_block_done(struct table_iter *ti)
{
	if (ti->bi.br == NULL) {
		return;
	}
	reader_return_block(ti->r, &ti->bi.br->block);
	FREE_AND_NULL(ti->bi.br);

	ti->bi.last_key.len = 0;
	ti->bi.next_off = 0;
}

static int32_t extract_block_size(byte *data, byte *typ, uint64_t off,
				  int version)
{
	int32_t result = 0;

	if (off == 0) {
		data += header_size(version);
	}

	*typ = data[0];
	if (is_block_type(*typ)) {
		result = get_be24(data + 1);
	}
	return result;
}

int reader_init_block_reader(struct reftable_reader *r, struct block_reader *br,
			     uint64_t next_off, byte want_typ)
{
	int32_t guess_block_size = r->block_size ? r->block_size :
						   DEFAULT_BLOCK_SIZE;
	struct reftable_block block = { 0 };
	byte block_typ = 0;
	int err = 0;
	uint32_t header_off = next_off ? 0 : header_size(r->version);
	int32_t block_size = 0;

	if (next_off >= r->size) {
		return 1;
	}

	err = reader_get_block(r, &block, next_off, guess_block_size);
	if (err < 0) {
		return err;
	}

	block_size = extract_block_size(block.data, &block_typ, next_off,
					r->version);
	if (block_size < 0) {
		return block_size;
	}

	if (want_typ != BLOCK_TYPE_ANY && block_typ != want_typ) {
		reader_return_block(r, &block);
		return 1;
	}

	if (block_size > guess_block_size) {
		reader_return_block(r, &block);
		err = reader_get_block(r, &block, next_off, block_size);
		if (err < 0) {
			return err;
		}
	}

	return block_reader_init(br, &block, header_off, r->block_size,
				 hash_size(r->hash_id));
}

static int table_iter_next_block(struct table_iter *dest,
				 struct table_iter *src)
{
	uint64_t next_block_off = src->block_off + src->bi.br->full_block_size;
	struct block_reader br = { 0 };
	int err = 0;

	dest->r = src->r;
	dest->typ = src->typ;
	dest->block_off = next_block_off;

	err = reader_init_block_reader(src->r, &br, next_block_off, src->typ);
	if (err > 0) {
		dest->finished = true;
		return 1;
	}
	if (err != 0) {
		return err;
	}

	{
		struct block_reader *brp =
			reftable_malloc(sizeof(struct block_reader));
		*brp = br;

		dest->finished = false;
		block_reader_start(brp, &dest->bi);
	}
	return 0;
}

static int table_iter_next(struct table_iter *ti, struct record rec)
{
	if (record_type(rec) != ti->typ) {
		return REFTABLE_API_ERROR;
	}

	while (true) {
		struct table_iter next = { 0 };
		int err = 0;
		if (ti->finished) {
			return 1;
		}

		err = table_iter_next_in_block(ti, rec);
		if (err <= 0) {
			return err;
		}

		err = table_iter_next_block(&next, ti);
		if (err != 0) {
			ti->finished = true;
		}
		table_iter_block_done(ti);
		if (err != 0) {
			return err;
		}
		table_iter_copy_from(ti, &next);
		block_iter_close(&next.bi);
	}
}

static int table_iter_next_void(void *ti, struct record rec)
{
	return table_iter_next((struct table_iter *)ti, rec);
}

static void table_iter_close(void *p)
{
	struct table_iter *ti = (struct table_iter *)p;
	table_iter_block_done(ti);
	block_iter_close(&ti->bi);
}

struct reftable_iterator_vtable table_iter_vtable = {
	.next = &table_iter_next_void,
	.close = &table_iter_close,
};

static void iterator_from_table_iter(struct reftable_iterator *it,
				     struct table_iter *ti)
{
	it->iter_arg = ti;
	it->ops = &table_iter_vtable;
}

static int reader_table_iter_at(struct reftable_reader *r,
				struct table_iter *ti, uint64_t off, byte typ)
{
	struct block_reader br = { 0 };
	struct block_reader *brp = NULL;

	int err = reader_init_block_reader(r, &br, off, typ);
	if (err != 0) {
		return err;
	}

	brp = reftable_malloc(sizeof(struct block_reader));
	*brp = br;
	ti->r = r;
	ti->typ = block_reader_type(brp);
	ti->block_off = off;
	block_reader_start(brp, &ti->bi);
	return 0;
}

static int reader_start(struct reftable_reader *r, struct table_iter *ti,
			byte typ, bool index)
{
	struct reftable_reader_offsets *offs = reader_offsets_for(r, typ);
	uint64_t off = offs->offset;
	if (index) {
		off = offs->index_offset;
		if (off == 0) {
			return 1;
		}
		typ = BLOCK_TYPE_INDEX;
	}

	return reader_table_iter_at(r, ti, off, typ);
}

static int reader_seek_linear(struct reftable_reader *r, struct table_iter *ti,
			      struct record want)
{
	struct record rec = new_record(record_type(want));
	struct slice want_key = { 0 };
	struct slice got_key = { 0 };
	struct table_iter next = { 0 };
	int err = -1;
	record_key(want, &want_key);

	while (true) {
		err = table_iter_next_block(&next, ti);
		if (err < 0) {
			goto exit;
		}

		if (err > 0) {
			break;
		}

		err = block_reader_first_key(next.bi.br, &got_key);
		if (err < 0) {
			goto exit;
		}
		{
			int cmp = slice_compare(got_key, want_key);
			if (cmp > 0) {
				table_iter_block_done(&next);
				break;
			}
		}

		table_iter_block_done(ti);
		table_iter_copy_from(ti, &next);
	}

	err = block_iter_seek(&ti->bi, want_key);
	if (err < 0) {
		goto exit;
	}
	err = 0;

exit:
	block_iter_close(&next.bi);
	record_destroy(&rec);
	slice_clear(&want_key);
	slice_clear(&got_key);
	return err;
}

static int reader_seek_indexed(struct reftable_reader *r,
			       struct reftable_iterator *it, struct record rec)
{
	struct index_record want_index = { 0 };
	struct record want_index_rec = { 0 };
	struct index_record index_result = { 0 };
	struct record index_result_rec = { 0 };
	struct table_iter index_iter = { 0 };
	struct table_iter next = { 0 };
	int err = 0;

	record_key(rec, &want_index.last_key);
	record_from_index(&want_index_rec, &want_index);
	record_from_index(&index_result_rec, &index_result);

	err = reader_start(r, &index_iter, record_type(rec), true);
	if (err < 0) {
		goto exit;
	}

	err = reader_seek_linear(r, &index_iter, want_index_rec);
	while (true) {
		err = table_iter_next(&index_iter, index_result_rec);
		table_iter_block_done(&index_iter);
		if (err != 0) {
			goto exit;
		}

		err = reader_table_iter_at(r, &next, index_result.offset, 0);
		if (err != 0) {
			goto exit;
		}

		err = block_iter_seek(&next.bi, want_index.last_key);
		if (err < 0) {
			goto exit;
		}

		if (next.typ == record_type(rec)) {
			err = 0;
			break;
		}

		if (next.typ != BLOCK_TYPE_INDEX) {
			err = REFTABLE_FORMAT_ERROR;
			break;
		}

		table_iter_copy_from(&index_iter, &next);
	}

	if (err == 0) {
		struct table_iter *malloced =
			reftable_calloc(sizeof(struct table_iter));
		table_iter_copy_from(malloced, &next);
		iterator_from_table_iter(it, malloced);
	}
exit:
	block_iter_close(&next.bi);
	table_iter_close(&index_iter);
	record_clear(want_index_rec);
	record_clear(index_result_rec);
	return err;
}

static int reader_seek_internal(struct reftable_reader *r,
				struct reftable_iterator *it, struct record rec)
{
	struct reftable_reader_offsets *offs =
		reader_offsets_for(r, record_type(rec));
	uint64_t idx = offs->index_offset;
	struct table_iter ti = { 0 };
	int err = 0;
	if (idx > 0) {
		return reader_seek_indexed(r, it, rec);
	}

	err = reader_start(r, &ti, record_type(rec), false);
	if (err < 0) {
		return err;
	}
	err = reader_seek_linear(r, &ti, rec);
	if (err < 0) {
		return err;
	}

	{
		struct table_iter *p =
			reftable_malloc(sizeof(struct table_iter));
		*p = ti;
		iterator_from_table_iter(it, p);
	}

	return 0;
}

int reader_seek(struct reftable_reader *r, struct reftable_iterator *it,
		struct record rec)
{
	byte typ = record_type(rec);

	struct reftable_reader_offsets *offs = reader_offsets_for(r, typ);
	if (!offs->present) {
		iterator_set_empty(it);
		return 0;
	}

	return reader_seek_internal(r, it, rec);
}

int reftable_reader_seek_ref(struct reftable_reader *r,
			     struct reftable_iterator *it, const char *name)
{
	struct reftable_ref_record ref = {
		.ref_name = (char *)name,
	};
	struct record rec = { 0 };
	record_from_ref(&rec, &ref);
	return reader_seek(r, it, rec);
}

int reftable_reader_seek_log_at(struct reftable_reader *r,
				struct reftable_iterator *it, const char *name,
				uint64_t update_index)
{
	struct reftable_log_record log = {
		.ref_name = (char *)name,
		.update_index = update_index,
	};
	struct record rec = { 0 };
	record_from_log(&rec, &log);
	return reader_seek(r, it, rec);
}

int reftable_reader_seek_log(struct reftable_reader *r,
			     struct reftable_iterator *it, const char *name)
{
	uint64_t max = ~((uint64_t)0);
	return reftable_reader_seek_log_at(r, it, name, max);
}

void reader_close(struct reftable_reader *r)
{
	block_source_close(&r->source);
	FREE_AND_NULL(r->name);
}

int reftable_new_reader(struct reftable_reader **p,
			struct reftable_block_source src, char const *name)
{
	struct reftable_reader *rd =
		reftable_calloc(sizeof(struct reftable_reader));
	int err = init_reader(rd, src, name);
	if (err == 0) {
		*p = rd;
	} else {
		reftable_free(rd);
	}
	return err;
}

void reftable_reader_free(struct reftable_reader *r)
{
	reader_close(r);
	reftable_free(r);
}

static int reftable_reader_refs_for_indexed(struct reftable_reader *r,
					    struct reftable_iterator *it,
					    byte *oid)
{
	struct obj_record want = {
		.hash_prefix = oid,
		.hash_prefix_len = r->object_id_len,
	};
	struct record want_rec = { 0 };
	struct reftable_iterator oit = { 0 };
	struct obj_record got = { 0 };
	struct record got_rec = { 0 };
	int err = 0;

	record_from_obj(&want_rec, &want);

	err = reader_seek(r, &oit, want_rec);
	if (err != 0) {
		return err;
	}

	record_from_obj(&got_rec, &got);
	err = iterator_next(oit, got_rec);
	reftable_iterator_destroy(&oit);
	if (err < 0) {
		return err;
	}

	if (err > 0 ||
	    memcmp(want.hash_prefix, got.hash_prefix, r->object_id_len)) {
		iterator_set_empty(it);
		return 0;
	}

	{
		struct indexed_table_ref_iter *itr = NULL;
		err = new_indexed_table_ref_iter(&itr, r, oid,
						 hash_size(r->hash_id),
						 got.offsets, got.offset_len);
		if (err < 0) {
			record_clear(got_rec);
			return err;
		}
		got.offsets = NULL;
		record_clear(got_rec);

		iterator_from_indexed_table_ref_iter(it, itr);
	}

	return 0;
}

static int reftable_reader_refs_for_unindexed(struct reftable_reader *r,
					      struct reftable_iterator *it,
					      byte *oid, int oid_len)
{
	struct table_iter *ti = reftable_calloc(sizeof(struct table_iter));
	struct filtering_ref_iterator *filter = NULL;
	int err = reader_start(r, ti, BLOCK_TYPE_REF, false);
	if (err < 0) {
		reftable_free(ti);
		return err;
	}

	filter = reftable_calloc(sizeof(struct filtering_ref_iterator));
	slice_resize(&filter->oid, oid_len);
	memcpy(filter->oid.buf, oid, oid_len);
	filter->r = r;
	filter->double_check = false;
	iterator_from_table_iter(&filter->it, ti);

	iterator_from_filtering_ref_iterator(it, filter);
	return 0;
}

int reftable_reader_refs_for(struct reftable_reader *r,
			     struct reftable_iterator *it, byte *oid,
			     int oid_len)
{
	if (r->obj_offsets.present) {
		return reftable_reader_refs_for_indexed(r, it, oid);
	}
	return reftable_reader_refs_for_unindexed(r, it, oid, oid_len);
}

uint64_t reftable_reader_max_update_index(struct reftable_reader *r)
{
	return r->max_update_index;
}

uint64_t reftable_reader_min_update_index(struct reftable_reader *r)
{
	return r->min_update_index;
}
