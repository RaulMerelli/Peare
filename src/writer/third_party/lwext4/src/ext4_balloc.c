/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
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
 * @file  ext4_balloc.c
 * @brief Physical block allocator.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_trans.h>
#include <ext4_balloc.h>
#include <ext4_super.h>
#include <ext4_crc32.h>
#include <ext4_block_group.h>
#include <ext4_fs.h>
#include <ext4_bitmap.h>
#include <ext4_inode.h>

/**@brief Compute number of block group from block address.
 * @param s superblock pointer.
 * @param baddr Absolute address of block.
 * @return Block group index
 */
uint32_t ext4_balloc_get_bgid_of_block(struct ext4_sblock *s,
				       uint64_t baddr)
{
	if (ext4_get32(s, first_data_block) && baddr)
		baddr--;

	return (uint32_t)(baddr / ext4_get32(s, blocks_per_group));
}

/**@brief Compute the starting block address of a block group
 * @param s   superblock pointer.
 * @param bgid block group index
 * @return Block address
 */
uint64_t ext4_balloc_get_block_of_bgid(struct ext4_sblock *s,
				       uint32_t bgid)
{
	uint64_t baddr = 0;
	if (ext4_get32(s, first_data_block))
		baddr++;

	baddr += bgid * ext4_get32(s, blocks_per_group);
	return baddr;
}

static uint32_t ext4_balloc_cluster_bits(struct ext4_sblock *sb)
{
	return ext4_sb_cluster_ratio_bits(sb);
}

static bool ext4_balloc_block_to_bitmap_index(struct ext4_sblock *sb,
						      ext4_fsblk_t block,
						      uint32_t *index)
{
	uint32_t bits = ext4_balloc_cluster_bits(sb);
	if (!index || bits == UINT32_MAX)
		return false;
	*index = ext4_fs_addr_to_idx_bg(sb, block) >> bits;
	return true;
}

static bool ext4_balloc_group_cluster_count(struct ext4_sblock *sb,
						    uint32_t bgid,
						    uint32_t *count)
{
	uint64_t first;
	uint64_t nblocks;
	uint64_t first_cluster;
	uint64_t nclusters;
	uint32_t bits = ext4_balloc_cluster_bits(sb);

	if (!count || bits == UINT32_MAX)
		return false;
	first = ext4_balloc_get_block_of_bgid(sb, bgid);
	nblocks = ext4_blocks_in_group_cnt(sb, bgid);
	if (!ext4_bmap_block_range_to_clusters(
		    ext4_fs_addr_to_idx_bg(sb, first), nblocks, bits,
		    &first_cluster, &nclusters) || nclusters > UINT32_MAX)
		return false;
	*count = (uint32_t)nclusters;
	return true;
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t ext4_balloc_bitmap_csum(struct ext4_sblock *sb,
					void *bitmap)
{
	uint32_t checksum = 0;
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		uint32_t block_size = ext4_sb_get_block_size(sb);

		checksum = ext4_sb_get_csum_seed(sb);
		/* Then calculate crc32 checksum against block_group_desc */
		checksum = ext4_crc32c(checksum, bitmap, block_size);
	}
	return checksum;
}
#else
#define ext4_balloc_bitmap_csum(...) 0
#endif

void ext4_balloc_set_bitmap_csum(struct ext4_sblock *sb,
				 struct ext4_bgroup *bg,
				 void *bitmap __unused)
{
	int desc_size = ext4_sb_get_desc_size(sb);
	uint32_t checksum = ext4_balloc_bitmap_csum(sb, bitmap);
	uint16_t lo_checksum = to_le16(checksum & 0xFFFF),
		 hi_checksum = to_le16(checksum >> 16);

	if (!ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM))
		return;

	/* See if we need to assign a 32bit checksum */
	bg->block_bitmap_csum_lo = lo_checksum;
	if (desc_size == EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE)
		bg->block_bitmap_csum_hi = hi_checksum;

}

#if CONFIG_META_CSUM_ENABLE
static bool
ext4_balloc_verify_bitmap_csum(struct ext4_sblock *sb,
			       struct ext4_bgroup *bg,
			       void *bitmap __unused)
{
	int desc_size = ext4_sb_get_desc_size(sb);
	uint32_t checksum = ext4_balloc_bitmap_csum(sb, bitmap);
	uint16_t lo_checksum = to_le16(checksum & 0xFFFF),
		 hi_checksum = to_le16(checksum >> 16);

	if (!ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM))
		return true;

	if (bg->block_bitmap_csum_lo != lo_checksum)
		return false;

	if (desc_size == EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE)
		if (bg->block_bitmap_csum_hi != hi_checksum)
			return false;

	return true;
}
#else
#define ext4_balloc_verify_bitmap_csum(...) true
#endif

