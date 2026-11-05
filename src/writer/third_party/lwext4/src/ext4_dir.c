/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 *
 *
 * HelenOS:
 * Copyright (c) 2012 Martin Sucha
 * Copyright (c) 2012 Frantisek Princ
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_dir.h
 * @brief Directory handle procedures.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_trans.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <ext4_crc32.h>
#include <ext4_casefold.h>
#include <ext4_inode.h>
#include <ext4_fs.h>
#include <ext4_extent.h>

#include <stdlib.h>
#include <string.h>

/****************************************************************************/

/* Walk through a dirent block to find a checksum "dirent" at the tail */
static struct ext4_dir_entry_tail *
ext4_dir_get_tail(struct ext4_inode_ref *inode_ref,
		struct ext4_dir_en *de)
{
	struct ext4_dir_entry_tail *t;
	struct ext4_sblock *sb = &inode_ref->fs->sb;

	t = EXT4_DIRENT_TAIL(de, ext4_sb_get_block_size(sb));

	if (t->reserved_zero1 || t->reserved_zero2)
		return NULL;
	if (to_le16(t->rec_len) != sizeof(struct ext4_dir_entry_tail))
		return NULL;
	if (t->reserved_ft != EXT4_DIRENTRY_DIR_CSUM)
		return NULL;

	return t;
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t ext4_dir_csum(struct ext4_inode_ref *inode_ref,
			      struct ext4_dir_en *dirent, int size)
{
	uint32_t csum;
	struct ext4_sblock *sb = &inode_ref->fs->sb;
	uint32_t ino_index = to_le32(inode_ref->index);
	uint32_t ino_gen = to_le32(ext4_inode_get_generation(inode_ref->inode));

	csum = ext4_sb_get_csum_seed(sb);
	/* Then calculate crc32 checksum against inode number
	 * and inode generation */
	csum = ext4_crc32c(csum, &ino_index, sizeof(ino_index));
	csum = ext4_crc32c(csum, &ino_gen, sizeof(ino_gen));
	/* Finally calculate crc32 checksum against directory entries */
	csum = ext4_crc32c(csum, dirent, size);
	return csum;
}
#else
#define ext4_dir_csum(...) 0
#endif

bool ext4_dir_csum_verify(struct ext4_inode_ref *inode_ref,
			      struct ext4_dir_en *dirent)
{
#ifdef CONFIG_META_CSUM_ENABLE
	struct ext4_dir_entry_tail *t;
	struct ext4_sblock *sb = &inode_ref->fs->sb;

	/* Compute the checksum only if the filesystem supports it */
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		t = ext4_dir_get_tail(inode_ref, dirent);
		if (!t) {
			/* There is no space to hold the checksum */
			return false;
		}

		ptrdiff_t __unused diff = (char *)t - (char *)dirent;
		uint32_t csum = ext4_dir_csum(inode_ref, dirent, diff);
		if (t->checksum != to_le32(csum))
			return false;

	}
#endif
	return true;
}

void ext4_dir_init_entry_tail(struct ext4_dir_entry_tail *t)
{
	memset(t, 0, sizeof(struct ext4_dir_entry_tail));
	t->rec_len = to_le16(sizeof(struct ext4_dir_entry_tail));
	t->reserved_ft = EXT4_DIRENTRY_DIR_CSUM;
}

void ext4_dir_set_csum(struct ext4_inode_ref *inode_ref,
			   struct ext4_dir_en *dirent)
{
	struct ext4_dir_entry_tail *t;
	struct ext4_sblock *sb = &inode_ref->fs->sb;

	/* Compute the checksum only if the filesystem supports it */
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		t = ext4_dir_get_tail(inode_ref, dirent);
		if (!t) {
			/* There is no space to hold the checksum */
			return;
		}

		ptrdiff_t __unused diff = (char *)t - (char *)dirent;
		uint32_t csum = ext4_dir_csum(inode_ref, dirent, diff);
		t->checksum = to_le32(csum);
	}
}

/**@brief Do some checks before returning iterator.
 * @param it Iterator to be checked
 * @param block_size Size of data block
 * @return Error code
 */
