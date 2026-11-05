/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * Copyright (c) 2015 Kaho Ng (ngkaho1234@gmail.com)
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
 * @file  ext4_journal.c
 * @brief Journal handle functions
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_dir.h>
#include <ext4_extent.h>
#include <ext4_ialloc.h>
#include <ext4_super.h>
#include <ext4_journal.h>
#include <ext4_blockdev.h>
#include <ext4_crc32.h>
#include <ext4_journal.h>

#include <string.h>
#include <stdlib.h>

/**@brief  Revoke entry during journal replay.*/
struct revoke_entry {
	/**@brief  Block number not to be replayed.*/
	ext4_fsblk_t block;

	/**@brief  For any transaction id smaller
	 *         than trans_id, records of @block
	 *         in those transactions should not
	 *         be replayed.*/
	uint32_t trans_id;

	/**@brief  Revoke tree node.*/
	RB_ENTRY(revoke_entry) revoke_node;
};

/**@brief  Valid journal replay information.*/
struct recover_info {
	/**@brief  Starting transaction id.*/
	uint32_t start_trans_id;

	/**@brief  Ending transaction id.*/
	uint32_t last_trans_id;

	/**@brief  Used as internal argument.*/
	uint32_t this_trans_id;

	/**@brief  No of transactions went through.*/
	uint32_t trans_cnt;

	/**@brief  RB-Tree storing revoke entries.*/
	RB_HEAD(jbd_revoke, revoke_entry) revoke_root;
};

/**@brief  Journal replay internal arguments.*/
struct replay_arg {
	/**@brief  Journal replay information.*/
	struct recover_info *info;

	/**@brief  Current block we are on.*/
	uint32_t *this_block;

	/**@brief  Current trans_id we are on.*/
	uint32_t this_trans_id;
};

static uint32_t jbd_fc_blocks(struct jbd_sb *sb)
{
	if (jbd_get32(&sb->header, blocktype) != JBD_SUPERBLOCK_V2)
		return 0;
	return jbd_get32(sb, num_fc_blks);
}

static uint32_t jbd_fc_first_block(struct jbd_sb *sb)
{
	/* JBD2 leaves one boundary block unused between the logs. */
	return jbd_get32(sb, maxlen) - jbd_fc_blocks(sb) + 1;
}

/* Make sure we wrap around the log correctly! */
#define wrap(sb, var)						\
do {									\
	if (var >= jbd_get32((sb), maxlen) -			\
	    jbd_fc_blocks((sb)))					\
		var -= ((jbd_get32((sb), maxlen) -			\
			 jbd_fc_blocks((sb))) -			\
			jbd_get32((sb), first));			\
} while (0)

static inline int32_t
trans_id_diff(uint32_t x, uint32_t y)
{
	int32_t diff = x - y;
	return diff;
}

static int
jbd_revoke_entry_cmp(struct revoke_entry *a, struct revoke_entry *b)
{
	if (a->block > b->block)
		return 1;
	else if (a->block < b->block)
		return -1;
	return 0;
}

static int
jbd_block_rec_cmp(struct jbd_block_rec *a, struct jbd_block_rec *b)
{
	if (a->lba > b->lba)
		return 1;
	else if (a->lba < b->lba)
		return -1;
	return 0;
}

static int
jbd_revoke_rec_cmp(struct jbd_revoke_rec *a, struct jbd_revoke_rec *b)
{
	if (a->lba > b->lba)
		return 1;
	else if (a->lba < b->lba)
		return -1;
	return 0;
}

RB_GENERATE_INTERNAL(jbd_revoke, revoke_entry, revoke_node,
		     jbd_revoke_entry_cmp, static inline)
RB_GENERATE_INTERNAL(jbd_block, jbd_block_rec, block_rec_node,
		     jbd_block_rec_cmp, static inline)
RB_GENERATE_INTERNAL(jbd_revoke_tree, jbd_revoke_rec, revoke_node,
		     jbd_revoke_rec_cmp, static inline)

#define jbd_alloc_revoke_entry() ext4_calloc(1, sizeof(struct revoke_entry))
#define jbd_free_revoke_entry(addr) ext4_free(addr)

static int jbd_has_csum(struct jbd_sb *jbd_sb)
{
	if (JBD_HAS_INCOMPAT_FEATURE(jbd_sb, JBD_FEATURE_INCOMPAT_CSUM_V2))
		return 2;

	if (JBD_HAS_INCOMPAT_FEATURE(jbd_sb, JBD_FEATURE_INCOMPAT_CSUM_V3))
		return 3;

	if (JBD_HAS_COMPAT_FEATURE(jbd_sb, JBD_FEATURE_COMPAT_CHECKSUM))
		return 1;

	return 0;
}

static bool jbd_has_csum_v2or3(struct jbd_sb *jbd_sb)
{
	return jbd_has_csum(jbd_sb) >= 2;
}

static uint32_t jbd_crc32_be(uint32_t crc, const void *buf, uint32_t size)
{
	const uint8_t *data = buf;

	while (size--) {
		unsigned int bit;
		crc ^= (uint32_t)*data++ << 24;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^
				((crc & 0x80000000U) ? 0x04C11DB7U : 0);
	}
	return crc;
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t jbd_sb_csum(struct jbd_sb *jbd_sb)
{
	uint32_t checksum = 0;

	if (jbd_has_csum_v2or3(jbd_sb)) {
		uint32_t orig_checksum = jbd_sb->checksum;
		jbd_set32(jbd_sb, checksum, 0);
		/* Calculate crc32c checksum against tho whole superblock */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_sb,
				JBD_SUPERBLOCK_SIZE);
		jbd_sb->checksum = orig_checksum;
	}
	return checksum;
}
#else
#define jbd_sb_csum(...) 0
#endif

static void jbd_sb_csum_set(struct jbd_sb *jbd_sb)
{
	if (!jbd_has_csum_v2or3(jbd_sb))
		return;

	jbd_set32(jbd_sb, checksum, jbd_sb_csum(jbd_sb));
}

#if CONFIG_META_CSUM_ENABLE
static bool
jbd_verify_sb_csum(struct jbd_sb *jbd_sb)
{
	if (!jbd_has_csum_v2or3(jbd_sb))
		return true;

	return jbd_sb_csum(jbd_sb) == jbd_get32(jbd_sb, checksum);
}
#else
#define jbd_verify_sb_csum(...) true
#endif

#if CONFIG_META_CSUM_ENABLE
static uint32_t jbd_meta_csum(struct jbd_fs *jbd_fs,
			      struct jbd_bhdr *bhdr)
{
	uint32_t checksum = 0;

	if (jbd_has_csum_v2or3(&jbd_fs->sb)) {
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		struct jbd_block_tail *tail =
			(struct jbd_block_tail *)((char *)bhdr + block_size -
				sizeof(struct jbd_block_tail));
		uint32_t orig_checksum = tail->checksum;
		tail->checksum = 0;

		/* First calculate crc32c checksum against fs uuid */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid,
				       sizeof(jbd_fs->sb.uuid));
		/* Calculate crc32c checksum against tho whole block */
		checksum = ext4_crc32c(checksum, bhdr,
				block_size);
		tail->checksum = orig_checksum;
	}
	return checksum;
}
#else
#define jbd_meta_csum(...) 0
#endif

static void jbd_meta_csum_set(struct jbd_fs *jbd_fs,
			      struct jbd_bhdr *bhdr)
{
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	struct jbd_block_tail *tail = (struct jbd_block_tail *)
				((char *)bhdr + block_size -
				sizeof(struct jbd_block_tail));
	if (!jbd_has_csum_v2or3(&jbd_fs->sb))
		return;

	tail->checksum = to_be32(jbd_meta_csum(jbd_fs, bhdr));
}

#if CONFIG_META_CSUM_ENABLE
static bool
jbd_verify_meta_csum(struct jbd_fs *jbd_fs,
		     struct jbd_bhdr *bhdr)
{
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	struct jbd_block_tail *tail = (struct jbd_block_tail *)
				((char *)bhdr + block_size -
				sizeof(struct jbd_block_tail));
	if (!jbd_has_csum_v2or3(&jbd_fs->sb))
		return true;

	return jbd_meta_csum(jbd_fs, bhdr) == to_be32(tail->checksum);
}
#else
#define jbd_verify_meta_csum(...) true
#endif

#if CONFIG_META_CSUM_ENABLE
static uint32_t jbd_commit_csum(struct jbd_fs *jbd_fs,
			      struct jbd_commit_header *header)
{
	uint32_t checksum = 0;

	if (jbd_has_csum_v2or3(&jbd_fs->sb)) {
		uint8_t orig_checksum_type = header->chksum_type,
			 orig_checksum_size = header->chksum_size;
		uint32_t orig_checksum = header->chksum[0];
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		header->chksum_type = 0;
		header->chksum_size = 0;
		header->chksum[0] = 0;

		/* First calculate crc32c checksum against fs uuid */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid,
				       sizeof(jbd_fs->sb.uuid));
		/* Calculate crc32c checksum against tho whole block */
		checksum = ext4_crc32c(checksum, header,
				block_size);

		header->chksum_type = orig_checksum_type;
		header->chksum_size = orig_checksum_size;
		header->chksum[0] = orig_checksum;
	}
	return checksum;
}
#else
#define jbd_commit_csum(...) 0
#endif

static void jbd_commit_csum_set(struct jbd_fs *jbd_fs,
			      struct jbd_commit_header *header)
{
	if (!jbd_has_csum_v2or3(&jbd_fs->sb))
		return;

	header->chksum_type = 0;
	header->chksum_size = 0;
	header->chksum[0] = jbd_commit_csum(jbd_fs, header);
}

#if CONFIG_META_CSUM_ENABLE
static bool jbd_verify_commit_csum(struct jbd_fs *jbd_fs,
				   struct jbd_commit_header *header)
{
	if (!jbd_has_csum_v2or3(&jbd_fs->sb))
		return true;

	return header->chksum[0] == to_be32(jbd_commit_csum(jbd_fs,
					    header));
}
#else
#define jbd_verify_commit_csum(...) true
#endif

#if CONFIG_META_CSUM_ENABLE
/*
 * NOTE: We only make use of @csum parameter when
 *       JBD_FEATURE_COMPAT_CHECKSUM is enabled.
 */
static uint32_t jbd_block_csum(struct jbd_fs *jbd_fs, const void *buf,
			       uint32_t csum,
			       uint32_t sequence)
{
	uint32_t checksum = 0;

	if (jbd_has_csum_v2or3(&jbd_fs->sb)) {
		uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
		uint32_t sequence_be = to_be32(sequence);
		/* First calculate crc32c checksum against fs uuid */
		checksum = ext4_crc32c(EXT4_CRC32_INIT, jbd_fs->sb.uuid,
				       sizeof(jbd_fs->sb.uuid));
		/* Then calculate crc32c checksum against sequence no. */
		checksum = ext4_crc32c(checksum, &sequence_be,
				sizeof(uint32_t));
		/* Calculate crc32c checksum against tho whole block */
		checksum = ext4_crc32c(checksum, buf,
				block_size);
	}
	(void)csum;
	return checksum;
}
#else
#define jbd_block_csum(...) 0
#endif

static void jbd_block_tag_csum_set(struct jbd_fs *jbd_fs, void *__tag,
				   uint32_t checksum)
{
	int ver = jbd_has_csum(&jbd_fs->sb);
	if (ver < 2)
		return;

	if (ver == 2) {
		struct jbd_block_tag *tag = __tag;
		jbd_set16(tag, checksum, (uint16_t)checksum);
	} else {
		struct jbd_block_tag3 *tag = __tag;
		tag->checksum = to_be32(checksum);
	}
}

/**@brief  Write jbd superblock to disk.
 * @param  jbd_fs jbd filesystem
 * @param  s jbd superblock
 * @return standard error code*/
static int jbd_sb_write(struct jbd_fs *jbd_fs, struct jbd_sb *s)
{
	int rc;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint64_t offset;
	ext4_fsblk_t fblock;
	if (jbd_fs->external) {
		jbd_sb_csum_set(s);
		return ext4_block_writebytes(jbd_fs->bdev, 0, s,
					     EXT4_SUPERBLOCK_SIZE);
	}
	rc = jbd_inode_bmap(jbd_fs, 0, &fblock);
	if (rc != EOK)
		return rc;

	jbd_sb_csum_set(s);
	offset = fblock * ext4_sb_get_block_size(&fs->sb);
	return ext4_block_writebytes(fs->bdev, offset, s,
				     EXT4_SUPERBLOCK_SIZE);
}

/**@brief  Read jbd superblock from disk.
 * @param  jbd_fs jbd filesystem
 * @param  s jbd superblock
 * @return standard error code*/
static int jbd_sb_read(struct jbd_fs *jbd_fs, struct jbd_sb *s)
{
	int rc;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint64_t offset;
	ext4_fsblk_t fblock;
	if (jbd_fs->external)
		return ext4_block_readbytes(jbd_fs->bdev, 0, s,
					    EXT4_SUPERBLOCK_SIZE);
	rc = jbd_inode_bmap(jbd_fs, 0, &fblock);
	if (rc != EOK)
		return rc;

	offset = fblock * ext4_sb_get_block_size(&fs->sb);
	return ext4_block_readbytes(fs->bdev, offset, s,
				    EXT4_SUPERBLOCK_SIZE);
}

/**@brief  Verify jbd superblock.
 * @param  sb jbd superblock
 * @return true if jbd superblock is valid */