int ext4_balloc_free_block(struct ext4_inode_ref *inode_ref, ext4_fsblk_t baddr)
{
	return ext4_balloc_free_blocks(inode_ref, baddr, 1);
}

int ext4_balloc_free_blocks(struct ext4_inode_ref *inode_ref,
			    ext4_fsblk_t first, uint32_t count)
{
	int rc = EOK;
	uint32_t remaining = count;
	uint32_t cluster_bits;
	uint64_t cluster_ratio;
	uint64_t start_block = first;
	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = &fs->sb;

	if (!count)
		return EOK;
	if (first > UINT64_MAX - count)
		return EINVAL;
	cluster_bits = ext4_balloc_cluster_bits(sb);
	if (cluster_bits == UINT32_MAX)
		return ENOTSUP;
	cluster_ratio = (uint64_t)1 << cluster_bits;

	while (remaining) {
		uint32_t bgid = ext4_balloc_get_bgid_of_block(sb, first);
		uint32_t block_index = ext4_fs_addr_to_idx_bg(sb, first);
		uint32_t group_blocks = ext4_blocks_in_group_cnt(sb, bgid);
		uint32_t blocks_here;
		uint64_t cluster_first;
		uint64_t cluster_count;
		uint32_t freed_clusters = 0;
		uint32_t i;
		struct ext4_block_group_ref bg_ref;
		struct ext4_block bitmap;
		struct ext4_bgroup *bg;

		if (block_index >= group_blocks)
			return EINVAL;
		blocks_here = group_blocks - block_index;
		if (blocks_here > remaining)
			blocks_here = remaining;
		if (!blocks_here || !ext4_bmap_block_range_to_clusters(
			    block_index, blocks_here, cluster_bits,
			    &cluster_first, &cluster_count) ||
		    cluster_count > UINT32_MAX)
			return EINVAL;

		rc = ext4_fs_get_block_group_ref(fs, bgid, &bg_ref);
		if (rc != EOK)
			return rc;
		bg = bg_ref.block_group;
		rc = ext4_trans_block_get(fs->bdev, &bitmap,
					  ext4_bg_get_block_bitmap(bg, sb));
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}
		if (!ext4_balloc_verify_bitmap_csum(sb, bg, bitmap.data)) {
			ext4_dbg(DEBUG_BALLOC, DBG_WARN
				"Bitmap checksum failed. Group: %" PRIu32 "\n",
				bg_ref.index);
		}

		for (i = 0; i < cluster_count; ++i) {
			uint32_t index = (uint32_t)(cluster_first + i);
			if (ext4_bmap_is_bit_set(bitmap.data, index))
				freed_clusters++;
		}
		if (freed_clusters) {
			uint64_t freed_blocks = (uint64_t)freed_clusters * cluster_ratio;
			uint64_t inode_units = freed_blocks *
				(ext4_sb_get_block_size(sb) / EXT4_INODE_BLOCK_SIZE);
			uint64_t inode_blocks = ext4_inode_get_blocks_count(sb,
									 inode_ref->inode);
			uint64_t total_free = ext4_sb_get_free_blocks_cnt(sb);
			uint32_t group_free = ext4_bg_get_free_blocks_count(bg, sb);
			if (inode_blocks < inode_units ||
			    total_free > UINT64_MAX - freed_blocks ||
			    group_free > UINT32_MAX - freed_clusters) {
				ext4_block_set(fs->bdev, &bitmap);
				ext4_fs_put_block_group_ref(&bg_ref);
				return EIO;
			}
			for (i = 0; i < cluster_count; ++i) {
				uint32_t index = (uint32_t)(cluster_first + i);
				if (ext4_bmap_is_bit_set(bitmap.data, index))
					ext4_bmap_bit_clr(bitmap.data, index);
			}
			ext4_balloc_set_bitmap_csum(sb, bg, bitmap.data);
			ext4_trans_set_block_dirty(bitmap.buf);
			ext4_sb_set_free_blocks_cnt(sb, total_free + freed_blocks);
			rc = ext4_inode_set_blocks_count(sb, inode_ref->inode,
							  inode_blocks - inode_units);
			if (rc != EOK) {
				ext4_block_set(fs->bdev, &bitmap);
				ext4_fs_put_block_group_ref(&bg_ref);
				return rc;
			}
			inode_ref->dirty = true;
			ext4_bg_set_free_blocks_count(bg, sb,
					      group_free + freed_clusters);
			bg_ref.dirty = true;
		}

		rc = ext4_block_set(fs->bdev, &bitmap);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}
		rc = ext4_fs_put_block_group_ref(&bg_ref);
		if (rc != EOK)
			return rc;
		first += blocks_here;
		remaining -= blocks_here;
	}

	for (uint32_t i = 0; i < count; ++i) {
		rc = ext4_trans_try_revoke_block(fs->bdev, start_block + i);
		if (rc != EOK)
			return rc;
	}
	ext4_bcache_invalidate_lba(fs->bdev->bc, start_block, count);
	return EOK;
}