static int ext4_dir_iterator_set(struct ext4_dir_iter *it,
				 uint32_t block_size)
{
	uint32_t off_in_block = it->curr_off % block_size;
	struct ext4_sblock *sb = &it->inode_ref->fs->sb;

	it->curr = NULL;

	/* Ensure proper alignment */
	if ((off_in_block % 4) != 0)
		return EIO;

	/* Ensure that the core of the entry does not overflow the block */
	if (off_in_block > block_size - 8)
		return EIO;

	struct ext4_dir_en *en;
	en = (void *)(it->curr_blk.data + off_in_block);

	/* Ensure that the whole entry does not overflow the block */
	uint32_t length = ext4_dir_en_get_entry_len(en, block_size);
	if (off_in_block + length > block_size)
		return EIO;

	/* Ensure the name length is not too large */
	if (ext4_dir_en_get_name_len(sb, en) > length - 8)
		return EIO;

	/* Everything OK - "publish" the entry */
	it->curr = en;
	return EOK;
}

static int ext4_dir_iterator_inline_seek(struct ext4_dir_iter *it,
					 uint64_t pos)
{
	struct ext4_inode *inode = it->inode_ref->inode;
	struct ext4_sblock *sb = &it->inode_ref->fs->sb;
	uint32_t name_len;
	uint32_t rec_len;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint8_t *entry;
	uint64_t total_size = 80 + it->inline_extra_size;

	it->curr = NULL;
	if (pos >= total_size) {
		it->curr_off = pos;
		return EOK;
	}
	if (pos == 0 || pos == 12) {
		struct ext4_dir_en *dot = (struct ext4_dir_en *)it->inline_dot;
		memset(it->inline_dot, 0, sizeof(it->inline_dot));
		ext4_dir_en_set_inode(dot, pos == 0 ? it->inode_ref->index :
				      to_le32(inode->blocks[0]));
		ext4_dir_en_set_entry_len(dot, 12, block_size);
		ext4_dir_en_set_name_len(sb, dot, pos == 0 ? 1 : 2);
		ext4_dir_en_set_inode_type(sb, dot, EXT4_DE_DIR);
		dot->name[0] = '.';
		if (pos == 12)
			dot->name[1] = '.';
		it->curr_off = pos;
		it->curr = dot;
		return EOK;
	}
	if (pos < 24)
		return EIO;
	if (pos < 80) {
		uint64_t offset = pos - 24;
		if (offset >= sizeof(inode->blocks) - sizeof(uint32_t)) {
			pos = 80;
			return ext4_dir_iterator_inline_seek(it, pos);
		}
		entry = (uint8_t *)inode->blocks + sizeof(uint32_t) + offset;
		rec_len = ext4_dir_en_get_entry_len((struct ext4_dir_en *)entry,
						    block_size);
		if (!rec_len && offset == 0) {
			uint64_t index;
			for (index = 0;
			     index < sizeof(inode->blocks) - sizeof(uint32_t);
			     ++index) {
				if (entry[index])
					return EIO;
			}
			pos = 80;
			return ext4_dir_iterator_inline_seek(it, pos);
		}
		if (rec_len < 8 || rec_len > 56 - offset || (rec_len & 3))
			return EIO;
	} else {
		uint64_t offset = pos - 80;
		if (offset >= it->inline_extra_size) {
			it->curr_off = pos;
			return EOK;
		}
		entry = it->inline_extra + offset;
		if (it->inline_extra_size - offset < 8)
			return EIO;
		rec_len = ext4_dir_en_get_entry_len((struct ext4_dir_en *)entry,
						    block_size);
		if (rec_len < 8 || rec_len > it->inline_extra_size - offset ||
		    (rec_len & 3))
			return EIO;
	}
	name_len = ext4_dir_en_get_name_len(sb, (struct ext4_dir_en *)entry);
	if (name_len > rec_len - 8)
		return EIO;
	it->curr_off = pos;
	it->curr = (struct ext4_dir_en *)entry;
	return EOK;
}