static bool jbd_verify_sb(struct jbd_fs *jbd_fs)
{
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	struct jbd_sb *sb = &jbd_fs->sb;
	struct jbd_bhdr *header = &sb->header;
	uint32_t type;
	uint32_t block_size;
	uint32_t maxlen;
	uint32_t first;
	uint32_t start;
	uint32_t compat = 0;
	uint32_t incompat = 0;
	uint32_t ro_compat = 0;
	uint32_t checksum_features;
	uint32_t num_fc_blks;
	uint64_t journal_size;
	uint32_t fs_block_size;

	if (jbd_get32(header, magic) != JBD_MAGIC_NUMBER)
		return false;

	type = jbd_get32(header, blocktype);
	if (type != JBD_SUPERBLOCK && type != JBD_SUPERBLOCK_V2)
		return false;

	block_size = jbd_get32(sb, blocksize);
	maxlen = jbd_get32(sb, maxlen);
	first = jbd_get32(sb, first);
	start = jbd_get32(sb, start);
	num_fc_blks = jbd_fc_blocks(sb);
	fs_block_size = ext4_sb_get_block_size(&fs->sb);
	journal_size = jbd_fs->external ? jbd_fs->external_size :
		ext4_inode_get_size(&fs->sb, jbd_fs->inode_ref.inode);

	if (block_size != fs_block_size || block_size < 1024 ||
	    (block_size & (block_size - 1)) ||
	    journal_size % block_size || maxlen <= first || first == 0 ||
	    num_fc_blks >= maxlen - first ||
	    (uint64_t)maxlen > journal_size / block_size ||
	    (start && (start < first || start >= maxlen - num_fc_blks)) ||
	    jbd_get32(sb, error_val) != 0 ||
	    (!jbd_fs->external &&
	     !ext4_inode_is_type(&fs->sb, jbd_fs->inode_ref.inode,
				 EXT4_INODE_MODE_FILE)) ||
	    (jbd_fs->external &&
	     memcmp(sb->uuid, fs->sb.journal_uuid, UUID_SIZE) != 0))
		return false;

	if (type == JBD_SUPERBLOCK) {
		return !ext4_sb_feature_com(&fs->sb, EXT4_FCOM_FAST_COMMIT);
	}

	compat = jbd_get32(sb, feature_compat);
	incompat = jbd_get32(sb, feature_incompat);
	ro_compat = jbd_get32(sb, feature_ro_compat);
	if ((incompat & ~JBD_KNOWN_INCOMPAT_FEATURES) ||
	    (ro_compat & ~JBD_KNOWN_ROCOMPAT_FEATURES))
		return false;
	if ((num_fc_blks &&
	     !ext4_sb_feature_com(&fs->sb, EXT4_FCOM_FAST_COMMIT)) ||
	    ((incompat & JBD_FEATURE_INCOMPAT_FAST_COMMIT) &&
	     (!num_fc_blks ||
	      !ext4_sb_feature_com(&fs->sb, EXT4_FCOM_FAST_COMMIT))))
		return false;

	checksum_features = incompat &
		(JBD_FEATURE_INCOMPAT_CSUM_V2 |
		 JBD_FEATURE_INCOMPAT_CSUM_V3);
	if (checksum_features ==
		(JBD_FEATURE_INCOMPAT_CSUM_V2 |
		 JBD_FEATURE_INCOMPAT_CSUM_V3) ||
	    (checksum_features &&
	     (compat & JBD_FEATURE_COMPAT_CHECKSUM)) ||
	    ((incompat & JBD_FEATURE_INCOMPAT_ASYNC_COMMIT) &&
	     !(checksum_features ||
	       (compat & JBD_FEATURE_COMPAT_CHECKSUM))))
		return false;

	if (checksum_features && sb->checksum_type != JBD_CRC32C_CHKSUM)
		return false;

	if (!jbd_verify_sb_csum(sb))
		return false;

	return true;
}

/**@brief  Write back dirty jbd superblock to disk.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
static int jbd_write_sb(struct jbd_fs *jbd_fs)
{
	int rc = EOK;
	if (jbd_fs->dirty) {
		rc = jbd_sb_write(jbd_fs, &jbd_fs->sb);
		if (rc != EOK)
			return rc;

		jbd_fs->dirty = false;
	}
	return rc;
}

/**@brief  Get reference to jbd filesystem.
 * @param  fs Filesystem to load journal of
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_get_fs_on_device(struct ext4_fs *fs, struct jbd_fs *jbd_fs,
		 struct ext4_blockdev *journal_bdev)
{
	int rc;
	uint32_t journal_ino;

	memset(jbd_fs, 0, sizeof(struct jbd_fs));
	jbd_fs->inode_ref.fs = fs;
	if (!ext4_get32(&fs->sb, journal_inode_number) &&
	    ext4_get32(&fs->sb, journal_dev)) {
		if (!journal_bdev || !ext4_get32(&fs->sb, journal_dev))
			return ENODEV;
		if (journal_bdev->part_size > UINT64_MAX -
		    ext4_sb_get_block_size(&fs->sb))
			return EINVAL;
		jbd_fs->external = true;
		jbd_fs->bdev = journal_bdev;
		/* The external superblock occupies one logical journal block. */
		jbd_fs->external_size = journal_bdev->part_size +
			ext4_sb_get_block_size(&fs->sb);
	} else {
		if (journal_bdev)
			return EINVAL;
		journal_ino = ext4_get32(&fs->sb, journal_inode_number);
		rc = ext4_fs_get_inode_ref(fs, journal_ino,
					   &jbd_fs->inode_ref);
		if (rc != EOK)
			return rc;
		jbd_fs->bdev = fs->bdev;
	}

	rc = jbd_sb_read(jbd_fs, &jbd_fs->sb);
	if (rc != EOK)
		goto Error;

	if (!jbd_verify_sb(jbd_fs)) {
		rc = EIO;
		goto Error;
	}

	return rc;
Error:
	if (!jbd_fs->external)
		ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
	memset(jbd_fs, 0, sizeof(struct jbd_fs));

	return rc;
}

int jbd_get_fs(struct ext4_fs *fs, struct jbd_fs *jbd_fs)
{
	return jbd_get_fs_on_device(fs, jbd_fs, NULL);
}

/**@brief  Put reference of jbd filesystem.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_put_fs(struct jbd_fs *jbd_fs)
{
	int rc = EOK;
	rc = jbd_write_sb(jbd_fs);

	if (!jbd_fs->external)
		ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
	return rc;
}

/**@brief  Data block lookup helper.
 * @param  jbd_fs jbd filesystem
 * @param  iblock block index
 * @param  fblock logical block address
 * @return standard error code*/
int jbd_inode_bmap(struct jbd_fs *jbd_fs,
		   ext4_lblk_t iblock,
		   ext4_fsblk_t *fblock)
{
	if (jbd_fs->external) {
		*fblock = iblock;
		return EOK;
	}
	int rc = ext4_fs_get_inode_dblk_idx(
			&jbd_fs->inode_ref,
			iblock,
			fblock,
			false);
	return rc;
}

/**@brief   jbd block get function (through cache).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @param   fblock jbd logical block address
 * @return  standard error code*/
static int jbd_block_get(struct jbd_fs *jbd_fs,
		  struct ext4_block *block,
		  ext4_fsblk_t fblock)
{
	/* TODO: journal device. */
	int rc;
	struct ext4_blockdev *bdev = jbd_fs->bdev;
	ext4_lblk_t iblock = (ext4_lblk_t)fblock;

	/* Lookup the logical block address of
	 * fblock.*/
	rc = jbd_inode_bmap(jbd_fs, iblock,
			    &fblock);
	if (rc != EOK)
		return rc;

	rc = ext4_block_get(bdev, block, fblock);

	/* If succeeded, mark buffer as BC_FLUSH to indicate
	 * that data should be written to disk immediately.*/
	if (rc == EOK) {
		ext4_bcache_set_flag(block->buf, BC_FLUSH);
		/* As we don't want to occupy too much space
		 * in block cache, we set this buffer BC_TMP.*/
		ext4_bcache_set_flag(block->buf, BC_TMP);
	}

	return rc;
}

/**@brief   jbd block get function (through cache, don't read).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @param   fblock jbd logical block address
 * @return  standard error code*/
static int jbd_block_get_noread(struct jbd_fs *jbd_fs,
			 struct ext4_block *block,
			 ext4_fsblk_t fblock)
{
	/* TODO: journal device. */
	int rc;
	struct ext4_blockdev *bdev = jbd_fs->bdev;
	ext4_lblk_t iblock = (ext4_lblk_t)fblock;
	rc = jbd_inode_bmap(jbd_fs, iblock,
			    &fblock);
	if (rc != EOK)
		return rc;

	rc = ext4_block_get_noread(bdev, block, fblock);
	if (rc == EOK)
		ext4_bcache_set_flag(block->buf, BC_FLUSH);

	return rc;
}

/**@brief   jbd block set procedure (through cache).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @return  standard error code*/
static int jbd_block_set(struct jbd_fs *jbd_fs,
		  struct ext4_block *block)
{
	struct ext4_blockdev *bdev = jbd_fs->bdev;
	return ext4_block_set(bdev, block);
}

/**@brief  helper functions to calculate
 *         block tag size, not including UUID part.
 * @param  jbd_fs jbd filesystem
 * @return tag size in bytes*/
static int jbd_tag_bytes(struct jbd_fs *jbd_fs)
{
	int size;

	/* It is very easy to deal with the case which
	 * JBD_FEATURE_INCOMPAT_CSUM_V3 is enabled.*/
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		return sizeof(struct jbd_block_tag3);

	size = sizeof(struct jbd_block_tag);

	/* If JBD_FEATURE_INCOMPAT_CSUM_V2 is enabled,
	 * add 2 bytes to size.*/
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V2))
		size += sizeof(uint16_t);

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		return size;

	/* If block number is 4 bytes in size,
	 * minus 4 bytes from size */
	return size - sizeof(uint32_t);
}

/**@brief  Tag information. */
struct tag_info {
	/**@brief  Tag size in bytes, including UUID part.*/
	int tag_bytes;

	/**@brief  block number stored in this tag.*/
	ext4_fsblk_t block;

	/**@brief  Is the first 4 bytes of block equals to
	 *	   JBD_MAGIC_NUMBER? */
	bool is_escape;

	/**@brief  whether UUID part exists or not.*/
	bool uuid_exist;

	/**@brief  UUID content if UUID part exists.*/
	uint8_t uuid[UUID_SIZE];

	/**@brief  Is this the last tag? */
	bool last_tag;

	/**@brief  crc32c checksum. */
	uint32_t checksum;
};

/**@brief  Extract information from a block tag.
 * @param  __tag pointer to the block tag
 * @param  tag_bytes block tag size of this jbd filesystem
 * @param  remain_buf_size size in buffer containing the block tag
 * @param  tag_info information of this tag.
 * @return  EOK when succeed, otherwise return EINVAL.*/
static int
jbd_extract_block_tag(struct jbd_fs *jbd_fs,
		      void *__tag,
		      int tag_bytes,
		      int32_t remain_buf_size,
		      struct tag_info *tag_info)
{
	char *uuid_start;
	tag_info->tag_bytes = tag_bytes;
	tag_info->uuid_exist = false;
	tag_info->last_tag = false;
	tag_info->is_escape = false;

	/* See whether it is possible to hold a valid block tag.*/
	if (remain_buf_size - tag_bytes < 0)
		return EINVAL;

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3)) {
		struct jbd_block_tag3 *tag = __tag;
		tag_info->block = jbd_get32(tag, blocknr);
		tag_info->checksum = jbd_get32(tag, checksum);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			 tag_info->block |=
				 (uint64_t)jbd_get32(tag, blocknr_high) << 32;

		if (jbd_get32(tag, flags) & JBD_FLAG_ESCAPE)
			tag_info->is_escape = true;

		if (!(jbd_get32(tag, flags) & JBD_FLAG_SAME_UUID)) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->uuid_exist = true;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(tag_info->uuid, uuid_start, UUID_SIZE);
		}

		if (jbd_get32(tag, flags) & JBD_FLAG_LAST_TAG)
			tag_info->last_tag = true;

	} else {
		struct jbd_block_tag *tag = __tag;
		tag_info->block = jbd_get32(tag, blocknr);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_CSUM_V2))
			tag_info->checksum = jbd_get16(tag, checksum);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			 tag_info->block |=
				 (uint64_t)jbd_get32(tag, blocknr_high) << 32;

		if (jbd_get16(tag, flags) & JBD_FLAG_ESCAPE)
			tag_info->is_escape = true;

		if (!(jbd_get16(tag, flags) & JBD_FLAG_SAME_UUID)) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->uuid_exist = true;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(tag_info->uuid, uuid_start, UUID_SIZE);
		}

		if (jbd_get16(tag, flags) & JBD_FLAG_LAST_TAG)
			tag_info->last_tag = true;

	}
	return EOK;
}

static bool jbd_verify_block_tag_csum(struct jbd_fs *jbd_fs,
				      const struct tag_info *tag_info,
				      const void *block, uint32_t sequence)
{
	uint32_t checksum;

	if (!jbd_has_csum_v2or3(&jbd_fs->sb))
		return true;

	checksum = jbd_block_csum(jbd_fs, block, 0, sequence);
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		return tag_info->checksum == checksum;

	return (uint16_t)tag_info->checksum == (uint16_t)checksum;
}