static int ext4_balloc_alloc_bigalloc_cluster(
	struct ext4_inode_ref *inode_ref, ext4_fsblk_t goal,
	ext4_fsblk_t *fblock, uint32_t cluster_bits)
{
	struct ext4_sblock *sb = &inode_ref->fs->sb;
	uint64_t cluster_ratio = (uint64_t)1 << cluster_bits;
	uint32_t goal_group = ext4_balloc_get_bgid_of_block(sb, goal);
	uint32_t goal_index;
	uint32_t group_count = ext4_block_group_cnt(sb);
	uint32_t attempt;
	uint32_t block_size = ext4_sb_get_block_size(sb);
	uint64_t inode_units = cluster_ratio *
		(block_size / EXT4_INODE_BLOCK_SIZE);

	if (!ext4_balloc_block_to_bitmap_index(sb, goal, &goal_index))
		return ENOTSUP;
	for (attempt = 0; attempt < group_count; ++attempt) {
		uint32_t bgid = (goal_group + attempt) % group_count;
		struct ext4_block_group_ref bg_ref;
		struct ext4_block bitmap;
		struct ext4_bgroup *bg;
		uint32_t cluster_count;
		uint32_t first;
		uint32_t found;
		uint64_t free_clusters;
		uint64_t free_blocks;
		int rc;

		rc = ext4_fs_get_block_group_ref(inode_ref->fs, bgid, &bg_ref);
		if (rc != EOK)
			return rc;
		bg = bg_ref.block_group;
		free_clusters = ext4_bg_get_free_blocks_count(bg, sb);
		if (!free_clusters) {
			ext4_fs_put_block_group_ref(&bg_ref);
			continue;
		}
		if (!ext4_balloc_group_cluster_count(sb, bgid, &cluster_count)) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return ENOTSUP;
		}
		rc = ext4_trans_block_get(inode_ref->fs->bdev, &bitmap,
					  ext4_bg_get_block_bitmap(bg, sb));
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}
		if (!ext4_balloc_verify_bitmap_csum(sb, bg, bitmap.data)) {
			ext4_dbg(DEBUG_BALLOC, DBG_WARN
				"Bitmap checksum failed. Group: %" PRIu32 "\n",
				bgid);
		}
		first = bgid == goal_group && goal_index < cluster_count
			? goal_index : 0;
		rc = ext4_bmap_bit_find_clr(bitmap.data, first, cluster_count,
					    &found);
		if (rc != EOK) {
			ext4_block_set(inode_ref->fs->bdev, &bitmap);
			ext4_fs_put_block_group_ref(&bg_ref);
			continue;
		}
		free_blocks = ext4_sb_get_free_blocks_cnt(sb);
		if (free_blocks < cluster_ratio || free_clusters > UINT32_MAX) {
			ext4_block_set(inode_ref->fs->bdev, &bitmap);
			ext4_fs_put_block_group_ref(&bg_ref);
			return EIO;
		}
		ext4_bmap_bit_set(bitmap.data, found);
		ext4_balloc_set_bitmap_csum(sb, bg, bitmap.data);
		ext4_trans_set_block_dirty(bitmap.buf);
		rc = ext4_block_set(inode_ref->fs->bdev, &bitmap);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}
		ext4_sb_set_free_blocks_cnt(sb, free_blocks - cluster_ratio);
		rc = ext4_inode_set_blocks_count(
			sb, inode_ref->inode,
			ext4_inode_get_blocks_count(sb, inode_ref->inode) +
			inode_units);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}
		inode_ref->dirty = true;
		ext4_bg_set_free_blocks_count(bg, sb,
					      (uint32_t)free_clusters - 1);
		bg_ref.dirty = true;
		rc = ext4_fs_put_block_group_ref(&bg_ref);
		if (rc != EOK)
			return rc;
		*fblock = ext4_fs_bg_idx_to_addr(
			sb, found << cluster_bits, bgid);
		return EOK;
	}
	return ENOSPC;
}