static int ext4_dir_inline_copy_segment(struct ext4_sblock *sb,
					const uint8_t *source, uint32_t size,
					uint8_t *destination, uint32_t *used,
					uint32_t limit)
{
	uint32_t offset = 0;
	uint32_t block_size = ext4_sb_get_block_size(sb);

	while (offset < size) {
		struct ext4_dir_en *entry = (struct ext4_dir_en *)(source + offset);
		uint32_t remaining = size - offset;
		uint32_t record_length;
		uint16_t name_length;
		uint32_t compact_length;

		if (remaining < 8)
			return EIO;
		record_length = ext4_dir_en_get_entry_len(entry, block_size);
		if (!record_length) {
			uint32_t index;
			for (index = offset; index < size; ++index) {
				if (source[index])
					return EIO;
			}
			break;
		}
		if (record_length < 8 || record_length > remaining ||
		    (record_length & 3))
			return EIO;
		name_length = ext4_dir_en_get_name_len(sb, entry);
		if (name_length > record_length - 8)
			return EIO;
		if (ext4_dir_en_get_inode(entry)) {
			compact_length = (8 + name_length + 3) & ~3U;
			if (compact_length > limit - *used)
				return ENOSPC;
			memcpy(destination + *used, entry, compact_length);
			ext4_dir_en_set_entry_len(
				(struct ext4_dir_en *)(destination + *used),
				compact_length, block_size);
			*used += compact_length;
		}
		offset += record_length;
	}
	return EOK;
}