/**@brief  Write information to a block tag.
 * @param  __tag pointer to the block tag
 * @param  remain_buf_size size in buffer containing the block tag
 * @param  tag_info information of this tag.
 * @return  EOK when succeed, otherwise return EINVAL.*/
static int
jbd_write_block_tag(struct jbd_fs *jbd_fs,
		    void *__tag,
		    int32_t remain_buf_size,
		    struct tag_info *tag_info)
{
	char *uuid_start;
	int tag_bytes = jbd_tag_bytes(jbd_fs);

	tag_info->tag_bytes = tag_bytes;

	/* See whether it is possible to hold a valid block tag.*/
	if (remain_buf_size - tag_bytes < 0)
		return EINVAL;

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3)) {
		struct jbd_block_tag3 *tag = __tag;
		memset(tag, 0, tag_bytes);
		jbd_set32(tag, blocknr, (uint32_t)tag_info->block);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			jbd_set32(tag, blocknr_high, tag_info->block >> 32);

		if (tag_info->uuid_exist) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(uuid_start, tag_info->uuid, UUID_SIZE);
		} else
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_SAME_UUID);

		jbd_block_tag_csum_set(jbd_fs, __tag, tag_info->checksum);

		if (tag_info->last_tag)
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_LAST_TAG);

		if (tag_info->is_escape)
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_ESCAPE);

	} else {
		struct jbd_block_tag *tag = __tag;
		memset(tag, 0, tag_bytes);
		jbd_set32(tag, blocknr, (uint32_t)tag_info->block);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			jbd_set32(tag, blocknr_high, tag_info->block >> 32);

		if (tag_info->uuid_exist) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(uuid_start, tag_info->uuid, UUID_SIZE);
		} else
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_SAME_UUID);

		jbd_block_tag_csum_set(jbd_fs, __tag, tag_info->checksum);

		if (tag_info->last_tag)
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_LAST_TAG);


		if (tag_info->is_escape)
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_ESCAPE);

	}
	return EOK;
}

/**@brief  Iterate all block tags in a block.
 * @param  jbd_fs jbd filesystem
 * @param  __tag_start pointer to the block
 * @param  tag_tbl_size size of the block
 * @param  func callback routine to indicate that
 *         a block tag is found
 * @param  arg additional argument to be passed to func */
static int
jbd_iterate_block_table(struct jbd_fs *jbd_fs,
			void *__tag_start,
			int32_t tag_tbl_size,
			void (*func)(struct jbd_fs * jbd_fs,
				     struct tag_info *tag_info,
				     void *arg),
			void *arg)
{
	char *tag_start, *tag_ptr;
	int tag_bytes = jbd_tag_bytes(jbd_fs);
	tag_start = __tag_start;
	tag_ptr = tag_start;

	/* Cut off the size of block tail storing checksum. */
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V2) ||
	    JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		tag_tbl_size -= sizeof(struct jbd_block_tail);

	while (tag_tbl_size) {
		struct tag_info tag_info;
		int rc = jbd_extract_block_tag(jbd_fs,
				      tag_ptr,
				      tag_bytes,
				      tag_tbl_size,
				      &tag_info);
		if (rc != EOK)
			return rc;

		if (func)
			func(jbd_fs, &tag_info, arg);

		/* Stop the iteration when we reach the last tag. */
		if (tag_info.last_tag)
			break;

		tag_ptr += tag_info.tag_bytes;
		tag_tbl_size -= tag_info.tag_bytes;
	}
	return EOK;
}

struct jbd_scan_tag_arg {
	uint32_t *next_block;
	uint32_t sequence;
	uint32_t legacy_checksum;
	int rc;
};

static void jbd_scan_descriptor_tag(struct jbd_fs *jbd_fs,
				    struct tag_info *tag_info,
				    void *__arg)
{
	struct jbd_scan_tag_arg *arg = __arg;
	struct ext4_block block;
	uint64_t block_count = ext4_sb_get_blocks_cnt(
		&jbd_fs->inode_ref.fs->sb);

	if (arg->rc != EOK)
		return;
	if (tag_info->block >= block_count) {
		arg->rc = EIO;
		return;
	}

	(*arg->next_block)++;
	wrap(&jbd_fs->sb, *arg->next_block);
	arg->rc = jbd_block_get(jbd_fs, &block, *arg->next_block);
	if (arg->rc != EOK)
		return;

	if (JBD_HAS_COMPAT_FEATURE(&jbd_fs->sb,
				   JBD_FEATURE_COMPAT_CHECKSUM)) {
		arg->legacy_checksum = jbd_crc32_be(arg->legacy_checksum,
						     block.data,
						     jbd_get32(&jbd_fs->sb,
							       blocksize));
	} else if (!jbd_verify_block_tag_csum(jbd_fs, tag_info, block.data,
					      arg->sequence)) {
		arg->rc = EIO;
	}
	jbd_block_set(jbd_fs, &block);
}

static int jbd_scan_descriptor_block(struct jbd_fs *jbd_fs,
				     struct jbd_bhdr *header,
				     uint32_t *this_block,
				     uint32_t sequence,
				     uint32_t *legacy_checksum)
{
	struct jbd_scan_tag_arg arg = {
		.next_block = this_block,
		.sequence = sequence,
		.legacy_checksum = *legacy_checksum,
		.rc = EOK
	};
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	int rc;

	if (JBD_HAS_COMPAT_FEATURE(&jbd_fs->sb,
				   JBD_FEATURE_COMPAT_CHECKSUM))
		arg.legacy_checksum = jbd_crc32_be(arg.legacy_checksum, header,
						   block_size);

	rc = jbd_iterate_block_table(jbd_fs, header + 1,
				     block_size - sizeof(struct jbd_bhdr),
				     jbd_scan_descriptor_tag, &arg);
	if (rc != EOK)
		return rc;
	if (arg.rc != EOK)
		return arg.rc;
	*legacy_checksum = arg.legacy_checksum;
	return EOK;
}

static void jbd_display_block_tags(struct jbd_fs *jbd_fs,
				   struct tag_info *tag_info,
				   void *arg)
{
	uint32_t *iblock = arg;
	ext4_dbg(DEBUG_JBD, "Block in block_tag: %" PRIu64 "\n", tag_info->block);
	(*iblock)++;
	wrap(&jbd_fs->sb, *iblock);
	(void)jbd_fs;
	return;
}

static struct revoke_entry *
jbd_revoke_entry_lookup(struct recover_info *info, ext4_fsblk_t block)
{
	struct revoke_entry tmp = {
		.block = block
	};

	return RB_FIND(jbd_revoke, &info->revoke_root, &tmp);
}

/**@brief  Replay a block in a transaction.
 * @param  jbd_fs jbd filesystem
 * @param  tag_info tag_info of the logged block.*/
static void jbd_replay_block_tags(struct jbd_fs *jbd_fs,
				  struct tag_info *tag_info,
				  void *__arg)
{
	int r;
	struct replay_arg *arg = __arg;
	struct recover_info *info = arg->info;
	uint32_t *this_block = arg->this_block;
	struct revoke_entry *revoke_entry;
	struct ext4_block journal_block, ext4_block;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;

	(*this_block)++;
	wrap(&jbd_fs->sb, *this_block);

	/* We replay this block only if the current transaction id
	 * is equal or greater than that in revoke entry.*/
	revoke_entry = jbd_revoke_entry_lookup(info, tag_info->block);
	if (revoke_entry &&
	    trans_id_diff(arg->this_trans_id, revoke_entry->trans_id) <= 0)
		return;

	ext4_dbg(DEBUG_JBD,
		 "Replaying block in block_tag: %" PRIu64 "\n",
		 tag_info->block);

	r = jbd_block_get(jbd_fs, &journal_block, *this_block);
	if (r != EOK)
		return;

	/* We need special treatment for ext4 superblock. */
	if (tag_info->block) {
		r = ext4_block_get_noread(fs->bdev, &ext4_block, tag_info->block);
		if (r != EOK) {
			jbd_block_set(jbd_fs, &journal_block);
			return;
		}

		memcpy(ext4_block.data,
			journal_block.data,
			jbd_get32(&jbd_fs->sb, blocksize));

		if (tag_info->is_escape)
			((struct jbd_bhdr *)ext4_block.data)->magic =
					to_be32(JBD_MAGIC_NUMBER);

		ext4_bcache_set_dirty(ext4_block.buf);
		ext4_block_set(fs->bdev, &ext4_block);
	} else {
		uint16_t mount_count, state;
		mount_count = ext4_get16(&fs->sb, mount_count);
		state = ext4_get16(&fs->sb, state);

		memcpy(&fs->sb,
			journal_block.data + EXT4_SUPERBLOCK_OFFSET,
			EXT4_SUPERBLOCK_SIZE);

		/* Mark system as mounted */
		ext4_set16(&fs->sb, state, state);
		r = ext4_sb_write(fs->bdev, &fs->sb);
		if (r != EOK)
			return;

		/*Update mount count*/
		ext4_set16(&fs->sb, mount_count, mount_count);
	}

	jbd_block_set(jbd_fs, &journal_block);
	
	return;
}

/**@brief  Add block address to revoke tree, along with
 *         its transaction id.
 * @param  info  journal replay info
 * @param  block  block address to be replayed.*/
static void jbd_add_revoke_block_tags(struct recover_info *info,
				      ext4_fsblk_t block)
{
	struct revoke_entry *revoke_entry;

	ext4_dbg(DEBUG_JBD, "Add block %" PRIu64 " to revoke tree\n", block);
	/* If the revoke entry with respect to the block address
	 * exists already, update its transaction id.*/
	revoke_entry = jbd_revoke_entry_lookup(info, block);
	if (revoke_entry) {
		revoke_entry->trans_id = info->this_trans_id;
		return;
	}

	revoke_entry = jbd_alloc_revoke_entry();
	ext4_assert(revoke_entry);
	revoke_entry->block = block;
	revoke_entry->trans_id = info->this_trans_id;
	RB_INSERT(jbd_revoke, &info->revoke_root, revoke_entry);

	return;
}

static void jbd_destroy_revoke_tree(struct recover_info *info)
{
	while (!RB_EMPTY(&info->revoke_root)) {
		struct revoke_entry *revoke_entry =
			RB_MIN(jbd_revoke, &info->revoke_root);
		ext4_assert(revoke_entry);
		RB_REMOVE(jbd_revoke, &info->revoke_root, revoke_entry);
		jbd_free_revoke_entry(revoke_entry);
	}
}

static int jbd_validate_revoke_block(struct jbd_fs *jbd_fs,
				     struct jbd_bhdr *header)
{
	struct jbd_revoke_header *revoke_hdr =
		(struct jbd_revoke_header *)header;
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	uint32_t count = jbd_get32(revoke_hdr, count);
	uint32_t record_len = JBD_HAS_INCOMPAT_FEATURE(
		&jbd_fs->sb, JBD_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
	uint32_t tail_size = jbd_has_csum_v2or3(&jbd_fs->sb) ?
		sizeof(struct jbd_revoke_tail) : 0;
	uint64_t block_count = ext4_sb_get_blocks_cnt(
		&jbd_fs->inode_ref.fs->sb);
	uint32_t offset;

	if (count < sizeof(*revoke_hdr) || count > block_size - tail_size ||
	    (count - sizeof(*revoke_hdr)) % record_len)
		return EIO;

	for (offset = sizeof(*revoke_hdr); offset < count;
	     offset += record_len) {
		uint64_t block;
		if (record_len == 8) {
			uint64_t value;
			memcpy(&value, (uint8_t *)header + offset, sizeof(value));
			block = to_be64(value);
		} else {
			uint32_t value;
			memcpy(&value, (uint8_t *)header + offset, sizeof(value));
			block = to_be32(value);
		}
		if (block >= block_count)
			return EIO;
	}

	return EOK;
}


#define ACTION_SCAN 0
#define ACTION_REVOKE 1
#define ACTION_RECOVER 2

/**@brief  Add entries in a revoke block to revoke tree.
 * @param  jbd_fs jbd filesystem
 * @param  header revoke block header
 * @param  info  journal replay info*/
static int jbd_build_revoke_tree(struct jbd_fs *jbd_fs,
				  struct jbd_bhdr *header,
				  struct recover_info *info)
{
	char *blocks_entry;
	struct jbd_revoke_header *revoke_hdr =
		(struct jbd_revoke_header *)header;
	uint32_t i, nr_entries, record_len = 4;
	int rc = jbd_validate_revoke_block(jbd_fs, header);
	if (rc != EOK)
		return rc;

	/* If we are working on a 64bit jbd filesystem, */
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		record_len = 8;

	nr_entries = (jbd_get32(revoke_hdr, count) -
			sizeof(struct jbd_revoke_header)) /
			record_len;

	blocks_entry = (char *)(revoke_hdr + 1);