int ext4_balloc_alloc_block(struct ext4_inode_ref *inode_ref,
			    ext4_fsblk_t goal,
			    ext4_fsblk_t *fblock)
{
	ext4_fsblk_t alloc = 0;
	ext4_fsblk_t bmp_blk_adr;
	uint32_t rel_blk_idx = 0;
	uint64_t free_blocks;
	int r;
	struct ext4_sblock *sb = &inode_ref->fs->sb;
	uint32_t cluster_bits = ext4_balloc_cluster_bits(sb);

	if (cluster_bits == UINT32_MAX)
		return ENOTSUP;
	if (cluster_bits)
		return ext4_balloc_alloc_bigalloc_cluster(inode_ref, goal,
							  fblock, cluster_bits);

	/* Load block group number for goal and relative index */
	uint32_t bg_id = ext4_balloc_get_bgid_of_block(sb, goal);
	uint32_t idx_in_bg = ext4_fs_addr_to_idx_bg(sb, goal);

	struct ext4_block b;
	struct ext4_block_group_ref bg_ref;

	/* Load block group reference */
	r = ext4_fs_get_block_group_ref(inode_ref->fs, bg_id, &bg_ref);
	if (r != EOK)
		return r;

	struct ext4_bgroup *bg = bg_ref.block_group;

	free_blocks = ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	if (free_blocks == 0) {
		/* This group has no free blocks */
		goto goal_failed;
	}

	/* Compute indexes */
	ext4_fsblk_t first_in_bg;
	first_in_bg = ext4_balloc_get_block_of_bgid(sb, bg_ref.index);

	uint32_t first_in_bg_index;
	first_in_bg_index = ext4_fs_addr_to_idx_bg(sb, first_in_bg);

	if (idx_in_bg < first_in_bg_index)
		idx_in_bg = first_in_bg_index;

	/* Load block with bitmap */
	bmp_blk_adr = ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

	r = ext4_trans_block_get(inode_ref->fs->bdev, &b, bmp_blk_adr);
	if (r != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return r;
	}

	if (!ext4_balloc_verify_bitmap_csum(sb, bg, b.data)) {
		ext4_dbg(DEBUG_BALLOC,
			DBG_WARN "Bitmap checksum failed."
			"Group: %" PRIu32"\n",
			bg_ref.index);
	}

	/* Check if goal is free */
	if (ext4_bmap_is_bit_clr(b.data, idx_in_bg)) {
		ext4_bmap_bit_set(b.data, idx_in_bg);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
					    b.data);
		ext4_trans_set_block_dirty(b.buf);
		r = ext4_block_set(inode_ref->fs->bdev, &b);
		if (r != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return r;
		}

		alloc = ext4_fs_bg_idx_to_addr(sb, idx_in_bg, bg_id);
		goto success;
	}

	uint32_t blk_in_bg = ext4_blocks_in_group_cnt(sb, bg_id);

	uint32_t end_idx = (idx_in_bg + 63) & ~63;
	if (end_idx > blk_in_bg)
		end_idx = blk_in_bg;

	/* Try to find free block near to goal */
	uint32_t tmp_idx;
	for (tmp_idx = idx_in_bg + 1; tmp_idx < end_idx; ++tmp_idx) {
		if (ext4_bmap_is_bit_clr(b.data, tmp_idx)) {
			ext4_bmap_bit_set(b.data, tmp_idx);

			ext4_balloc_set_bitmap_csum(sb, bg, b.data);
			ext4_trans_set_block_dirty(b.buf);
			r = ext4_block_set(inode_ref->fs->bdev, &b);
			if (r != EOK) {
				ext4_fs_put_block_group_ref(&bg_ref);
				return r;
			}

			alloc = ext4_fs_bg_idx_to_addr(sb, tmp_idx, bg_id);
			goto success;
		}
	}

	/* Find free bit in bitmap */
	r = ext4_bmap_bit_find_clr(b.data, idx_in_bg, blk_in_bg, &rel_blk_idx);
	if (r == EOK) {
		ext4_bmap_bit_set(b.data, rel_blk_idx);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group, b.data);
		ext4_trans_set_block_dirty(b.buf);
		r = ext4_block_set(inode_ref->fs->bdev, &b);
		if (r != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return r;
		}

		alloc = ext4_fs_bg_idx_to_addr(sb, rel_blk_idx, bg_id);
		goto success;
	}

	/* No free block found yet */
	r = ext4_block_set(inode_ref->fs->bdev, &b);
	if (r != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return r;
	}