int ext4_dir_inline_data_expand(struct ext4_inode_ref *parent)
{
	struct ext4_sblock *sb = &parent->fs->sb;
	struct ext4_inode *inode = parent->inode;
	struct ext4_inode_ref dotdot_ref;
	struct ext4_block block;
	uint8_t *extra;
	uint8_t *directory;
	uint32_t extra_size;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint32_t checksum_size = ext4_sb_feature_ro_com(
		sb, EXT4_FRO_COM_METADATA_CSUM) ?
		sizeof(struct ext4_dir_entry_tail) : 0;
	uint32_t limit = block_size - checksum_size;
	uint32_t used = 0;
	uint32_t parent_inode;
	uint32_t dot_length = 12;
	uint32_t dotdot_length;
	uint8_t *last_entry;
	ext4_fsblk_t fblock;
	ext4_lblk_t iblock;
	int rc;

	if (!ext4_inode_has_flag(inode, EXT4_INODE_FLAG_INLINE_DATA))
		return EOK;
	if (!ext4_inode_is_type(sb, inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;
	rc = ext4_inode_inline_data_get(parent, &extra, &extra_size);
	if (rc != EOK)
		return rc;
	directory = ext4_malloc(block_size);
	if (!directory)
		return ENOMEM;
	memset(directory, 0, block_size);
	parent_inode = to_le32(inode->blocks[0]);
	if (!parent_inode) {
		ext4_free(directory);
		return EIO;
	}
	rc = ext4_fs_get_inode_ref(parent->fs, parent_inode, &dotdot_ref);
	if (rc != EOK) {
		ext4_free(directory);
		return rc;
	}
	struct ext4_dir_en *dot = (struct ext4_dir_en *)directory;
	ext4_dir_write_entry(sb, dot, dot_length, parent, ".", 1);
	used = dot_length;
	struct ext4_dir_en *dotdot = (struct ext4_dir_en *)(directory + used);
	dotdot_length = limit - used;
	if (dotdot_length < 12) {
		ext4_fs_put_inode_ref(&dotdot_ref);
		ext4_free(directory);
		return ENOSPC;
	}
	ext4_dir_write_entry(sb, dotdot, 12, &dotdot_ref, "..", 2);
	used += 12;
	last_entry = (uint8_t *)dotdot;
	rc = ext4_dir_inline_copy_segment(
		sb, (const uint8_t *)inode->blocks + sizeof(uint32_t),
		sizeof(inode->blocks) - sizeof(uint32_t), directory, &used, limit);
	if (rc == EOK && extra_size)
		rc = ext4_dir_inline_copy_segment(sb, extra, extra_size, directory,
						  &used, limit);
	if (rc == EOK) {
		struct ext4_dir_en *entry = (struct ext4_dir_en *)directory;
		uint32_t offset = 0;
		while (offset < used) {
			last_entry = (uint8_t *)entry;
			offset += ext4_dir_en_get_entry_len(entry, block_size);
			entry = (struct ext4_dir_en *)(directory + offset);
		}
		ext4_dir_en_set_entry_len((struct ext4_dir_en *)last_entry,
					  limit - (uint32_t)(last_entry - directory),
					  block_size);
		if (checksum_size)
			ext4_dir_init_entry_tail(
				EXT4_DIRENT_TAIL(directory, block_size));
	}
	ext4_fs_put_inode_ref(&dotdot_ref);
	if (rc != EOK) {
		ext4_free(directory);
		return rc;
	}
	rc = ext4_inode_inline_data_remove(parent);
	if (rc != EOK)
		goto Finish;
	memset(inode->blocks, 0, sizeof(inode->blocks));
	ext4_inode_clear_flag(inode, EXT4_INODE_FLAG_INLINE_DATA);
	ext4_inode_set_size(inode, 0);
#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE
	if (ext4_sb_feature_incom(sb, EXT4_FINCOM_EXTENTS)) {
		ext4_inode_set_flag(inode, EXT4_INODE_FLAG_EXTENTS);
		ext4_extent_tree_init(parent);
	}
#endif
	parent->dirty = true;
	rc = ext4_fs_append_inode_dblk(parent, &fblock, &iblock);
	if (rc != EOK)
		goto Finish;
	rc = ext4_trans_block_get_noread(parent->fs->bdev, &block, fblock);
	if (rc != EOK)
		goto Finish;
	memcpy(block.data, directory, block_size);
	ext4_dir_set_csum(parent, (struct ext4_dir_en *)block.data);
	ext4_trans_set_block_dirty(block.buf);
	rc = ext4_block_set(parent->fs->bdev, &block);
	if (rc == EOK)
		parent->dirty = true;

Finish:
	ext4_free(directory);
	return rc;
}

/**@brief Seek to next valid directory entry.
 *        Here can be jumped to the next data block.
 * @param it  Initialized iterator
 * @param pos Position of the next entry
 * @return Error code
 */
static int ext4_dir_iterator_seek(struct ext4_dir_iter *it, uint64_t pos)
{
	struct ext4_sblock *sb = &it->inode_ref->fs->sb;
	struct ext4_inode *inode = it->inode_ref->inode;
	struct ext4_blockdev *bdev = it->inode_ref->fs->bdev;
	uint64_t size = ext4_inode_get_size(sb, inode);
	int r;
	if (it->inline_data)
		return ext4_dir_iterator_inline_seek(it, pos);

	/* The iterator is not valid until we seek to the desired position */
	it->curr = NULL;

	/* Are we at the end? */
	if (pos >= size) {
		if (it->curr_blk.lb_id) {

			r = ext4_block_set(bdev, &it->curr_blk);
			it->curr_blk.lb_id = 0;
			if (r != EOK)
				return r;
		}

		it->curr_off = pos;
		return EOK;
	}

	/* Compute next block address */
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint64_t current_blk_idx = it->curr_off / block_size;
	uint32_t next_blk_idx = (uint32_t)(pos / block_size);

	/*
	 * If we don't have a block or are moving across block boundary,
	 * we need to get another block
	 */
	if ((it->curr_blk.lb_id == 0) ||
	    (current_blk_idx != next_blk_idx)) {
		if (it->curr_blk.lb_id) {
			r = ext4_block_set(bdev, &it->curr_blk);
			it->curr_blk.lb_id = 0;

			if (r != EOK)
				return r;
		}

		ext4_fsblk_t next_blk;
		r = ext4_fs_get_inode_dblk_idx(it->inode_ref, next_blk_idx,
					       &next_blk, false);
		if (r != EOK)
			return r;

		r = ext4_trans_block_get(bdev, &it->curr_blk, next_blk);
		if (r != EOK) {
			it->curr_blk.lb_id = 0;
			return r;
		}
	}

	it->curr_off = pos;
	return ext4_dir_iterator_set(it, block_size);
}

int ext4_dir_iterator_init(struct ext4_dir_iter *it,
			   struct ext4_inode_ref *inode_ref, uint64_t pos)
{
	int r;
	it->inode_ref = inode_ref;
	it->curr = 0;
	it->curr_off = 0;
	it->curr_blk.lb_id = 0;
	it->inline_data = ext4_inode_has_flag(inode_ref->inode,
						      EXT4_INODE_FLAG_INLINE_DATA);
	it->inline_extra = NULL;
	it->inline_extra_size = 0;
	if (it->inline_data) {
		r = ext4_inode_inline_data_get(inode_ref, &it->inline_extra,
					       &it->inline_extra_size);
		if (r != EOK)
			return r;
	}

	return ext4_dir_iterator_seek(it, pos);
}

int ext4_dir_iterator_next(struct ext4_dir_iter *it)
{
	int r = EOK;
	uint32_t skip;

	while (r == EOK) {
		skip = ext4_dir_en_get_entry_len(
			it->curr, ext4_sb_get_block_size(&it->inode_ref->fs->sb));
		r = ext4_dir_iterator_seek(it, it->curr_off + skip);

		if (!it->curr)
			break;
		/*Skip NULL referenced entry*/
		if (ext4_dir_en_get_inode(it->curr) != 0)
			break;
	}

	return r;
}

int ext4_dir_iterator_fini(struct ext4_dir_iter *it)
{
	it->curr = 0;
	if (it->inline_data)
		return EOK;

	if (it->curr_blk.lb_id)
		return ext4_block_set(it->inode_ref->fs->bdev, &it->curr_blk);

	return EOK;
}

void ext4_dir_write_entry(struct ext4_sblock *sb, struct ext4_dir_en *en,
			  uint32_t entry_len, struct ext4_inode_ref *child,
			  const char *name, size_t name_len)
{
	/* Check maximum entry length */
	ext4_assert(entry_len <= ext4_sb_get_block_size(sb));

	/* Store the inode type only when the directory format has this field. */
	if (ext4_sb_feature_incom(sb, EXT4_FINCOM_FILETYPE)) {
		switch (ext4_inode_type(sb, child->inode)) {
		case EXT4_INODE_MODE_DIRECTORY:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_DIR);
			break;
		case EXT4_INODE_MODE_FILE:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_REG_FILE);
			break;
		case EXT4_INODE_MODE_SOFTLINK:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_SYMLINK);
			break;
		case EXT4_INODE_MODE_CHARDEV:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_CHRDEV);
			break;
		case EXT4_INODE_MODE_BLOCKDEV:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_BLKDEV);
			break;
		case EXT4_INODE_MODE_FIFO:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_FIFO);
			break;
		case EXT4_INODE_MODE_SOCKET:
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_SOCK);
			break;
		default:
			/* FIXME: unsupported filetype */
			ext4_dir_en_set_inode_type(sb, en, EXT4_DE_UNKNOWN);
		}
	}

	/* Set basic attributes */
	ext4_dir_en_set_inode(en, child->index);
	ext4_dir_en_set_entry_len(en, entry_len,
				  ext4_sb_get_block_size(sb));
	ext4_dir_en_set_name_len(sb, en, (uint16_t)name_len);

	/* Write name */
	memcpy(en->name, name, name_len);
}