	for (i = 0;i < nr_entries;i++) {
		if (record_len == 8) {
			uint64_t *blocks =
				(uint64_t *)blocks_entry;
			jbd_add_revoke_block_tags(info, to_be64(*blocks));
		} else {
			uint32_t *blocks =
				(uint32_t *)blocks_entry;
			jbd_add_revoke_block_tags(info, to_be32(*blocks));
		}
		blocks_entry += record_len;
	}
	return EOK;
}

static void jbd_debug_descriptor_block(struct jbd_fs *jbd_fs,
				       struct jbd_bhdr *header,
				       uint32_t *iblock)
{
	(void)jbd_iterate_block_table(jbd_fs,
				header + 1,
				jbd_get32(&jbd_fs->sb, blocksize) -
					sizeof(struct jbd_bhdr),
				jbd_display_block_tags,
				iblock);
}

static void jbd_replay_descriptor_block(struct jbd_fs *jbd_fs,
					struct jbd_bhdr *header,
					struct replay_arg *arg)
{
	(void)jbd_iterate_block_table(jbd_fs,
				header + 1,
				jbd_get32(&jbd_fs->sb, blocksize) -
					sizeof(struct jbd_bhdr),
				jbd_replay_block_tags,
				arg);
}

#define JBD_FC_TAG_ADD_RANGE 0x0001
#define JBD_FC_TAG_DEL_RANGE 0x0002
#define JBD_FC_TAG_CREAT 0x0003
#define JBD_FC_TAG_LINK 0x0004
#define JBD_FC_TAG_UNLINK 0x0005
#define JBD_FC_TAG_INODE 0x0006
#define JBD_FC_TAG_PAD 0x0007
#define JBD_FC_TAG_TAIL 0x0008
#define JBD_FC_TAG_HEAD 0x0009
#define JBD_FC_TAG_HEADER_SIZE 4

struct jbd_fc_scan_result {
	uint32_t committed_tags;
	uint32_t expected_tid;
};

struct jbd_fc_pending_map {
	struct ext4_inode_ref inode_ref;
	struct ext4_extent_map_entry *entries;
	size_t count;
	uint32_t inode_number;
	bool active;
};

static uint16_t jbd_fc_get_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t jbd_fc_get_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t jbd_fc_get_le48(const uint8_t *data)
{
	return (uint64_t)jbd_fc_get_le16(data) << 32 |
	       jbd_fc_get_le32(data + 2);
}

static bool jbd_fc_valid_name(const uint8_t *name, uint16_t length)
{
	uint16_t i;
	if (!length || length > EXT4_DIRECTORY_FILENAME_LEN ||
	    (length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return false;
	for (i = 0; i < length; ++i)
		if (!name[i] || name[i] == '/')
			return false;
	return true;
}

static int jbd_fc_validate_tag(struct jbd_fs *jbd_fs, uint16_t tag,
			       const uint8_t *value, uint16_t length)
{
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint32_t inodes_count = ext4_get32(&fs->sb, inodes_count);

	switch (tag) {
	case JBD_FC_TAG_HEAD:
		return length == 8 ? EOK : EIO;
	case JBD_FC_TAG_ADD_RANGE: {
		uint32_t ino;
		uint32_t logical;
		uint16_t raw_length;
		uint32_t blocks;
		uint64_t physical;
		if (length != 16)
			return EIO;
		ino = jbd_fc_get_le32(value);
		logical = jbd_fc_get_le32(value + 4);
		raw_length = jbd_fc_get_le16(value + 8);
		blocks = raw_length > 32768 ? raw_length - 32768 : raw_length;
		physical = jbd_fc_get_le48(value + 10);
		if (!ino || ino > inodes_count || !blocks ||
		    (uint64_t)logical + blocks > (uint64_t)UINT32_MAX + 1 ||
		    physical + blocks > fs->bdev->lg_bcnt)
			return EIO;
		return EOK;
	}
	case JBD_FC_TAG_DEL_RANGE:
		if (length != 12 || !jbd_fc_get_le32(value) ||
		    jbd_fc_get_le32(value) > inodes_count ||
		    !jbd_fc_get_le32(value + 8) ||
		    (uint64_t)jbd_fc_get_le32(value + 4) +
			jbd_fc_get_le32(value + 8) > (uint64_t)UINT32_MAX + 1)
			return EIO;
		return EOK;
	case JBD_FC_TAG_CREAT:
	case JBD_FC_TAG_LINK:
	case JBD_FC_TAG_UNLINK:
		if (length < 9 || !jbd_fc_get_le32(value) ||
		    jbd_fc_get_le32(value) > inodes_count ||
		    !jbd_fc_get_le32(value + 4) ||
		    jbd_fc_get_le32(value + 4) > inodes_count ||
		    !jbd_fc_valid_name(value + 8, length - 8))
			return EIO;
		return EOK;
	case JBD_FC_TAG_INODE: {
		uint32_t ino;
		uint16_t inode_size = ext4_get16(&fs->sb, inode_size);
		uint16_t extra_isize;
		uint16_t raw_length;
		if (length < sizeof(uint32_t) + EXT4_REV0_INODE_SIZE)
			return EIO;
		ino = jbd_fc_get_le32(value);
		raw_length = length - sizeof(uint32_t);
		if (!ino || ino > inodes_count)
			return EIO;
		if (inode_size == EXT4_REV0_INODE_SIZE)
			return raw_length == EXT4_REV0_INODE_SIZE ? EOK : EIO;
		extra_isize = jbd_fc_get_le16(value + sizeof(uint32_t) +
					     offsetof(struct ext4_inode, extra_isize));
		if (extra_isize < 4 ||
		    extra_isize > inode_size - EXT4_REV0_INODE_SIZE ||
		    raw_length != EXT4_REV0_INODE_SIZE + extra_isize)
			return EIO;
		return EOK;
	}
	case JBD_FC_TAG_PAD:
		return EOK;
	case JBD_FC_TAG_TAIL:
		return length >= 8 ? EOK : EIO;
	default:
		return ENOTSUP;
	}
}

static int jbd_fc_scan(struct jbd_fs *jbd_fs, uint32_t expected_tid,
		       struct jbd_fc_scan_result *result)
{
	struct jbd_sb *sb = &jbd_fs->sb;
	uint32_t block_size = jbd_get32(sb, blocksize);
	uint32_t fc_blocks = jbd_fc_blocks(sb);
	uint32_t first;
	uint32_t tags = 0;
	uint32_t committed_tags = 0;
	uint32_t block_index;
	uint32_t crc = 0;
	bool saw_head = false;
	bool stop = false;

	memset(result, 0, sizeof(*result));
	result->expected_tid = expected_tid;
	if (fc_blocks <= 1 ||
	    !JBD_HAS_INCOMPAT_FEATURE(sb, JBD_FEATURE_INCOMPAT_FAST_COMMIT))
		return EOK;
	first = jbd_fc_first_block(sb);
	for (block_index = 0; block_index < fc_blocks - 1 && !stop;
	     ++block_index) {
		struct ext4_block block;
		size_t offset = 0;
		int rc = jbd_block_get(jbd_fs, &block, first + block_index);
		if (rc != EOK)
			return rc;
		while (offset + JBD_FC_TAG_HEADER_SIZE <= block_size) {
			const uint8_t *data = block.data + offset;
			uint16_t tag = jbd_fc_get_le16(data);
			uint16_t length = jbd_fc_get_le16(data + 2);
			const uint8_t *value = data + JBD_FC_TAG_HEADER_SIZE;
			size_t total = JBD_FC_TAG_HEADER_SIZE + length;
			if (tag == 0) {
				stop = true;
				break;
			}
			if (total > block_size - offset) {
				if (committed_tags) {
					stop = true;
					rc = EOK;
				} else {
					rc = EIO;
				}
				break;
			}
			if (!saw_head && tag != JBD_FC_TAG_HEAD) {
				stop = true;
				break;
			}
			rc = jbd_fc_validate_tag(jbd_fs, tag, value, length);
			if (rc != EOK) {
				if (committed_tags) {
					stop = true;
					rc = EOK;
				}
				break;
			}
			if (tag == JBD_FC_TAG_PAD || tag == JBD_FC_TAG_TAIL) {
				if (total != block_size - offset) {
					if (committed_tags) {
						stop = true;
						rc = EOK;
					} else {
					rc = EIO;
					}
					break;
				}
			}
			if (tag == JBD_FC_TAG_HEAD) {
				if (saw_head) {
					rc = EIO;
					if (committed_tags) {
						stop = true;
						rc = EOK;
					}
					break;
				}
				if (jbd_fc_get_le32(value) != 0) {
					rc = ENOTSUP;
					break;
				}
				if (jbd_fc_get_le32(value + 4) != expected_tid) {
					stop = true;
					rc = EOK;
					break;
				}
				saw_head = true;
				crc = ext4_crc32c(crc, data, total);
				++tags;
			} else if (tag == JBD_FC_TAG_TAIL) {
				uint32_t tid = jbd_fc_get_le32(value);
				uint32_t stored_crc = jbd_fc_get_le32(value + 4);
				uint32_t tail_crc = ext4_crc32c(
					crc, data,
					JBD_FC_TAG_HEADER_SIZE + sizeof(uint32_t));
				if (tid != expected_tid || stored_crc != tail_crc) {
					rc = EIO;
					if (committed_tags) {
						stop = true;
						rc = EOK;
					}
					break;
				}
				++tags;
				committed_tags = tags;
				crc = 0;
			} else {
				crc = ext4_crc32c(crc, data, total);
				++tags;
			}
			if (tag == JBD_FC_TAG_PAD || tag == JBD_FC_TAG_TAIL)
				break;
			offset += total;
		}
		{
			int close_rc = jbd_block_set(jbd_fs, &block);
			if (rc == EOK)
				rc = close_rc;
		}
		if (rc != EOK)
			return rc;
	}
	result->committed_tags = committed_tags;
	return EOK;
}

static int jbd_fc_flush_map(struct jbd_fc_pending_map *pending)
{
	int rc = EOK;
	if (!pending->active)
		return EOK;
	rc = ext4_extent_replace(&pending->inode_ref, pending->entries,
				 pending->count);
	ext4_free(pending->entries);
	pending->entries = NULL;
	pending->count = 0;
	if (ext4_fs_put_inode_ref(&pending->inode_ref) != EOK && rc == EOK)
		rc = EIO;
	memset(pending, 0, sizeof(*pending));
	return rc;
}

static int jbd_fc_load_map(struct ext4_fs *fs,
			   struct jbd_fc_pending_map *pending, uint32_t ino)
{
	int rc;
	rc = ext4_fs_get_inode_ref(fs, ino, &pending->inode_ref);
	if (rc != EOK)
		return rc;
	if (!ext4_inode_has_flag(pending->inode_ref.inode,
				 EXT4_INODE_FLAG_EXTENTS)) {
		ext4_fs_put_inode_ref(&pending->inode_ref);
		return ENOTSUP;
	}
	rc = ext4_extent_collect(&pending->inode_ref, &pending->entries,
				 &pending->count);
	if (rc != EOK) {
		ext4_fs_put_inode_ref(&pending->inode_ref);
		return rc;
	}
	pending->inode_number = ino;
	pending->active = true;
	return EOK;
}

static int jbd_fc_change_map(struct ext4_fs *fs,
			     struct jbd_fc_pending_map *pending, uint32_t ino,
			     uint32_t logical, uint32_t length,
			     ext4_fsblk_t physical, bool unwritten, bool add)
{
	struct ext4_extent_map_entry *updated;
	size_t capacity;
	size_t count = 0;
	size_t i;
	uint64_t end = (uint64_t)logical + length;
	int rc;

	if (pending->active && pending->inode_number != ino) {
		rc = jbd_fc_flush_map(pending);
		if (rc != EOK)
			return rc;
	}
	if (!pending->active) {
		rc = jbd_fc_load_map(fs, pending, ino);
		if (rc != EOK)
			return rc;
	}
	if (pending->count > SIZE_MAX - 2)
		return ENOMEM;
	capacity = pending->count + 2;
	updated = ext4_calloc(capacity, sizeof(*updated));
	if (!updated)
		return ENOMEM;
	for (i = 0; i < pending->count; ++i) {
		struct ext4_extent_map_entry current = pending->entries[i];
		uint64_t current_end = (uint64_t)current.logical + current.length;
		if (current_end <= logical || current.logical >= end) {
			updated[count++] = current;
			continue;
		}
		if (current.logical < logical) {
			struct ext4_extent_map_entry left = current;
			left.length = logical - current.logical;
			updated[count++] = left;
		}
		if (current_end > end) {
			struct ext4_extent_map_entry right = current;
			right.logical = (uint32_t)end;
			right.physical += end - current.logical;
			right.length = (uint32_t)(current_end - end);
			updated[count++] = right;
		}
	}
	if (add) {
		struct ext4_extent_map_entry item;
		size_t position = count;
		item.logical = logical;
		item.length = length;
		item.physical = physical;
		item.unwritten = unwritten;
		while (position && updated[position - 1].logical > logical) {
			updated[position] = updated[position - 1];
			position--;
		}
		updated[position] = item;
		count++;
	}
	ext4_free(pending->entries);
	pending->entries = updated;
	pending->count = count;
	return EOK;
}

static int jbd_fc_apply_inode(struct ext4_fs *fs, const uint8_t *value,
			      uint16_t length)
{
	uint32_t ino = jbd_fc_get_le32(value);
	const uint8_t *snapshot = value + sizeof(uint32_t);
	uint16_t snapshot_length = length - sizeof(uint32_t);
	struct ext4_inode_ref ref;
	uint64_t blocks_count;
	uint32_t flags;
	uint16_t links_count;
	bool inline_data;
	bool is_dir;
	int rc;

	rc = ext4_fs_get_inode_ref(fs, ino, &ref);
	if (rc != EOK)
		return rc;
	blocks_count = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
	flags = jbd_fc_get_le32(snapshot +
			       offsetof(struct ext4_inode, flags));
	inline_data = (flags & EXT4_INODE_FLAG_INLINE_DATA) != 0;
	memcpy(ref.inode, snapshot, offsetof(struct ext4_inode, blocks));
	if (inline_data) {
		memcpy(ref.inode->blocks,
		       snapshot + offsetof(struct ext4_inode, blocks),
		       sizeof(ref.inode->blocks));
	}
	if (snapshot_length > offsetof(struct ext4_inode, generation))
		memcpy(&ref.inode->generation,
		       snapshot + offsetof(struct ext4_inode, generation),
		       snapshot_length - offsetof(struct ext4_inode, generation));
	if (flags & EXT4_INODE_FLAG_EXTENTS) {
		if (jbd_fc_get_le16((const uint8_t *)ref.inode->blocks) != 0xF30A)
			ext4_extent_tree_init(&ref);
		if (!inline_data) {
			rc = ext4_inode_set_blocks_count(&fs->sb, ref.inode,
						 blocks_count);
			if (rc != EOK) {
				ext4_fs_put_inode_ref(&ref);
				return rc;
			}
		}
	}
	ref.dirty = true;
	is_dir = ext4_inode_is_type(&fs->sb, ref.inode,
				    EXT4_INODE_MODE_DIRECTORY);
	links_count = ext4_inode_get_links_cnt(ref.inode);
	rc = ext4_fs_put_inode_ref(&ref);
	if (rc != EOK)
		return rc;
	return ext4_ialloc_set_inode_allocated(fs, ino, is_dir,
					       links_count != 0);
}

static int jbd_fc_apply_dentry(struct ext4_fs *fs, uint16_t tag,
			       const uint8_t *value, uint16_t length)
{
	uint32_t parent_ino = jbd_fc_get_le32(value);
	uint32_t child_ino = jbd_fc_get_le32(value + 4);
	uint16_t name_length = length - 8;
	char name[EXT4_DIRECTORY_FILENAME_LEN + 1];
	struct ext4_inode_ref parent;
	struct ext4_inode_ref child;
	struct ext4_dir_search_result found;
	uint32_t found_ino = 0;
	int find_rc;
	int rc;

	memcpy(name, value + 8, name_length);
	name[name_length] = 0;
	rc = ext4_fs_get_inode_ref(fs, parent_ino, &parent);
	if (rc != EOK)
		return rc;
	rc = ext4_fs_get_inode_ref(fs, child_ino, &child);
	if (rc != EOK) {
		ext4_fs_put_inode_ref(&parent);
		return rc;
	}
	if (!ext4_inode_is_type(&fs->sb, parent.inode,
				EXT4_INODE_MODE_DIRECTORY)) {
		ext4_fs_put_inode_ref(&child);
		ext4_fs_put_inode_ref(&parent);
		return ENOTDIR;
	}
	find_rc = ext4_dir_find_entry(&found, &parent, name, name_length);
	if (find_rc == EOK) {
		found_ino = ext4_dir_en_get_inode(found.dentry);
		ext4_dir_destroy_result(&parent, &found);
	} else if (find_rc != ENOENT) {
		ext4_fs_put_inode_ref(&child);
		ext4_fs_put_inode_ref(&parent);
		return find_rc;
	}
	if (tag == JBD_FC_TAG_UNLINK) {
		rc = found_ino == child_ino
			? ext4_dir_remove_entry(&parent, name, name_length) : EOK;
	} else if (found_ino == child_ino) {
		rc = EOK;
	} else {
		if (found_ino)
			rc = ext4_dir_remove_entry(&parent, name, name_length);
		else
			rc = EOK;
		if (rc == EOK)
			rc = ext4_dir_add_entry(&parent, name, name_length, &child);
	}
	if (ext4_fs_put_inode_ref(&child) != EOK && rc == EOK)
		rc = EIO;
	if (ext4_fs_put_inode_ref(&parent) != EOK && rc == EOK)
		rc = EIO;
	return rc;
}

static int jbd_fc_replay(struct jbd_fs *jbd_fs,
			 const struct jbd_fc_scan_result *scan)
{
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	struct jbd_fc_pending_map pending = {0};
	uint32_t block_size = jbd_get32(&jbd_fs->sb, blocksize);
	uint32_t fc_blocks = jbd_fc_blocks(&jbd_fs->sb);
	uint32_t first = jbd_fc_first_block(&jbd_fs->sb);
	uint32_t remaining = scan->committed_tags;
	uint32_t block_index;
	int rc = EOK;

	if (!remaining)
		return EOK;
	for (block_index = 0; block_index < fc_blocks - 1 && remaining;
	     ++block_index) {
		struct ext4_block block;
		size_t offset = 0;
		rc = jbd_block_get(jbd_fs, &block, first + block_index);
		if (rc != EOK)
			goto done;
		while (offset + JBD_FC_TAG_HEADER_SIZE <= block_size && remaining) {
			const uint8_t *data = block.data + offset;
			uint16_t tag = jbd_fc_get_le16(data);
			uint16_t length = jbd_fc_get_le16(data + 2);
			const uint8_t *value = data + JBD_FC_TAG_HEADER_SIZE;
			size_t total = JBD_FC_TAG_HEADER_SIZE + length;
			if (!tag || total > block_size - offset) {
				rc = EIO;
				break;
			}
			rc = EOK;
			if (tag == JBD_FC_TAG_ADD_RANGE ||
			    tag == JBD_FC_TAG_DEL_RANGE) {
				uint32_t ino = jbd_fc_get_le32(value);
				uint32_t logical;
				uint32_t blocks;
				ext4_fsblk_t physical = 0;
				bool unwritten = false;
				if (tag == JBD_FC_TAG_ADD_RANGE) {
					uint16_t raw_length = jbd_fc_get_le16(value + 8);
					logical = jbd_fc_get_le32(value + 4);
					blocks = raw_length > 32768
						? raw_length - 32768 : raw_length;
					physical = jbd_fc_get_le48(value + 10);
					unwritten = raw_length > 32768;
				} else {
					logical = jbd_fc_get_le32(value + 4);
					blocks = jbd_fc_get_le32(value + 8);
				}
				rc = jbd_fc_change_map(fs, &pending, ino, logical,
						       blocks, physical, unwritten,
						       tag == JBD_FC_TAG_ADD_RANGE);
			} else {
				rc = jbd_fc_flush_map(&pending);
				if (rc == EOK && (tag == JBD_FC_TAG_CREAT ||
						  tag == JBD_FC_TAG_LINK ||
						  tag == JBD_FC_TAG_UNLINK))
					rc = jbd_fc_apply_dentry(fs, tag, value,
								 length);
				else if (rc == EOK && tag == JBD_FC_TAG_INODE)
					rc = jbd_fc_apply_inode(fs, value, length);
				else if (rc == EOK && tag != JBD_FC_TAG_HEAD &&
					 tag != JBD_FC_TAG_PAD &&
					 tag != JBD_FC_TAG_TAIL)
					rc = ENOTSUP;
			}
			if (rc != EOK)
				break;
			--remaining;
			if (tag == JBD_FC_TAG_PAD || tag == JBD_FC_TAG_TAIL)
				break;
			offset += total;
		}
		{
			int close_rc = jbd_block_set(jbd_fs, &block);
			if (rc == EOK)
				rc = close_rc;
		}
		if (rc != EOK)
			goto done;
	}
	if (remaining)
		rc = EIO;
	done:
	if (jbd_fc_flush_map(&pending) != EOK && rc == EOK)
		rc = EIO;
	return rc;
}

/**@brief  The core routine of journal replay.
 * @param  jbd_fs jbd filesystem
 * @param  info  journal replay info
 * @param  action action needed to be taken
 * @return standard error code*/
static int jbd_iterate_log(struct jbd_fs *jbd_fs,
			   struct recover_info *info,
			   int action)
{
	int r = EOK;
	bool log_end = false;
	struct jbd_sb *sb = &jbd_fs->sb;
	uint32_t start_trans_id, this_trans_id;
	uint32_t start_block, this_block;
	uint32_t legacy_checksum = EXT4_CRC32_INIT;

	/* We start iterating valid blocks in the whole journal.*/
	start_trans_id = this_trans_id = jbd_get32(sb, sequence);
	start_block = this_block = jbd_get32(sb, start);
	if (action == ACTION_SCAN)
		info->trans_cnt = 0;
	else if (!info->trans_cnt)
		log_end = true;

	ext4_dbg(DEBUG_JBD, "Start of journal at trans id: %" PRIu32 "\n",
			    start_trans_id);

	while (!log_end) {
		struct ext4_block block;
		struct jbd_bhdr *header;
		/* If we are not scanning for the last
		 * valid transaction in the journal,
		 * we will stop when we reach the end of
		 * the journal.*/
		if (action != ACTION_SCAN)
			if (trans_id_diff(this_trans_id, info->last_trans_id) > 0) {
				log_end = true;
				continue;
			}

		r = jbd_block_get(jbd_fs, &block, this_block);
		if (r != EOK)
			break;

		header = (struct jbd_bhdr *)block.data;
		/* This block does not have a valid magic number,
		 * so we have reached the end of the journal.*/
		if (jbd_get32(header, magic) != JBD_MAGIC_NUMBER) {
			jbd_block_set(jbd_fs, &block);
			log_end = true;
			continue;
		}

		/* If the transaction id we found is not expected,
		 * we may have reached the end of the journal.
		 *
		 * If we are not scanning the journal, something
		 * bad might have taken place. :-( */
		if (jbd_get32(header, sequence) != this_trans_id) {
			if (action != ACTION_SCAN)
				r = EIO;

			jbd_block_set(jbd_fs, &block);
			log_end = true;
			continue;
		}

		switch (jbd_get32(header, blocktype)) {
		case JBD_DESCRIPTOR_BLOCK:
			if (!jbd_verify_meta_csum(jbd_fs, header)) {
				ext4_dbg(DEBUG_JBD,
					DBG_WARN "Descriptor block checksum failed."
						"Journal block: %" PRIu32"\n",
						this_block);
				r = EIO;
				log_end = true;
				break;
			}
			ext4_dbg(DEBUG_JBD, "Descriptor block: %" PRIu32", "
					    "trans_id: %" PRIu32"\n",
					    this_block, this_trans_id);
			if (action == ACTION_RECOVER) {
				struct replay_arg replay_arg;
				replay_arg.info = info;
				replay_arg.this_block = &this_block;
				replay_arg.this_trans_id = this_trans_id;

				jbd_replay_descriptor_block(jbd_fs,
						header, &replay_arg);
			} else if (action == ACTION_SCAN) {
				r = jbd_scan_descriptor_block(jbd_fs, header,
							      &this_block,
							      this_trans_id,
							      &legacy_checksum);
				if (r != EOK) {
					log_end = true;
					break;
				}
			} else
				jbd_debug_descriptor_block(jbd_fs,
						header, &this_block);

			break;
		case JBD_COMMIT_BLOCK:
			if (!jbd_verify_commit_csum(jbd_fs,
					(struct jbd_commit_header *)header)) {
				ext4_dbg(DEBUG_JBD,
					DBG_WARN "Commit block checksum failed."
						"Journal block: %" PRIu32"\n",
						this_block);
				r = EIO;
				log_end = true;
				break;
			}
			if (action == ACTION_SCAN &&
			    JBD_HAS_COMPAT_FEATURE(sb,
					   JBD_FEATURE_COMPAT_CHECKSUM)) {
				struct jbd_commit_header *commit =
					(struct jbd_commit_header *)header;
				if (commit->chksum_type != JBD_CRC32_CHKSUM ||
				    commit->chksum_size != JBD_CRC32_CHKSUM_SIZE ||
				    jbd_get32(commit, chksum[0]) != legacy_checksum) {
					r = EIO;
					log_end = true;
					break;
				}
				legacy_checksum = EXT4_CRC32_INIT;
			}
			ext4_dbg(DEBUG_JBD, "Commit block: %" PRIu32", "
					    "trans_id: %" PRIu32"\n",
					    this_block, this_trans_id);
			/*
			 * This is the end of a transaction,
			 * we may now proceed to the next transaction.
			 */
			this_trans_id++;
			if (action == ACTION_SCAN)
				info->trans_cnt++;
			break;
		case JBD_REVOKE_BLOCK:
			if (!jbd_verify_meta_csum(jbd_fs, header)) {
				ext4_dbg(DEBUG_JBD,
					DBG_WARN "Revoke block checksum failed."
						"Journal block: %" PRIu32"\n",
						this_block);
				r = EIO;
				log_end = true;
				break;
			}
			if (action == ACTION_SCAN) {
				r = jbd_validate_revoke_block(jbd_fs, header);
				if (r != EOK) {
					log_end = true;
					break;
				}
			}
			ext4_dbg(DEBUG_JBD, "Revoke block: %" PRIu32", "
					    "trans_id: %" PRIu32"\n",
					    this_block, this_trans_id);
			if (action == ACTION_REVOKE) {
				info->this_trans_id = this_trans_id;
				r = jbd_build_revoke_tree(jbd_fs, header, info);
				if (r != EOK)
					log_end = true;
			}
			break;
		default:
			log_end = true;
			break;
		}
		jbd_block_set(jbd_fs, &block);
		this_block++;
		wrap(sb, this_block);
		if (this_block == start_block)
			log_end = true;

	}
	ext4_dbg(DEBUG_JBD, "End of journal.\n");
	if (r == EOK && action == ACTION_SCAN) {
		/* We have finished scanning the journal. */
		info->start_trans_id = start_trans_id;
		if (trans_id_diff(this_trans_id, start_trans_id) > 0)
			info->last_trans_id = this_trans_id - 1;
		else
			info->last_trans_id = this_trans_id;
	}

	return r;
}

/**@brief  Replay journal.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_recover(struct jbd_fs *jbd_fs)
{
	int r = EOK;
	struct recover_info info;
	struct jbd_sb *sb = &jbd_fs->sb;
	struct jbd_fc_scan_result fc_scan;
	uint32_t expected_fc_tid;
	uint32_t features_incompatible;
	bool replayed_full = sb->start != 0;
	bool recovery_needed;

	RB_INIT(&info.revoke_root);
	features_incompatible = ext4_get32(&jbd_fs->inode_ref.fs->sb,
					   features_incompatible);
	recovery_needed = (features_incompatible & EXT4_FINCOM_RECOVER) != 0;
	if (replayed_full) {
		r = jbd_iterate_log(jbd_fs, &info, ACTION_SCAN);
		if (r != EOK)
			goto out;
	} else {
		memset(&info, 0, sizeof(info));
		RB_INIT(&info.revoke_root);
		info.last_trans_id = jbd_get32(sb, sequence);
	}
	expected_fc_tid = replayed_full ? info.last_trans_id + 1 :
					   info.last_trans_id;
	r = jbd_fc_scan(jbd_fs, expected_fc_tid, &fc_scan);
	if (r != EOK)
		goto out;
	if (replayed_full) {
		r = jbd_iterate_log(jbd_fs, &info, ACTION_REVOKE);
		if (r != EOK)
			goto out;
		r = jbd_iterate_log(jbd_fs, &info, ACTION_RECOVER);
		if (r != EOK)
			goto out;
	}
	if (fc_scan.committed_tags) {
		r = jbd_fc_replay(jbd_fs, &fc_scan);
		if (r != EOK)
			goto out;
	}
	if (replayed_full || fc_scan.committed_tags || recovery_needed) {
		features_incompatible = ext4_get32(
			&jbd_fs->inode_ref.fs->sb, features_incompatible);
		jbd_set32(sb, start, 0);
		jbd_set32(sb, sequence, fc_scan.committed_tags ?
			  expected_fc_tid : info.last_trans_id);
		features_incompatible &= ~EXT4_FINCOM_RECOVER;
		ext4_set32(&jbd_fs->inode_ref.fs->sb, features_incompatible,
			   features_incompatible);
		jbd_fs->dirty = true;
		r = ext4_sb_write(jbd_fs->inode_ref.fs->bdev,
				  &jbd_fs->inode_ref.fs->sb);
	}
	out:
	jbd_destroy_revoke_tree(&info);
	return r;
}

static void jbd_journal_write_sb(struct jbd_journal *journal)
{
	struct jbd_fs *jbd_fs = journal->jbd_fs;
	jbd_set32(&jbd_fs->sb, start, journal->start);
	jbd_set32(&jbd_fs->sb, sequence, journal->trans_id);
	jbd_fs->dirty = true;
}

/**@brief  Start accessing the journal.
 * @param  jbd_fs jbd filesystem
 * @param  journal current journal session
 * @return standard error code*/
int jbd_journal_start(struct jbd_fs *jbd_fs,
		      struct jbd_journal *journal)
{
	int r;
	uint32_t features_incompatible =
			ext4_get32(&jbd_fs->inode_ref.fs->sb,
				   features_incompatible);
	features_incompatible |= EXT4_FINCOM_RECOVER;
	ext4_set32(&jbd_fs->inode_ref.fs->sb,
			features_incompatible,
			features_incompatible);
	r = ext4_sb_write(jbd_fs->inode_ref.fs->bdev,
			&jbd_fs->inode_ref.fs->sb);
	if (r != EOK)
		return r;

	journal->first = jbd_get32(&jbd_fs->sb, first);
	journal->start = journal->first;
	journal->last = journal->first;
	/*
	 * To invalidate any stale records we need to start from
	 * the checkpoint transaction ID of the previous journalling session
	 * plus 1.
	 */
	journal->trans_id = jbd_get32(&jbd_fs->sb, sequence) + 1;
	journal->alloc_trans_id = journal->trans_id;

	journal->block_size = jbd_get32(&jbd_fs->sb, blocksize);

	TAILQ_INIT(&journal->cp_queue);
	RB_INIT(&journal->block_rec_root);
	journal->jbd_fs = jbd_fs;
	jbd_journal_write_sb(journal);
	r = jbd_write_sb(jbd_fs);
	if (r != EOK)
		return r;

	jbd_fs->bdev->journal = journal;
	return EOK;
}

static void jbd_trans_end_write(struct ext4_bcache *bc __unused,
			  struct ext4_buf *buf __unused,
			  int res,
			  void *arg);

/*
 * This routine is only suitable to committed transactions. */
static void jbd_journal_flush_trans(struct jbd_trans *trans)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct jbd_journal *journal = trans->journal;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	void *tmp_data = ext4_malloc(journal->block_size);
	ext4_assert(tmp_data);

	TAILQ_FOREACH_SAFE(jbd_buf, &trans->buf_queue, buf_node,
			tmp) {
		struct ext4_buf *buf;
		struct ext4_block block;
		/* The buffer is not yet flushed. */
		buf = ext4_bcache_find_get(fs->bdev->bc, &block,
					   jbd_buf->block_rec->lba);
		if (!(buf && ext4_bcache_test_flag(buf, BC_UPTODATE) &&
		      jbd_buf->block_rec->trans == trans)) {
			int r;
			struct ext4_block jbd_block = EXT4_BLOCK_ZERO();
			r = jbd_block_get(journal->jbd_fs,
						&jbd_block,
						jbd_buf->jbd_lba);
			ext4_assert(r == EOK);
			memcpy(tmp_data, jbd_block.data,
					journal->block_size);
			ext4_block_set(fs->bdev, &jbd_block);
			r = ext4_blocks_set_direct(fs->bdev, tmp_data,
					jbd_buf->block_rec->lba, 1);
			jbd_trans_end_write(fs->bdev->bc, buf, r, jbd_buf);
		} else
			ext4_block_flush_buf(fs->bdev, buf);

		if (buf)
			ext4_block_set(fs->bdev, &block);
	}

	ext4_free(tmp_data);
}

static void
jbd_journal_skip_pure_revoke(struct jbd_journal *journal,
			     struct jbd_trans *trans)
{
	journal->start = trans->start_iblock +
		trans->alloc_blocks;
	wrap(&journal->jbd_fs->sb, journal->start);
	journal->trans_id = trans->trans_id + 1;
	jbd_journal_free_trans(journal,
			trans, false);
	jbd_journal_write_sb(journal);
}

void
jbd_journal_purge_cp_trans(struct jbd_journal *journal,
			   bool flush,
			   bool once)
{
	struct jbd_trans *trans;
	while ((trans = TAILQ_FIRST(&journal->cp_queue))) {
		if (!trans->data_cnt) {
			TAILQ_REMOVE(&journal->cp_queue,
					trans,
					trans_node);
			jbd_journal_skip_pure_revoke(journal, trans);
		} else {
			if (trans->data_cnt ==
					trans->written_cnt) {
				journal->start =
					trans->start_iblock +
					trans->alloc_blocks;
				wrap(&journal->jbd_fs->sb,
						journal->start);
				journal->trans_id =
					trans->trans_id + 1;
				TAILQ_REMOVE(&journal->cp_queue,
						trans,
						trans_node);
				jbd_journal_free_trans(journal,
						trans,
						false);
				jbd_journal_write_sb(journal);
			} else if (!flush) {
				journal->start =
					trans->start_iblock;
				wrap(&journal->jbd_fs->sb,
						journal->start);
				journal->trans_id =
					trans->trans_id;
				jbd_journal_write_sb(journal);
				break;
			} else
				jbd_journal_flush_trans(trans);
		}
		if (once)
			break;
	}
}

/**@brief  Stop accessing the journal.
 * @param  journal current journal session
 * @return standard error code*/
int jbd_journal_stop(struct jbd_journal *journal)
{
	int r;
	struct jbd_fs *jbd_fs = journal->jbd_fs;
	uint32_t features_incompatible;

	/* Make sure that journalled content have reached
	 * the disk.*/
	jbd_journal_purge_cp_trans(journal, true, false);

	/* There should be no block record in this journal
	 * session. */
	if (!RB_EMPTY(&journal->block_rec_root))
		ext4_dbg(DEBUG_JBD,
			 DBG_WARN "There are still block records "
			 	  "in this journal session!\n");

	features_incompatible =
		ext4_get32(&jbd_fs->inode_ref.fs->sb,
			   features_incompatible);
	features_incompatible &= ~EXT4_FINCOM_RECOVER;
	ext4_set32(&jbd_fs->inode_ref.fs->sb,
			features_incompatible,
			features_incompatible);
	r = ext4_sb_write(jbd_fs->inode_ref.fs->bdev,
			&jbd_fs->inode_ref.fs->sb);
	if (r != EOK)
		return r;

	journal->start = 0;
	journal->trans_id = 0;
	jbd_journal_write_sb(journal);
	return jbd_write_sb(journal->jbd_fs);
}

/**@brief  Allocate a block in the journal.
 * @param  journal current journal session
 * @param  trans transaction
 * @return allocated block address*/
static uint32_t jbd_journal_alloc_block(struct jbd_journal *journal,
					struct jbd_trans *trans)
{
	uint32_t start_block;

	start_block = journal->last++;
	trans->alloc_blocks++;
	wrap(&journal->jbd_fs->sb, journal->last);
	
	/* If there is no space left, flush just one journalled
	 * transaction.*/
	if (journal->last == journal->start) {
		jbd_journal_purge_cp_trans(journal, true, true);
		ext4_assert(journal->last != journal->start);
	}

	return start_block;
}

static struct jbd_block_rec *
jbd_trans_block_rec_lookup(struct jbd_journal *journal,
			   ext4_fsblk_t lba)
{
	struct jbd_block_rec tmp = {
		.lba = lba
	};

	return RB_FIND(jbd_block,
		       &journal->block_rec_root,
		       &tmp);
}

static void
jbd_trans_change_ownership(struct jbd_block_rec *block_rec,
			   struct jbd_trans *new_trans)
{
	LIST_REMOVE(block_rec, tbrec_node);
	if (new_trans) {
		/* Now this block record belongs to this transaction. */
		LIST_INSERT_HEAD(&new_trans->tbrec_list, block_rec, tbrec_node);
	}
	block_rec->trans = new_trans;
}

static inline struct jbd_block_rec *
jbd_trans_insert_block_rec(struct jbd_trans *trans,
			   ext4_fsblk_t lba)
{
	struct jbd_block_rec *block_rec;
	block_rec = jbd_trans_block_rec_lookup(trans->journal, lba);
	if (block_rec) {
		jbd_trans_change_ownership(block_rec, trans);
		return block_rec;
	}
	block_rec = ext4_calloc(1, sizeof(struct jbd_block_rec));
	if (!block_rec)
		return NULL;

	block_rec->lba = lba;
	block_rec->trans = trans;
	TAILQ_INIT(&block_rec->dirty_buf_queue);
	LIST_INSERT_HEAD(&trans->tbrec_list, block_rec, tbrec_node);
	RB_INSERT(jbd_block, &trans->journal->block_rec_root, block_rec);
	return block_rec;
}

/*
 * This routine will do the dirty works.
 */
static void
jbd_trans_finish_callback(struct jbd_journal *journal,
			  const struct jbd_trans *trans,
			  struct jbd_block_rec *block_rec,
			  bool abort,
			  bool revoke)
{
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	if (block_rec->trans != trans)
		return;

	if (!abort) {
		struct jbd_buf *jbd_buf, *tmp;
		TAILQ_FOREACH_SAFE(jbd_buf,
				&block_rec->dirty_buf_queue,
				dirty_buf_node,
				tmp) {
			jbd_trans_end_write(fs->bdev->bc,
					NULL,
					EOK,
					jbd_buf);
		}
	} else {
		/*
		 * We have to roll back data if the block is going to be
		 * aborted.
		 */
		struct jbd_buf *jbd_buf;
		struct ext4_block jbd_block = EXT4_BLOCK_ZERO(),
				  block = EXT4_BLOCK_ZERO();
		jbd_buf = TAILQ_LAST(&block_rec->dirty_buf_queue,
				jbd_buf_dirty);
		if (jbd_buf) {
			if (!revoke) {
				int r;
				r = ext4_block_get_noread(fs->bdev,
							&block,
							block_rec->lba);
				ext4_assert(r == EOK);
				r = jbd_block_get(journal->jbd_fs,
							&jbd_block,
							jbd_buf->jbd_lba);
				ext4_assert(r == EOK);
				memcpy(block.data, jbd_block.data,
						journal->block_size);

				jbd_trans_change_ownership(block_rec,
						jbd_buf->trans);

				block.buf->end_write = jbd_trans_end_write;
				block.buf->end_write_arg = jbd_buf;

				ext4_bcache_set_flag(jbd_block.buf, BC_TMP);
				ext4_bcache_set_dirty(block.buf);

				ext4_block_set(fs->bdev, &jbd_block);
				ext4_block_set(fs->bdev, &block);
				return;
			} else {
				/* The revoked buffer is yet written. */
				jbd_trans_change_ownership(block_rec,
						jbd_buf->trans);
			}
		}
	}
}

static inline void
jbd_trans_remove_block_rec(struct jbd_journal *journal,
			   struct jbd_block_rec *block_rec,
			   struct jbd_trans *trans)
{
	/* If this block record doesn't belong to this transaction,
	 * give up.*/
	if (block_rec->trans == trans) {
		LIST_REMOVE(block_rec, tbrec_node);
		RB_REMOVE(jbd_block,
				&journal->block_rec_root,
				block_rec);
		ext4_free(block_rec);
	}
}

/**@brief  Add block to a transaction and mark it dirty.
 * @param  trans transaction
 * @param  block block descriptor
 * @return standard error code*/
int jbd_trans_set_block_dirty(struct jbd_trans *trans,
			      struct ext4_block *block)
{
	struct jbd_buf *jbd_buf;
	struct jbd_revoke_rec *rec, tmp_rec = {
		.lba = block->lb_id
	};
	struct jbd_block_rec *block_rec;

	if (block->buf->end_write == jbd_trans_end_write) {
		jbd_buf = block->buf->end_write_arg;
		if (jbd_buf && jbd_buf->trans == trans)
			return EOK;
	}
	jbd_buf = ext4_calloc(1, sizeof(struct jbd_buf));
	if (!jbd_buf)
		return ENOMEM;

	if ((block_rec = jbd_trans_insert_block_rec(trans,
					block->lb_id)) == NULL) {
		ext4_free(jbd_buf);
		return ENOMEM;
	}

	TAILQ_INSERT_TAIL(&block_rec->dirty_buf_queue,
			jbd_buf,
			dirty_buf_node);

	jbd_buf->block_rec = block_rec;
	jbd_buf->trans = trans;
	jbd_buf->block = *block;
	ext4_bcache_inc_ref(block->buf);

	/* If the content reach the disk, notify us
	 * so that we may do a checkpoint. */
	block->buf->end_write = jbd_trans_end_write;
	block->buf->end_write_arg = jbd_buf;

	trans->data_cnt++;
	TAILQ_INSERT_HEAD(&trans->buf_queue, jbd_buf, buf_node);

	ext4_bcache_set_dirty(block->buf);
	rec = RB_FIND(jbd_revoke_tree,
			&trans->revoke_root,
			&tmp_rec);
	if (rec) {
		RB_REMOVE(jbd_revoke_tree, &trans->revoke_root,
			  rec);
		ext4_free(rec);
	}

	return EOK;
}

/**@brief  Add block to be revoked to a transaction
 * @param  trans transaction
 * @param  lba logical block address
 * @return standard error code*/
int jbd_trans_revoke_block(struct jbd_trans *trans,
			   ext4_fsblk_t lba)
{
	struct jbd_revoke_rec tmp_rec = {
		.lba = lba
	}, *rec;
	rec = RB_FIND(jbd_revoke_tree,
		      &trans->revoke_root,
		      &tmp_rec);
	if (rec)
		return EOK;

	rec = ext4_calloc(1, sizeof(struct jbd_revoke_rec));
	if (!rec)
		return ENOMEM;

	rec->lba = lba;
	RB_INSERT(jbd_revoke_tree, &trans->revoke_root, rec);
	return EOK;
}

/**@brief  Try to add block to be revoked to a transaction.
 *         If @lba still remains in an transaction on checkpoint
 *         queue, add @lba as a revoked block to the transaction.
 * @param  trans transaction
 * @param  lba logical block address
 * @return standard error code*/
int jbd_trans_try_revoke_block(struct jbd_trans *trans,
			       ext4_fsblk_t lba)
{
	struct jbd_journal *journal = trans->journal;
	struct jbd_block_rec *block_rec =
		jbd_trans_block_rec_lookup(journal, lba);

	if (block_rec) {
		if (block_rec->trans == trans) {
			struct jbd_buf *jbd_buf =
				TAILQ_LAST(&block_rec->dirty_buf_queue,
					jbd_buf_dirty);
			/* If there are still unwritten buffers. */
			if (TAILQ_FIRST(&block_rec->dirty_buf_queue) !=
			    jbd_buf)
				jbd_trans_revoke_block(trans, lba);

		} else
			jbd_trans_revoke_block(trans, lba);
	}

	return EOK;
}

/**@brief  Free a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @param  abort discard all the modifications on the block?*/
void jbd_journal_free_trans(struct jbd_journal *journal,
			    struct jbd_trans *trans,
			    bool abort)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct jbd_revoke_rec *rec, *tmp2;
	struct jbd_block_rec *block_rec, *tmp3;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	TAILQ_FOREACH_SAFE(jbd_buf, &trans->buf_queue, buf_node,
			  tmp) {
		block_rec = jbd_buf->block_rec;
		if (abort) {
			jbd_buf->block.buf->end_write = NULL;
			jbd_buf->block.buf->end_write_arg = NULL;
			ext4_bcache_clear_dirty(jbd_buf->block.buf);
			ext4_block_set(fs->bdev, &jbd_buf->block);
		}

		TAILQ_REMOVE(&jbd_buf->block_rec->dirty_buf_queue,
			jbd_buf,
			dirty_buf_node);
		jbd_trans_finish_callback(journal,
				trans,
				block_rec,
				abort,
				false);
		TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
		ext4_free(jbd_buf);
	}
	RB_FOREACH_SAFE(rec, jbd_revoke_tree, &trans->revoke_root,
			  tmp2) {
		RB_REMOVE(jbd_revoke_tree, &trans->revoke_root, rec);
		ext4_free(rec);
	}
	LIST_FOREACH_SAFE(block_rec, &trans->tbrec_list, tbrec_node,
			  tmp3) {
		jbd_trans_remove_block_rec(journal, block_rec, trans);
	}

	ext4_free(trans);
}

struct jbd_tag_count_arg {
	uint32_t count;
};

static void jbd_count_block_tag(struct jbd_fs *jbd_fs,
				struct tag_info *tag_info, void *__arg)
{
	struct jbd_tag_count_arg *arg = __arg;
	(void)jbd_fs;
	(void)tag_info;
	arg->count++;
}

/**@brief  Write commit block for a transaction
 * @param  trans transaction
 * @return standard error code*/
static int jbd_trans_write_commit_block(struct jbd_trans *trans)
{
	int rc;
	struct ext4_block block;
	struct jbd_commit_header *header;
	uint32_t commit_iblock;
	struct jbd_journal *journal = trans->journal;
	uint32_t block_size = journal->block_size;

	if (JBD_HAS_COMPAT_FEATURE(&journal->jbd_fs->sb,
				   JBD_FEATURE_COMPAT_CHECKSUM)) {
		uint32_t checksum = EXT4_CRC32_INIT;
		uint32_t current = trans->start_iblock;
		uint32_t remaining = trans->alloc_blocks;

		while (remaining) {
			struct ext4_block log_block;
			struct jbd_bhdr *log_header;
			int count = 0;

			rc = jbd_block_get(journal->jbd_fs, &log_block, current);
			if (rc != EOK)
				return rc;
			log_header = (struct jbd_bhdr *)log_block.data;
			if (jbd_get32(log_header, magic) != JBD_MAGIC_NUMBER) {
				jbd_block_set(journal->jbd_fs, &log_block);
				return EIO;
			}
			if (jbd_get32(log_header, blocktype) ==
			    JBD_DESCRIPTOR_BLOCK) {
				struct jbd_tag_count_arg count_arg = {0};
				checksum = jbd_crc32_be(checksum, log_block.data,
							block_size);
				rc = jbd_iterate_block_table(journal->jbd_fs,
							     log_header + 1,
							     block_size -
						     sizeof(struct jbd_bhdr),
						     jbd_count_block_tag,
						     &count_arg);
				count = (int)count_arg.count;
				jbd_block_set(journal->jbd_fs, &log_block);
				if (rc != EOK || (uint32_t)count >= remaining)
					return EIO;

				current++;
				wrap(&journal->jbd_fs->sb, current);
				remaining--;
				while (count--) {
					rc = jbd_block_get(journal->jbd_fs,
							   &log_block, current);
					if (rc != EOK)
						return rc;
					checksum = jbd_crc32_be(checksum,
								log_block.data,
								block_size);
					jbd_block_set(journal->jbd_fs, &log_block);
					current++;
					wrap(&journal->jbd_fs->sb, current);
					remaining--;
				}
				continue;
			}
			if (jbd_get32(log_header, blocktype) != JBD_REVOKE_BLOCK) {
				jbd_block_set(journal->jbd_fs, &log_block);
				return EIO;
			}
			jbd_block_set(journal->jbd_fs, &log_block);
			current++;
			wrap(&journal->jbd_fs->sb, current);
			remaining--;
		}
		trans->data_csum = checksum;
	}

	commit_iblock = jbd_journal_alloc_block(journal, trans);

	rc = jbd_block_get_noread(journal->jbd_fs, &block, commit_iblock);
	if (rc != EOK)
		return rc;

	memset(block.data, 0, block_size);
	header = (struct jbd_commit_header *)block.data;
	jbd_set32(&header->header, magic, JBD_MAGIC_NUMBER);
	jbd_set32(&header->header, blocktype, JBD_COMMIT_BLOCK);
	jbd_set32(&header->header, sequence, trans->trans_id);

	if (JBD_HAS_COMPAT_FEATURE(&journal->jbd_fs->sb,
				   JBD_FEATURE_COMPAT_CHECKSUM)) {
		header->chksum_type = JBD_CRC32_CHKSUM;
		header->chksum_size = JBD_CRC32_CHKSUM_SIZE;
		jbd_set32(header, chksum[0], trans->data_csum);
	}
	jbd_commit_csum_set(journal->jbd_fs, header);
	ext4_bcache_set_dirty(block.buf);
	ext4_bcache_set_flag(block.buf, BC_TMP);
	rc = jbd_block_set(journal->jbd_fs, &block);
	return rc;
}

/**@brief  Write descriptor block for a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
static int jbd_journal_prepare(struct jbd_journal *journal,
			       struct jbd_trans *trans)
{
	int rc = EOK, i = 0;
	struct ext4_block desc_block = EXT4_BLOCK_ZERO(),
			  data_block = EXT4_BLOCK_ZERO();
	int32_t tag_tbl_size = 0;
	uint32_t desc_iblock = 0;
	uint32_t data_iblock = 0;
	char *tag_start = NULL, *tag_ptr = NULL;
	struct jbd_buf *jbd_buf, *tmp;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	uint32_t checksum = EXT4_CRC32_INIT;
	struct jbd_bhdr *bhdr = NULL;
	void *data;

	/* Try to remove any non-dirty buffers from the tail of
	 * buf_queue. */
	TAILQ_FOREACH_REVERSE_SAFE(jbd_buf, &trans->buf_queue,
			jbd_trans_buf, buf_node, tmp) {
		struct jbd_revoke_rec tmp_rec = {
			.lba = jbd_buf->block_rec->lba
		};
		/* We stop the iteration when we find a dirty buffer. */
		if (ext4_bcache_test_flag(jbd_buf->block.buf,
					BC_DIRTY))
			break;
	
		TAILQ_REMOVE(&jbd_buf->block_rec->dirty_buf_queue,
			jbd_buf,
			dirty_buf_node);

		jbd_buf->block.buf->end_write = NULL;
		jbd_buf->block.buf->end_write_arg = NULL;
		jbd_trans_finish_callback(journal,
				trans,
				jbd_buf->block_rec,
				true,
				RB_FIND(jbd_revoke_tree,
					&trans->revoke_root,
					&tmp_rec));
		jbd_trans_remove_block_rec(journal,
					jbd_buf->block_rec, trans);
		trans->data_cnt--;

		ext4_block_set(fs->bdev, &jbd_buf->block);
		TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
		ext4_free(jbd_buf);
	}