goal_failed:

	r = ext4_fs_put_block_group_ref(&bg_ref);
	if (r != EOK)
		return r;

	/* Try other block groups */
	uint32_t block_group_count = ext4_block_group_cnt(sb);
	uint32_t bgid = (bg_id + 1) % block_group_count;
	uint32_t count = block_group_count;

	while (count > 0) {
		r = ext4_fs_get_block_group_ref(inode_ref->fs, bgid, &bg_ref);
		if (r != EOK)
			return r;

		struct ext4_bgroup *bg = bg_ref.block_group;
		free_blocks = ext4_bg_get_free_blocks_count(bg, sb);
		if (free_blocks == 0) {
			/* This group has no free blocks */
			goto next_group;
		}

		/* Load block with bitmap */
		bmp_blk_adr = ext4_bg_get_block_bitmap(bg, sb);
		r = ext4_trans_block_get(inode_ref->fs->bdev, &b, bmp_blk_adr);
		if (r != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return r;
		}

		if (!ext4_balloc_verify_bitmap_csum(sb, bg, b.data)) {
			ext4_dbg(DEBUG_BALLOC,
				DBG_WARN "Bitmap checksum failed."
				"Group: %" PRIu32"\n",
				bg_ref.index);
		}

		/* Compute indexes */
		first_in_bg = ext4_balloc_get_block_of_bgid(sb, bgid);
		idx_in_bg = ext4_fs_addr_to_idx_bg(sb, first_in_bg);
		blk_in_bg = ext4_blocks_in_group_cnt(sb, bgid);
		first_in_bg_index = ext4_fs_addr_to_idx_bg(sb, first_in_bg);

		if (idx_in_bg < first_in_bg_index)
			idx_in_bg = first_in_bg_index;

		r = ext4_bmap_bit_find_clr(b.data, idx_in_bg, blk_in_bg,
				&rel_blk_idx);
		if (r == EOK) {
			ext4_bmap_bit_set(b.data, rel_blk_idx);
			ext4_balloc_set_bitmap_csum(sb, bg, b.data);
			ext4_trans_set_block_dirty(b.buf);
			r = ext4_block_set(inode_ref->fs->bdev, &b);
			if (r != EOK) {
				ext4_fs_put_block_group_ref(&bg_ref);
				return r;
			}

			alloc = ext4_fs_bg_idx_to_addr(sb, rel_blk_idx, bgid);
			goto success;
		}

		r = ext4_block_set(inode_ref->fs->bdev, &b);
		if (r != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return r;
		}

	next_group:
		r = ext4_fs_put_block_group_ref(&bg_ref);
		if (r != EOK) {
			return r;
		}

		/* Goto next group */
		bgid = (bgid + 1) % block_group_count;
		count--;
	}

	return ENOSPC;

success:
    /* Empty command - because of syntax */
    ;

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
	uint64_t sb_free_blocks = ext4_sb_get_free_blocks_cnt(sb);
	sb_free_blocks--;
	ext4_sb_set_free_blocks_cnt(sb, sb_free_blocks);

	/* Update inode blocks (different block size!) count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks += block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */

	uint32_t fb_cnt = ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	fb_cnt--;
	ext4_bg_set_free_blocks_count(bg_ref.block_group, sb, fb_cnt);

	bg_ref.dirty = true;
	r = ext4_fs_put_block_group_ref(&bg_ref);

	*fblock = alloc;
	return r;
}