int ext4_dir_add_entry(struct ext4_inode_ref *parent, const char *name,
		       uint32_t name_len, struct ext4_inode_ref *child)
{
	int r;
	struct ext4_fs *fs = parent->fs;
	struct ext4_sblock *sb = &parent->fs->sb;
	if (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INLINE_DATA)) {
		r = ext4_dir_inline_data_expand(parent);
		if (r != EOK)
			return r;
	}

#if CONFIG_DIR_INDEX_ENABLE
	/* Index adding (if allowed) */
	if ((ext4_sb_feature_com(sb, EXT4_FCOM_DIR_INDEX)) &&
	    (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INDEX))) {
		r = ext4_dir_dx_add_entry(parent, child, name, name_len);

		/* Check if index is not corrupted */
		if (r != EXT4_ERR_BAD_DX_DIR) {
			if (r != EOK)
				return r;

			return EOK;
		}

		/* Needed to clear dir index flag if corrupted */
		ext4_inode_clear_flag(parent->inode, EXT4_INODE_FLAG_INDEX);
		parent->dirty = true;
	}
#endif

	/* Linear algorithm */
	uint32_t iblock = 0;
	ext4_fsblk_t fblock = 0;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint64_t inode_size = ext4_inode_get_size(sb, parent->inode);
	uint32_t total_blocks = (uint32_t)(inode_size / block_size);

	/* Find block, where is space for new entry and try to add */
	bool success = false;
	for (iblock = 0; iblock < total_blocks; ++iblock) {
		r = ext4_fs_get_inode_dblk_idx(parent, iblock, &fblock, false);
		if (r != EOK)
			return r;

		struct ext4_block block;
		r = ext4_trans_block_get(fs->bdev, &block, fblock);
		if (r != EOK)
			return r;

		if (!ext4_dir_csum_verify(parent, (void *)block.data)) {
			ext4_dbg(DEBUG_DIR,
				 DBG_WARN "Leaf block checksum failed."
				 "Inode: %" PRIu32", "
				 "Block: %" PRIu32"\n",
				 parent->index,
				 iblock);
		}

		/* If adding is successful, function can finish */
		r = ext4_dir_try_insert_entry(sb, parent, &block, child,
						name, name_len);
		if (r == EOK)
			success = true;

		r = ext4_block_set(fs->bdev, &block);
		if (r != EOK)
			return r;

		if (success)
			return EOK;
	}

	/* No free block found - needed to allocate next data block */

	iblock = 0;
	fblock = 0;
	r = ext4_fs_append_inode_dblk(parent, &fblock, &iblock);
	if (r != EOK)
		return r;

	/* Load new block */
	struct ext4_block b;

	r = ext4_trans_block_get_noread(fs->bdev, &b, fblock);
	if (r != EOK)
		return r;

	/* Fill block with zeroes */
	memset(b.data, 0, block_size);
	struct ext4_dir_en *blk_en = (void *)b.data;

	/* Save new block */
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
	uint32_t el = block_size - sizeof(struct ext4_dir_entry_tail);
		ext4_dir_write_entry(sb, blk_en, el, child, name, name_len);
		ext4_dir_init_entry_tail(EXT4_DIRENT_TAIL(b.data, block_size));
	} else {
		ext4_dir_write_entry(sb, blk_en, block_size, child, name,
				name_len);
	}

	ext4_dir_set_csum(parent, (void *)b.data);
	ext4_trans_set_block_dirty(b.buf);
	r = ext4_block_set(fs->bdev, &b);

	return r;
}