	TAILQ_FOREACH_SAFE(jbd_buf, &trans->buf_queue, buf_node, tmp) {
		struct tag_info tag_info;
		bool uuid_exist = false;
		bool is_escape = false;
		struct jbd_revoke_rec tmp_rec = {
			.lba = jbd_buf->block_rec->lba
		};
		if (!ext4_bcache_test_flag(jbd_buf->block.buf,
					   BC_DIRTY)) {
			TAILQ_REMOVE(&jbd_buf->block_rec->dirty_buf_queue,
					jbd_buf,
					dirty_buf_node);

			jbd_buf->block.buf->end_write = NULL;
			jbd_buf->block.buf->end_write_arg = NULL;

			/* The buffer has not been modified, just release
			 * that jbd_buf. */
			jbd_trans_finish_callback(journal,
					trans,
					jbd_buf->block_rec,
					true,
					RB_FIND(jbd_revoke_tree,
						&trans->revoke_root,
						&tmp_rec));
			jbd_trans_remove_block_rec(journal,
					jbd_buf->block_rec, trans);
			trans->data_cnt--;

			ext4_block_set(fs->bdev, &jbd_buf->block);
			TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
			ext4_free(jbd_buf);
			continue;
		}
		if (((struct jbd_bhdr *)jbd_buf->block.data)->magic ==
				to_be32(JBD_MAGIC_NUMBER))
			is_escape = true;

again:
		if (!desc_iblock) {
			desc_iblock = jbd_journal_alloc_block(journal, trans);
			rc = jbd_block_get_noread(journal->jbd_fs, &desc_block, desc_iblock);
			if (rc != EOK)
				break;

			memset(desc_block.data, 0, journal->block_size);
			bhdr = (struct jbd_bhdr *)desc_block.data;
			jbd_set32(bhdr, magic, JBD_MAGIC_NUMBER);
			jbd_set32(bhdr, blocktype, JBD_DESCRIPTOR_BLOCK);
			jbd_set32(bhdr, sequence, trans->trans_id);

			tag_start = (char *)(bhdr + 1);
			tag_ptr = tag_start;
			uuid_exist = true;
			tag_tbl_size = journal->block_size -
				sizeof(struct jbd_bhdr);

		if (jbd_has_csum_v2or3(&journal->jbd_fs->sb))
				tag_tbl_size -= sizeof(struct jbd_block_tail);

			if (!trans->start_iblock)
				trans->start_iblock = desc_iblock;

			ext4_bcache_set_dirty(desc_block.buf);
			ext4_bcache_set_flag(desc_block.buf, BC_TMP);
		}
		tag_info.block = jbd_buf->block.lb_id;
		tag_info.uuid_exist = uuid_exist;
		tag_info.is_escape = is_escape;
		if (i == trans->data_cnt - 1)
			tag_info.last_tag = true;
		else
			tag_info.last_tag = false;

		tag_info.checksum = 0;

		if (uuid_exist)
			memcpy(tag_info.uuid, journal->jbd_fs->sb.uuid,
					UUID_SIZE);

		if (tag_tbl_size < jbd_tag_bytes(journal->jbd_fs) +
				   (uuid_exist ? UUID_SIZE : 0)) {
			jbd_meta_csum_set(journal->jbd_fs, bhdr);
			desc_iblock = 0;
			rc = jbd_block_set(journal->jbd_fs, &desc_block);
			if (rc != EOK)
				break;

			goto again;
		}

		data_iblock = jbd_journal_alloc_block(journal, trans);
		rc = jbd_block_get_noread(journal->jbd_fs, &data_block, data_iblock);
		if (rc != EOK) {
			desc_iblock = 0;
			ext4_bcache_clear_dirty(desc_block.buf);
			jbd_block_set(journal->jbd_fs, &desc_block);
			break;
		}

		data = data_block.data;
		memcpy(data, jbd_buf->block.data,
			journal->block_size);
		if (is_escape)
			((struct jbd_bhdr *)data)->magic = 0;
		checksum = jbd_block_csum(journal->jbd_fs, data,
					  EXT4_CRC32_INIT,
					  trans->trans_id);
		tag_info.checksum = checksum;
		rc = jbd_write_block_tag(journal->jbd_fs, tag_ptr,
				 tag_tbl_size, &tag_info);
		if (rc != EOK) {
			ext4_bcache_clear_dirty(data_block.buf);
			jbd_block_set(journal->jbd_fs, &data_block);
			desc_iblock = 0;
			ext4_bcache_clear_dirty(desc_block.buf);
			jbd_block_set(journal->jbd_fs, &desc_block);
			break;
		}

		ext4_bcache_set_dirty(data_block.buf);
		ext4_bcache_set_flag(data_block.buf, BC_TMP);
		rc = jbd_block_set(journal->jbd_fs, &data_block);
		if (rc != EOK) {
			desc_iblock = 0;
			ext4_bcache_clear_dirty(desc_block.buf);
			jbd_block_set(journal->jbd_fs, &desc_block);
			break;
		}
		jbd_buf->jbd_lba = data_iblock;

		tag_ptr += tag_info.tag_bytes;
		tag_tbl_size -= tag_info.tag_bytes;

		i++;
	}
	if (rc == EOK && desc_iblock) {
		jbd_meta_csum_set(journal->jbd_fs,
				(struct jbd_bhdr *)bhdr);
		trans->data_csum = checksum;
		rc = jbd_block_set(journal->jbd_fs, &desc_block);
	}