int ext4_balloc_try_alloc_block(struct ext4_inode_ref *inode_ref,
				ext4_fsblk_t baddr, bool *free)
{
	int rc;

	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = &fs->sb;
	uint32_t cluster_bits = ext4_balloc_cluster_bits(sb);

	if (cluster_bits == UINT32_MAX)
		return ENOTSUP;
	if (cluster_bits) {
		uint64_t ratio = (uint64_t)1 << cluster_bits;
		uint32_t bgid = ext4_balloc_get_bgid_of_block(sb, baddr);
		uint32_t index;
		uint32_t block_size = ext4_sb_get_block_size(sb);
		uint64_t free_blocks;
		uint64_t free_clusters;
		struct ext4_block_group_ref bg_ref;
		struct ext4_block bitmap;
		struct ext4_bgroup *bg;
		if (!free || !ext4_balloc_block_to_bitmap_index(sb, baddr,
							       &index))
			return EINVAL;
		rc = ext4_fs_get_block_group_ref(fs, bgid, &bg_ref);
		if (rc != EOK)
			return rc;
		bg = bg_ref.block_group;
		rc = ext4_trans_block_get(fs->bdev, &bitmap,
					  ext4_bg_get_block_bitmap(bg, sb));
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}
		*free = ext4_bmap_is_bit_clr(bitmap.data, index);
		if (*free) {
			free_blocks = ext4_sb_get_free_blocks_cnt(sb);
			free_clusters = ext4_bg_get_free_blocks_count(bg, sb);
			if (free_blocks < ratio || !free_clusters) {
				ext4_block_set(fs->bdev, &bitmap);
				ext4_fs_put_block_group_ref(&bg_ref);
				return EIO;
			}
			ext4_bmap_bit_set(bitmap.data, index);
			ext4_balloc_set_bitmap_csum(sb, bg, bitmap.data);
			ext4_trans_set_block_dirty(bitmap.buf);
			rc = ext4_block_set(fs->bdev, &bitmap);
			if (rc != EOK) {
				ext4_fs_put_block_group_ref(&bg_ref);
				return rc;
			}
			ext4_sb_set_free_blocks_cnt(sb, free_blocks - ratio);
			rc = ext4_inode_set_blocks_count(
				sb, inode_ref->inode,
				ext4_inode_get_blocks_count(sb, inode_ref->inode) +
				ratio * (block_size / EXT4_INODE_BLOCK_SIZE));
			if (rc != EOK) {
				ext4_fs_put_block_group_ref(&bg_ref);
				return rc;
			}
			inode_ref->dirty = true;
			ext4_bg_set_free_blocks_count(bg, sb,
						      (uint32_t)free_clusters - 1);
			bg_ref.dirty = true;
		} else {
			rc = ext4_block_set(fs->bdev, &bitmap);
			if (rc != EOK) {
				ext4_fs_put_block_group_ref(&bg_ref);
				return rc;
			}
		}
		return ext4_fs_put_block_group_ref(&bg_ref);
	}

	/* Compute indexes */
	uint32_t block_group = ext4_balloc_get_bgid_of_block(sb, baddr);
	uint32_t index_in_group = ext4_fs_addr_to_idx_bg(sb, baddr);

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	rc = ext4_fs_get_block_group_ref(fs, block_group, &bg_ref);
	if (rc != EOK)
		return rc;

	/* Load block with bitmap */
	ext4_fsblk_t bmp_blk_addr;
	bmp_blk_addr = ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

	struct ext4_block b;
	rc = ext4_trans_block_get(fs->bdev, &b, bmp_blk_addr);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	if (!ext4_balloc_verify_bitmap_csum(sb, bg_ref.block_group, b.data)) {
		ext4_dbg(DEBUG_BALLOC,
			DBG_WARN "Bitmap checksum failed."
			"Group: %" PRIu32"\n",
			bg_ref.index);
	}

	/* Check if block is free */
	*free = ext4_bmap_is_bit_clr(b.data, index_in_group);

	/* Allocate block if possible */
	if (*free) {
		ext4_bmap_bit_set(b.data, index_in_group);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group, b.data);
		ext4_trans_set_block_dirty(b.buf);
	}

	/* Release block with bitmap */
	rc = ext4_block_set(fs->bdev, &b);
	if (rc != EOK) {
		/* Error in saving bitmap */
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	/* If block is not free, return */
	if (!(*free))
		goto terminate;

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
	uint64_t sb_free_blocks = ext4_sb_get_free_blocks_cnt(sb);
	sb_free_blocks--;
	ext4_sb_set_free_blocks_cnt(sb, sb_free_blocks);

	/* Update inode blocks count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks += block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */
	uint32_t fb_cnt = ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	fb_cnt--;
	ext4_bg_set_free_blocks_count(bg_ref.block_group, sb, fb_cnt);

	bg_ref.dirty = true;

terminate:
	return ext4_fs_put_block_group_ref(&bg_ref);
}

/**
 * @}
 */