int ext4_dir_find_entry(struct ext4_dir_search_result *result,
			struct ext4_inode_ref *parent, const char *name,
			uint32_t name_len)
{
	int r;
	struct ext4_sblock *sb = &parent->fs->sb;
	int casefold = ext4_inode_has_flag(parent->inode,
					   EXT4_INODE_FLAG_CASEFOLD);

	/* Entry clear */
	result->block.lb_id = 0;
	result->dentry = NULL;

#if CONFIG_DIR_INDEX_ENABLE
	/* Index search */
	if ((ext4_sb_feature_com(sb, EXT4_FCOM_DIR_INDEX)) &&
	    (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INDEX))) {
		r = ext4_dir_dx_find_entry(result, parent, name_len, name);
		/* Check if index is not corrupted */
		if (r != EXT4_ERR_BAD_DX_DIR) {
			if (r != EOK)
				return r;

			return EOK;
		}

		/* Needed to clear dir index flag if corrupted */
		ext4_inode_clear_flag(parent->inode, EXT4_INODE_FLAG_INDEX);
		parent->dirty = true;
	}
#endif
	if (ext4_inode_has_flag(parent->inode, EXT4_INODE_FLAG_INLINE_DATA)) {
		struct ext4_dir_iter iterator;
		r = ext4_dir_iterator_init(&iterator, parent, 0);
		if (r != EOK)
			return r;
		while (iterator.curr) {
			uint16_t entry_len = ext4_dir_en_get_name_len(sb, iterator.curr);
			int equal = 0;
			if (ext4_dir_en_get_inode(iterator.curr) &&
			    ((casefold && ext4_casefold_equal(iterator.curr->name,
				entry_len, name, name_len, &equal) == 0 && equal) ||
			     (!casefold && entry_len == name_len &&
				!memcmp(iterator.curr->name, name, name_len)))) {
				result->dentry = iterator.curr;
				return ext4_dir_iterator_fini(&iterator);
			}
			r = ext4_dir_iterator_next(&iterator);
			if (r != EOK) {
				ext4_dir_iterator_fini(&iterator);
				return r;
			}
		}
		r = ext4_dir_iterator_fini(&iterator);
		return r == EOK ? ENOENT : r;
	}

	/* Linear algorithm */

	uint32_t iblock;
	ext4_fsblk_t fblock;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint64_t inode_size = ext4_inode_get_size(sb, parent->inode);
	uint32_t total_blocks = (uint32_t)(inode_size / block_size);

	/* Walk through all data blocks */
	for (iblock = 0; iblock < total_blocks; ++iblock) {
		/* Load block address */
		r = ext4_fs_get_inode_dblk_idx(parent, iblock, &fblock, false);
		if (r != EOK)
			return r;

		/* Load data block */
		struct ext4_block b;
		r = ext4_trans_block_get(parent->fs->bdev, &b, fblock);
		if (r != EOK)
			return r;

		if (!ext4_dir_csum_verify(parent, (void *)b.data)) {
			ext4_dbg(DEBUG_DIR,
				 DBG_WARN "Leaf block checksum failed."
				 "Inode: %" PRIu32", "
				 "Block: %" PRIu32"\n",
				 parent->index,
				 iblock);
		}

		/* Try to find entry in block */
		struct ext4_dir_en *res_entry;
		r = ext4_dir_find_in_block(&b, sb, name_len, name, casefold,
					   &res_entry);
		if (r == EOK) {
			result->block = b;
			result->dentry = res_entry;
			return EOK;
		}

		/* Entry not found - put block and continue to the next block */

		r = ext4_block_set(parent->fs->bdev, &b);
		if (r != EOK)
			return r;
	}

	return ENOENT;
}