	return rc;
}

/**@brief  Write revoke block for a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
static int
jbd_journal_prepare_revoke(struct jbd_journal *journal,
			   struct jbd_trans *trans)
{
	int rc = EOK, i = 0;
	struct ext4_block desc_block = EXT4_BLOCK_ZERO();
	int32_t tag_tbl_size = 0;
	uint32_t desc_iblock = 0;
	char *blocks_entry = NULL;
	struct jbd_revoke_rec *rec, *tmp;
	struct jbd_revoke_header *header = NULL;
	int32_t record_len = 4;
	struct jbd_bhdr *bhdr = NULL;

	if (JBD_HAS_INCOMPAT_FEATURE(&journal->jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		record_len = 8;

	RB_FOREACH_SAFE(rec, jbd_revoke_tree, &trans->revoke_root,
			  tmp) {
again:
		if (!desc_iblock) {
			desc_iblock = jbd_journal_alloc_block(journal, trans);
			rc = jbd_block_get_noread(journal->jbd_fs, &desc_block,
						  desc_iblock);
			if (rc != EOK)
				break;

			memset(desc_block.data, 0, journal->block_size);
			bhdr = (struct jbd_bhdr *)desc_block.data;
			jbd_set32(bhdr, magic, JBD_MAGIC_NUMBER);
			jbd_set32(bhdr, blocktype, JBD_REVOKE_BLOCK);
			jbd_set32(bhdr, sequence, trans->trans_id);
			
			header = (struct jbd_revoke_header *)bhdr;
			blocks_entry = (char *)(header + 1);
			tag_tbl_size = journal->block_size -
				sizeof(struct jbd_revoke_header);

		if (jbd_has_csum_v2or3(&journal->jbd_fs->sb))
				tag_tbl_size -= sizeof(struct jbd_block_tail);

			if (!trans->start_iblock)
				trans->start_iblock = desc_iblock;

			ext4_bcache_set_dirty(desc_block.buf);
			ext4_bcache_set_flag(desc_block.buf, BC_TMP);
		}

		if (tag_tbl_size < record_len) {
			jbd_set32(header, count,
				  journal->block_size - tag_tbl_size);
			jbd_meta_csum_set(journal->jbd_fs, bhdr);
			bhdr = NULL;
			desc_iblock = 0;
			header = NULL;
			rc = jbd_block_set(journal->jbd_fs, &desc_block);
			if (rc != EOK)
				break;

			goto again;
		}
		if (record_len == 8) {
			uint64_t *blocks =
				(uint64_t *)blocks_entry;
			*blocks = to_be64(rec->lba);
		} else {
			uint32_t *blocks =
				(uint32_t *)blocks_entry;
			*blocks = to_be32((uint32_t)rec->lba);
		}
		blocks_entry += record_len;
		tag_tbl_size -= record_len;

		i++;
	}
	if (rc == EOK && desc_iblock) {
		if (header != NULL)
			jbd_set32(header, count,
				  journal->block_size - tag_tbl_size);

		jbd_meta_csum_set(journal->jbd_fs, bhdr);
		rc = jbd_block_set(journal->jbd_fs, &desc_block);
	}

	return rc;
}

/**@brief  Put references of block descriptors in a transaction.
 * @param  journal current journal session
 * @param  trans transaction*/
void jbd_journal_cp_trans(struct jbd_journal *journal, struct jbd_trans *trans)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	TAILQ_FOREACH_SAFE(jbd_buf, &trans->buf_queue, buf_node,
			tmp) {
		struct ext4_block block = jbd_buf->block;
		ext4_block_set(fs->bdev, &block);
	}
}

/**@brief  Update the start block of the journal when
 *         all the contents in a transaction reach the disk.*/
static void jbd_trans_end_write(struct ext4_bcache *bc __unused,
			  struct ext4_buf *buf,
			  int res,
			  void *arg)
{
	struct jbd_buf *jbd_buf = arg;
	struct jbd_trans *trans = jbd_buf->trans;
	struct jbd_block_rec *block_rec = jbd_buf->block_rec;
	struct jbd_journal *journal = trans->journal;
	bool first_in_queue =
		trans == TAILQ_FIRST(&journal->cp_queue);
	if (res != EOK)
		trans->error = res;

	TAILQ_REMOVE(&trans->buf_queue, jbd_buf, buf_node);
	TAILQ_REMOVE(&block_rec->dirty_buf_queue,
			jbd_buf,
			dirty_buf_node);

	jbd_trans_finish_callback(journal,
			trans,
			jbd_buf->block_rec,
			false,
			false);
	if (block_rec->trans == trans && buf) {
		/* Clear the end_write and end_write_arg fields. */
		buf->end_write = NULL;
		buf->end_write_arg = NULL;
	}

	ext4_free(jbd_buf);

	trans->written_cnt++;
	if (trans->written_cnt == trans->data_cnt) {
		/* If it is the first transaction on checkpoint queue,
		 * we will shift the start of the journal to the next
		 * transaction, and remove subsequent written
		 * transactions from checkpoint queue until we find
		 * an unwritten one. */
		if (first_in_queue) {
			journal->start = trans->start_iblock +
				trans->alloc_blocks;
			wrap(&journal->jbd_fs->sb, journal->start);
			journal->trans_id = trans->trans_id + 1;
			TAILQ_REMOVE(&journal->cp_queue, trans, trans_node);
			jbd_journal_free_trans(journal, trans, false);

			jbd_journal_purge_cp_trans(journal, false, false);
			jbd_journal_write_sb(journal);
			jbd_write_sb(journal->jbd_fs);
		}
	}
}