int ext4_dir_remove_entry(struct ext4_inode_ref *parent, const char *name,
			  uint32_t name_len)
{
	struct ext4_sblock *sb = &parent->fs->sb;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	/* Check if removing from directory */
	if (!ext4_inode_is_type(sb, parent->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;
	int expand_rc = ext4_dir_inline_data_expand(parent);
	if (expand_rc != EOK)
		return expand_rc;

	/* Try to find entry */
	struct ext4_dir_search_result result;
	int rc = ext4_dir_find_entry(&result, parent, name, name_len);
	if (rc != EOK)
		return rc;

	/* Invalidate entry */
	ext4_dir_en_set_inode(result.dentry, 0);

	/* Store entry position in block */
	uint32_t pos = (uint8_t *)result.dentry - result.block.data;

	/*
	 * If entry is not the first in block, it must be merged
	 * with previous entry
	 */
	if (pos != 0) {
		uint32_t offset = 0;

		/* Start from the first entry in block */
		struct ext4_dir_en *tmp_de =(void *)result.block.data;
		uint32_t de_len = ext4_dir_en_get_entry_len(tmp_de, block_size);

		/* Find direct predecessor of removed entry */
		while ((offset + de_len) < pos) {
			offset += ext4_dir_en_get_entry_len(tmp_de, block_size);
			tmp_de = (void *)(result.block.data + offset);
			de_len = ext4_dir_en_get_entry_len(tmp_de, block_size);
		}

		ext4_assert(de_len + offset == pos);

		/* Add to removed entry length to predecessor's length */
		uint32_t del_len;
		del_len = ext4_dir_en_get_entry_len(result.dentry, block_size);
		ext4_dir_en_set_entry_len(tmp_de, de_len + del_len, block_size);
	}

	ext4_dir_set_csum(parent,
			(struct ext4_dir_en *)result.block.data);
	ext4_trans_set_block_dirty(result.block.buf);

	return ext4_dir_destroy_result(parent, &result);
}

int ext4_dir_try_insert_entry(struct ext4_sblock *sb,
			      struct ext4_inode_ref *inode_ref,
			      struct ext4_block *dst_blk,
			      struct ext4_inode_ref *child, const char *name,
			      uint32_t name_len)
{
	/* Compute required length entry and align it to 4 bytes */
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint16_t required_len = sizeof(struct ext4_fake_dir_entry) + name_len;

	if ((required_len % 4) != 0)
		required_len += 4 - (required_len % 4);

	/* Initialize pointers, stop means to upper bound */
	struct ext4_dir_en *start = (void *)dst_blk->data;
	struct ext4_dir_en *stop = (void *)(dst_blk->data + block_size);

	/*
	 * Walk through the block and check for invalid entries
	 * or entries with free space for new entry
	 */
	while (start < stop) {
		uint32_t inode = ext4_dir_en_get_inode(start);
		uint32_t rec_len = ext4_dir_en_get_entry_len(start, block_size);
		uint8_t itype = ext4_dir_en_get_inode_type(sb, start);

		/* If invalid and large enough entry, use it */
		if ((inode == 0) && (itype != EXT4_DIRENTRY_DIR_CSUM) &&
		    (rec_len >= required_len)) {
			ext4_dir_write_entry(sb, start, rec_len, child, name,
					     name_len);
			ext4_dir_set_csum(inode_ref, (void *)dst_blk->data);
			ext4_trans_set_block_dirty(dst_blk->buf);

			return EOK;
		}

		/* Valid entry, try to split it */
		if (inode != 0) {
		uint16_t used_len;
			used_len = ext4_dir_en_get_name_len(sb, start);

			uint16_t sz;
			sz = sizeof(struct ext4_fake_dir_entry) + used_len;

			if ((used_len % 4) != 0)
				sz += 4 - (used_len % 4);

			uint32_t free_space = rec_len - sz;

			/* There is free space for new entry */
			if (free_space >= required_len) {
				/* Cut tail of current entry */
				struct ext4_dir_en * new_entry;
				new_entry = (void *)((uint8_t *)start + sz);
				ext4_dir_en_set_entry_len(start, sz, block_size);
				ext4_dir_write_entry(sb, new_entry, free_space,
						     child, name, name_len);

				ext4_dir_set_csum(inode_ref,
						  (void *)dst_blk->data);
				ext4_trans_set_block_dirty(dst_blk->buf);
				return EOK;
			}
		}

		/* Jump to the next entry */
		start = (void *)((uint8_t *)start + rec_len);
	}

	/* No free space found for new entry */
	return ENOSPC;
}

int ext4_dir_find_in_block(struct ext4_block *block, struct ext4_sblock *sb,
			   size_t name_len, const char *name, int casefold,
			   struct ext4_dir_en **res_entry)
{
	/* Start from the first entry in block */
	struct ext4_dir_en *de = (struct ext4_dir_en *)block->data;

	/* Set upper bound for cycling */
	uint8_t *addr_limit = block->data + ext4_sb_get_block_size(sb);

	/* Walk through the block and check entries */
	while ((uint8_t *)de < addr_limit) {
		/* Termination condition */
		if ((uint8_t *)de + name_len > addr_limit)
			break;

		/* Valid entry - check it */
		if (ext4_dir_en_get_inode(de) != 0) {
			/* For more efficient compare only lengths firstly*/
			uint16_t el = ext4_dir_en_get_name_len(sb, de);
			if (casefold || el == name_len) {
				int equal = 0;
				int match = casefold
				    ? ext4_casefold_equal(name, name_len, de->name,
						 el, &equal) == 0 && equal
				    : memcmp(name, de->name, name_len) == 0;
				if (match) {
					*res_entry = de;
					return EOK;
				}
			}
		}

		uint32_t de_len = ext4_dir_en_get_entry_len(
			de, ext4_sb_get_block_size(sb));

		/* Corrupted entry */
		if (de_len == 0)
			return EINVAL;

		/* Jump to next entry */
		de = (struct ext4_dir_en *)((uint8_t *)de + de_len);
	}

	/* Entry not found */
	return ENOENT;
}

int ext4_dir_destroy_result(struct ext4_inode_ref *parent,
			    struct ext4_dir_search_result *result)
{
	if (result->block.lb_id)
		return ext4_block_set(parent->fs->bdev, &result->block);

	return EOK;
}

/**
 * @}
 */