/**@brief  Commit a transaction to the journal immediately.
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
static int __jbd_journal_commit_trans(struct jbd_journal *journal,
				      struct jbd_trans *trans)
{
	int rc = EOK;
	uint32_t last = journal->last;
	struct jbd_revoke_rec *rec, *tmp;

	trans->trans_id = journal->alloc_trans_id;
	rc = jbd_journal_prepare(journal, trans);
	if (rc != EOK)
		goto Finish;

	rc = jbd_journal_prepare_revoke(journal, trans);
	if (rc != EOK)
		goto Finish;

	if (TAILQ_EMPTY(&trans->buf_queue) &&
	    RB_EMPTY(&trans->revoke_root)) {
		/* Since there are no entries in both buffer list
		 * and revoke entry list, we do not consider trans as
		 * complete transaction and just return EOK.*/
		jbd_journal_free_trans(journal, trans, false);
		goto Finish;
	}

	rc = jbd_trans_write_commit_block(trans);
	if (rc != EOK)
		goto Finish;

	journal->alloc_trans_id++;

	/* Complete the checkpoint of buffers which are revoked. */
	RB_FOREACH_SAFE(rec, jbd_revoke_tree, &trans->revoke_root,
			tmp) {
		struct jbd_block_rec *block_rec =
			jbd_trans_block_rec_lookup(journal, rec->lba);
		struct jbd_buf *jbd_buf = NULL;
		if (block_rec)
			jbd_buf = TAILQ_LAST(&block_rec->dirty_buf_queue,
					jbd_buf_dirty);
		if (jbd_buf) {
			struct ext4_buf *buf;
			struct ext4_block block = EXT4_BLOCK_ZERO();
			/*
			 * We do this to reset the ext4_buf::end_write and
			 * ext4_buf::end_write_arg fields so that the checkpoint
			 * callback won't be triggered again.
			 */
			buf = ext4_bcache_find_get(journal->jbd_fs->bdev->bc,
					&block,
					jbd_buf->block_rec->lba);
			jbd_trans_end_write(journal->jbd_fs->bdev->bc,
					buf,
					EOK,
					jbd_buf);
			if (buf)
				ext4_block_set(journal->jbd_fs->bdev, &block);
		}
	}

	if (TAILQ_EMPTY(&journal->cp_queue)) {
		/*
		 * This transaction is going to be the first object in the
		 * checkpoint queue.
		 * When the first transaction in checkpoint queue is completely
		 * written to disk, we shift the tail of the log to right.
		 */
		if (trans->data_cnt) {
			journal->start = trans->start_iblock;
			wrap(&journal->jbd_fs->sb, journal->start);
			journal->trans_id = trans->trans_id;
			jbd_journal_write_sb(journal);
			jbd_write_sb(journal->jbd_fs);
			TAILQ_INSERT_TAIL(&journal->cp_queue, trans,
					trans_node);
			jbd_journal_cp_trans(journal, trans);
		} else {
			journal->start = trans->start_iblock +
				trans->alloc_blocks;
			wrap(&journal->jbd_fs->sb, journal->start);
			journal->trans_id = trans->trans_id + 1;
			jbd_journal_write_sb(journal);
			jbd_journal_free_trans(journal, trans, false);
		}
	} else {
		/* No need to do anything to the JBD superblock. */
		TAILQ_INSERT_TAIL(&journal->cp_queue, trans,
				trans_node);
		if (trans->data_cnt)
			jbd_journal_cp_trans(journal, trans);
	}
Finish:
	if (rc != EOK && rc != ENOSPC) {
		journal->last = last;
		jbd_journal_free_trans(journal, trans, true);
	}
	return rc;
}

/**@brief  Allocate a new transaction
 * @param  journal current journal session
 * @return transaction allocated*/
struct jbd_trans *
jbd_journal_new_trans(struct jbd_journal *journal)
{
	struct jbd_trans *trans = NULL;
	trans = ext4_calloc(1, sizeof(struct jbd_trans));
	if (!trans)
		return NULL;

	/* We will assign a trans_id to this transaction,
	 * once it has been committed.*/
	trans->journal = journal;
	trans->data_csum = EXT4_CRC32_INIT;
	trans->error = EOK;
	TAILQ_INIT(&trans->buf_queue);
	return trans;
}

/**@brief  Commit a transaction to the journal immediately.
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
int jbd_journal_commit_trans(struct jbd_journal *journal,
			     struct jbd_trans *trans)
{
	int r = EOK;
	r = __jbd_journal_commit_trans(journal, trans);
	return r;
}

/**
 * @}
 */
