#include <ext4_balloc.h>
#include <ext4_blockdev.h>
#include <ext4_config.h>
#include <ext4_crc32.h>
#include <ext4_errno.h>
#include <ext4_extent.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_trans.h>

#include <stdlib.h>
#include <string.h>

#define PEARE_EXTENT_MAGIC 0xF30A
#define PEARE_EXTENT_ROOT_CAPACITY 4
#define PEARE_EXTENT_HEADER_SIZE 12
#define PEARE_EXTENT_ENTRY_SIZE 12
#define PEARE_EXTENT_MAX_BLOCKS 32768U
#define PEARE_EXTENT_PHYSICAL_MAX 0x0000FFFFFFFFFFFFULL
#define PEARE_EXTENT_MAX_DEPTH 5
#define PEARE_EXTENT_MAX_WRITE_DEPTH PEARE_EXTENT_MAX_DEPTH

struct peare_extent_path {
	uint8_t *node;
	struct ext4_block block;
	ext4_fsblk_t block_no;
	uint16_t entries;
	uint16_t capacity;
	uint16_t slot;
	bool external;
};

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static uint64_t extent_physical(const uint8_t *entry);
static uint32_t extent_checksum(struct ext4_inode_ref *ref,
				const uint8_t *node, uint32_t block_size);

static uint8_t *extent_root(struct ext4_inode_ref *ref)
{
	return (uint8_t *)ref->inode->blocks;
}

static uint64_t extent_index_block(const uint8_t *entry)
{
	return (uint64_t)read_le32(entry + 4) |
	       ((uint64_t)read_le16(entry + 8) << 32);
}

static void set_extent_index(uint8_t *entry, uint32_t logical,
			     ext4_fsblk_t block)
{
	write_le32(entry, logical);
	write_le32(entry + 4, (uint32_t)block);
	write_le16(entry + 8, (uint16_t)(block >> 32));
	write_le16(entry + 10, 0);
}

static uint16_t extent_node_capacity(struct ext4_inode_ref *ref, bool root)
{
	uint32_t block_size = ext4_sb_get_block_size(&ref->fs->sb);
	if (root)
		return PEARE_EXTENT_ROOT_CAPACITY;
	return (uint16_t)((block_size - PEARE_EXTENT_HEADER_SIZE) /
			  PEARE_EXTENT_ENTRY_SIZE);
}

static int validate_node(struct ext4_inode_ref *ref, uint8_t *node,
			 bool root, uint16_t expected_depth,
			 uint16_t *entries)
{
	uint16_t count;
	uint16_t capacity;
	uint32_t previous_key = 0;
	uint32_t previous_end = 0;
	uint16_t expected_max = extent_node_capacity(ref, root);
	uint16_t i;

	if (read_le16(node) != PEARE_EXTENT_MAGIC ||
	    read_le16(node + 6) != expected_depth)
		return EIO;

	count = read_le16(node + 2);
	capacity = read_le16(node + 4);
	if (capacity != expected_max || count > capacity ||
	    expected_depth > PEARE_EXTENT_MAX_DEPTH ||
	    (expected_depth && !count))
		return EIO;
	if (!root && ext4_sb_feature_ro_com(&ref->fs->sb,
						 EXT4_FRO_COM_METADATA_CSUM) &&
		 read_le32(node + ext4_sb_get_block_size(&ref->fs->sb) -
			    sizeof(uint32_t)) !=
		 extent_checksum(ref, node, ext4_sb_get_block_size(&ref->fs->sb)))
		return EIO;

	for (i = 0; i < count; ++i) {
		const uint8_t *entry = node + PEARE_EXTENT_HEADER_SIZE +
				       i * PEARE_EXTENT_ENTRY_SIZE;
		uint32_t logical = read_le32(entry);
		if (expected_depth) {
			uint64_t child = extent_index_block(entry);
			if (child > PEARE_EXTENT_PHYSICAL_MAX ||
			    child >= ref->fs->bdev->lg_bcnt ||
			    (i && logical <= previous_key) || read_le16(entry + 10))
				return EIO;
			previous_key = logical;
		} else {
			uint16_t raw_length = read_le16(entry + 4);
			uint32_t length = raw_length > PEARE_EXTENT_MAX_BLOCKS
					  ? raw_length - PEARE_EXTENT_MAX_BLOCKS : raw_length;
			uint64_t physical = extent_physical(entry);
			if (!length || physical > PEARE_EXTENT_PHYSICAL_MAX ||
			    logical > UINT32_MAX - length ||
			    (i && logical < previous_end) ||
			    physical + length > ref->fs->bdev->lg_bcnt)
				return EIO;
			previous_end = logical + length;
		}
	}

	*entries = count;
	return EOK;
}

static uint32_t extent_length(const uint8_t *entry)
{
	uint16_t raw = read_le16(entry + 4);
	return raw > PEARE_EXTENT_MAX_BLOCKS
		? raw - PEARE_EXTENT_MAX_BLOCKS : raw;
}

static uint64_t extent_physical(const uint8_t *entry)
{
	return ((uint64_t)read_le16(entry + 6) << 32) | read_le32(entry + 8);
}

static void set_extent(uint8_t *entry, uint32_t logical, uint32_t length,
		       uint64_t physical)
{
	write_le32(entry, logical);
	write_le16(entry + 4, (uint16_t)length);
	write_le16(entry + 6, (uint16_t)(physical >> 32));
	write_le32(entry + 8, (uint32_t)physical);
}

static uint32_t extent_checksum(struct ext4_inode_ref *ref, const uint8_t *node,
				uint32_t block_size)
{
	uint32_t checksum = ext4_sb_get_csum_seed(&ref->fs->sb);
	uint32_t inode_number = to_le32(ref->index);
	uint32_t generation = to_le32(ext4_inode_get_generation(ref->inode));

	checksum = ext4_crc32c(checksum, &inode_number, sizeof(inode_number));
	checksum = ext4_crc32c(checksum, &generation, sizeof(generation));
	return ext4_crc32c(checksum, node, block_size - sizeof(uint32_t));
}

static void update_extent_checksum(struct ext4_inode_ref *ref, uint8_t *node,
				   uint32_t block_size)
{
	if (ext4_sb_feature_ro_com(&ref->fs->sb, EXT4_FRO_COM_METADATA_CSUM))
		write_le32(node + block_size - sizeof(uint32_t),
			   extent_checksum(ref, node, block_size));
}

static uint32_t node_first_logical(const uint8_t *node)
{
	return read_le32(node + PEARE_EXTENT_HEADER_SIZE);
}

static uint16_t node_entry_count(const uint8_t *node)
{
	return read_le16(node + 2);
}

static uint16_t node_child_position(const uint8_t *node, uint16_t entries,
				   uint32_t logical)
{
	uint16_t position = 0;
	uint16_t i;
	for (i = 1; i < entries; ++i) {
		if (read_le32(node + PEARE_EXTENT_HEADER_SIZE +
			      i * PEARE_EXTENT_ENTRY_SIZE) > logical)
			break;
		position = i;
	}
	return position;
}

static int load_extent_path(struct ext4_inode_ref *ref, uint32_t logical,
			    bool write, struct peare_extent_path *path,
			    uint16_t *depth)
{
	uint8_t *root = extent_root(ref);
	uint16_t level;
	int rc;

	memset(path, 0, sizeof(*path) * (PEARE_EXTENT_MAX_DEPTH + 1));
	*depth = read_le16(root + 6);
	if (*depth > PEARE_EXTENT_MAX_DEPTH)
		return ENOTSUP;
	if (write && *depth > PEARE_EXTENT_MAX_WRITE_DEPTH)
		return ENOTSUP;
	path[0].node = root;
	path[0].capacity = PEARE_EXTENT_ROOT_CAPACITY;
	rc = validate_node(ref, root, true, *depth, &path[0].entries);
	if (rc != EOK)
		return rc;

	for (level = 0; level < *depth; ++level) {
		struct peare_extent_path *parent = &path[level];
		struct peare_extent_path *child = &path[level + 1];
		const uint8_t *index;
		ext4_fsblk_t block_no;
		uint16_t child_entries;

		if (!parent->entries)
			return EIO;
		parent->slot = node_child_position(parent->node, parent->entries,
						   logical);
		index = parent->node + PEARE_EXTENT_HEADER_SIZE +
			parent->slot * PEARE_EXTENT_ENTRY_SIZE;
		block_no = extent_index_block(index);
		if (block_no >= ref->fs->bdev->lg_bcnt)
			return EIO;
		if (write)
			rc = ext4_trans_block_get(ref->fs->bdev, &child->block,
						  block_no);
		else
			rc = ext4_block_get(ref->fs->bdev, &child->block, block_no);
		if (rc != EOK)
			return rc;
		child->external = true;
		child->block_no = block_no;
		child->node = child->block.data;
		child->capacity = extent_node_capacity(ref, false);
		rc = validate_node(ref, child->node, false, *depth - level - 1,
				   &child_entries);
		if (rc != EOK)
			return rc;
		child->entries = child_entries;
		if (child_entries && read_le32(index) !=
				     node_first_logical(child->node))
			return EIO;
	}
	return EOK;
}

static int close_extent_path(struct ext4_fs *fs,
			     struct peare_extent_path *path, uint16_t depth)
{
	int result = EOK;
	while (depth) {
		struct peare_extent_path *item = &path[depth--];
		if (item->external) {
			int rc = ext4_block_set(fs->bdev, &item->block);
			if (result == EOK && rc != EOK)
				result = rc;
			item->external = false;
		}
	}
	return result;
}

static int mark_extent_node(struct ext4_inode_ref *ref,
			    struct peare_extent_path *item)
{
	int rc;
	if (!item->external) {
		ref->dirty = true;
		return EOK;
	}
	update_extent_checksum(ref, item->node,
			       ext4_sb_get_block_size(&ref->fs->sb));
	rc = ext4_trans_set_block_dirty(item->block.buf);
	return rc;
}

static int allocate_extent_node(struct ext4_inode_ref *ref, uint16_t depth,
				const uint8_t *entries, uint16_t count,
				ext4_fsblk_t *block_no)
{
	struct ext4_block block;
	uint32_t block_size = ext4_sb_get_block_size(&ref->fs->sb);
	uint16_t capacity = extent_node_capacity(ref, false);
	ext4_fsblk_t goal;
	int rc;

	if (depth > PEARE_EXTENT_MAX_DEPTH || count > capacity)
		return EINVAL;
	rc = ext4_fs_indirect_find_goal(ref, &goal);
	if (rc != EOK)
		return rc;
	rc = ext4_balloc_alloc_block(ref, goal, block_no);
	if (rc != EOK)
		return rc;
	rc = ext4_trans_block_get_noread(ref->fs->bdev, &block, *block_no);
	if (rc != EOK) {
		ext4_balloc_free_block(ref, *block_no);
		return rc;
	}
	memset(block.data, 0, block_size);
	write_le16(block.data, PEARE_EXTENT_MAGIC);
	write_le16(block.data + 2, count);
	write_le16(block.data + 4, capacity);
	write_le16(block.data + 6, depth);
	if (count)
		memcpy(block.data + PEARE_EXTENT_HEADER_SIZE, entries,
		       count * PEARE_EXTENT_ENTRY_SIZE);
	update_extent_checksum(ref, block.data, block_size);
	rc = ext4_trans_set_block_dirty(block.buf);
	if (rc == EOK)
		rc = ext4_block_set(ref->fs->bdev, &block);
	else
		ext4_block_set(ref->fs->bdev, &block);
	if (rc != EOK)
		ext4_balloc_free_block(ref, *block_no);
	return rc;
}

static void replace_node_entries(uint8_t *node, uint16_t depth,
				 const uint8_t *entries, uint16_t count)
{
	uint16_t capacity = read_le16(node + 4);
	memset(node + PEARE_EXTENT_HEADER_SIZE, 0,
	       capacity * PEARE_EXTENT_ENTRY_SIZE);
	if (count)
		memcpy(node + PEARE_EXTENT_HEADER_SIZE, entries,
		       count * PEARE_EXTENT_ENTRY_SIZE);
	write_le16(node + 2, count);
	write_le16(node + 6, depth);
}

static int grow_extent_root(struct ext4_inode_ref *ref, uint16_t old_depth,
			    const uint8_t *entries, uint16_t count)
{
	uint8_t *root = extent_root(ref);
	uint16_t left_count = count / 2;
	uint16_t right_count = count - left_count;
	uint32_t left_key;
	uint32_t right_key;
	ext4_fsblk_t left_block;
	ext4_fsblk_t right_block;
	int rc;

	if (old_depth >= PEARE_EXTENT_MAX_WRITE_DEPTH ||
	    !left_count || !right_count)
		return ENOSPC;
	left_key = read_le32(entries);
	right_key = read_le32(entries + left_count * PEARE_EXTENT_ENTRY_SIZE);
	rc = allocate_extent_node(ref, old_depth, entries, left_count, &left_block);
	if (rc != EOK)
		return rc;
	rc = allocate_extent_node(ref, old_depth,
				  entries + left_count * PEARE_EXTENT_ENTRY_SIZE,
				  right_count, &right_block);
	if (rc != EOK) {
		ext4_balloc_free_block(ref, left_block);
		return rc;
	}
	memset(root + PEARE_EXTENT_HEADER_SIZE, 0,
	       PEARE_EXTENT_ROOT_CAPACITY * PEARE_EXTENT_ENTRY_SIZE);
	write_le16(root + 2, 2);
	write_le16(root + 6, old_depth + 1);
	set_extent_index(root + PEARE_EXTENT_HEADER_SIZE, left_key, left_block);
	set_extent_index(root + PEARE_EXTENT_HEADER_SIZE + PEARE_EXTENT_ENTRY_SIZE,
			 right_key, right_block);
	ref->dirty = true;
	return EOK;
}

static int split_external_node(struct ext4_inode_ref *ref,
			       struct peare_extent_path *item,
			       uint16_t depth, const uint8_t *entries,
			       uint16_t count, uint32_t *left_key,
			       uint32_t *right_key, ext4_fsblk_t *right_block)
{
	uint16_t left_count = count / 2;
	uint16_t right_count = count - left_count;
	int rc;

	if (!left_count || !right_count)
		return EIO;
	*left_key = read_le32(entries);
	*right_key = read_le32(entries + left_count * PEARE_EXTENT_ENTRY_SIZE);
	replace_node_entries(item->node, depth, entries, left_count);
	rc = mark_extent_node(ref, item);
	if (rc != EOK)
		return rc;
	return allocate_extent_node(ref, depth,
				    entries + left_count * PEARE_EXTENT_ENTRY_SIZE,
				    right_count, right_block);
}

static int propagate_first_key(struct ext4_inode_ref *ref,
			       struct peare_extent_path *path, uint16_t level)
{
	while (level) {
		struct peare_extent_path *child = &path[level];
		struct peare_extent_path *parent = &path[level - 1];
		uint8_t *index;
		int rc;

		if (!child->entries)
			return EIO;
		index = parent->node + PEARE_EXTENT_HEADER_SIZE +
			parent->slot * PEARE_EXTENT_ENTRY_SIZE;
		write_le32(index, node_first_logical(child->node));
		rc = mark_extent_node(ref, parent);
		if (rc != EOK || parent->slot)
			return rc;
		--level;
	}
	return EOK;
}

static int insert_split_child(struct ext4_inode_ref *ref,
			      struct peare_extent_path *path,
			      uint16_t depth, uint16_t parent_level,
			      uint16_t child_slot, uint32_t left_key,
			      uint32_t right_key, ext4_fsblk_t right_block)
{
	while (true) {
		struct peare_extent_path *parent = &path[parent_level];
		uint16_t node_depth = depth - parent_level;
		uint16_t count = parent->entries;
		uint16_t capacity = parent->capacity;
		uint8_t *items;
		uint16_t i;
		int rc;

		items = ext4_calloc((size_t)count + 1, PEARE_EXTENT_ENTRY_SIZE);
		if (!items)
			return ENOMEM;
		for (i = 0; i < count; ++i) {
			const uint8_t *source = parent->node + PEARE_EXTENT_HEADER_SIZE +
						i * PEARE_EXTENT_ENTRY_SIZE;
			if (i < child_slot)
				memcpy(items + i * PEARE_EXTENT_ENTRY_SIZE, source,
				       PEARE_EXTENT_ENTRY_SIZE);
			else if (i == child_slot)
				set_extent_index(items + i * PEARE_EXTENT_ENTRY_SIZE,
						left_key, path[parent_level + 1].block_no);
			else
				memcpy(items + (i + 1) * PEARE_EXTENT_ENTRY_SIZE, source,
				       PEARE_EXTENT_ENTRY_SIZE);
		}
		set_extent_index(items + (child_slot + 1) * PEARE_EXTENT_ENTRY_SIZE,
				right_key, right_block);

		if (count < capacity) {
			replace_node_entries(parent->node, node_depth, items, count + 1);
			parent->entries = count + 1;
			rc = mark_extent_node(ref, parent);
			ext4_free(items);
			if (rc != EOK)
				return rc;
			return propagate_first_key(ref, path, parent_level);
		}

		if (!parent_level) {
			if (node_depth >= PEARE_EXTENT_MAX_DEPTH) {
				ext4_free(items);
				return ENOSPC;
			}
			rc = grow_extent_root(ref, node_depth, items, count + 1);
			ext4_free(items);
			return rc;
		}

		{
			uint32_t new_left_key;
			uint32_t new_right_key;
			ext4_fsblk_t new_right_block;
			uint16_t next_slot = path[parent_level - 1].slot;
			rc = split_external_node(ref, parent, node_depth, items,
						 count + 1, &new_left_key,
						 &new_right_key, &new_right_block);
			ext4_free(items);
			if (rc != EOK)
				return rc;
			parent->entries = (count + 1) / 2;
			--parent_level;
			child_slot = next_slot;
			left_key = new_left_key;
			right_key = new_right_key;
			right_block = new_right_block;
		}
	}
}

static int mark_changed_extent_path(struct ext4_inode_ref *ref,
				    struct peare_extent_path *path,
				    uint16_t depth)
{
	uint16_t level;
	int rc;
	for (level = 1; level <= depth; ++level) {
		rc = mark_extent_node(ref, &path[level]);
		if (rc != EOK)
			return rc;
	}
	ref->dirty = true;
	return EOK;
}

void ext4_extent_tree_init(struct ext4_inode_ref *inode_ref)
{
	uint8_t *root = extent_root(inode_ref);

	memset(root, 0, sizeof(inode_ref->inode->blocks));
	write_le16(root, PEARE_EXTENT_MAGIC);
	write_le16(root + 4, PEARE_EXTENT_ROOT_CAPACITY);
}

int ext4_extent_get_blocks(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
			   uint32_t max_blocks, ext4_fsblk_t *result,
			   bool create, uint32_t *blocks_count)
{
	struct peare_extent_path path[PEARE_EXTENT_MAX_DEPTH + 1];
	struct peare_extent_path *leaf;
	uint8_t *node;
	uint8_t *items;
	uint16_t entries;
	uint16_t depth;
	uint16_t capacity;
	uint16_t position;
	uint16_t i;
	uint64_t physical;
	uint32_t length;
	uint32_t cluster_bits;
	uint32_t cluster_ratio = 1;
	uint32_t cluster_logical;
	ext4_fsblk_t allocated;
	ext4_fsblk_t physical_block;
	ext4_fsblk_t goal;
	bool allocated_cluster = false;
	bool merge_previous;
	bool merge_next;
	int rc;

	if (!inode_ref || !inode_ref->inode || !inode_ref->fs || !result ||
	    !max_blocks)
		return EINVAL;
	cluster_bits = ext4_sb_cluster_ratio_bits(&inode_ref->fs->sb);
	if (cluster_bits == UINT32_MAX)
		return ENOTSUP;
	if (cluster_bits) {
		cluster_ratio = (uint32_t)1 << cluster_bits;
		if (cluster_ratio > PEARE_EXTENT_MAX_BLOCKS)
			return ENOTSUP;
	}
	*result = 0;
	if (blocks_count)
		*blocks_count = 0;

	rc = load_extent_path(inode_ref, iblock, create, path, &depth);
	if (rc != EOK) {
		close_extent_path(inode_ref->fs, path, PEARE_EXTENT_MAX_DEPTH);
		return rc;
	}
	leaf = &path[depth];
	node = leaf->node;
	entries = leaf->entries;
	capacity = leaf->capacity;

	position = entries;
	for (i = 0; i < entries; ++i) {
		uint8_t *entry = node + PEARE_EXTENT_HEADER_SIZE +
				 i * PEARE_EXTENT_ENTRY_SIZE;
		uint32_t logical = read_le32(entry);
		length = extent_length(entry);
		physical = extent_physical(entry);
		if (iblock >= logical && (uint64_t)iblock < (uint64_t)logical + length) {
			uint32_t offset = iblock - logical;
			if (read_le16(entry + 4) > PEARE_EXTENT_MAX_BLOCKS) {
				if (create) {
					close_extent_path(inode_ref->fs, path, depth);
					return ENOTSUP;
				}
				*result = 0;
			} else {
				*result = physical + offset;
			}
			if (blocks_count) {
				uint32_t available = length - offset;
				*blocks_count = available < max_blocks ? available : max_blocks;
			}
			return close_extent_path(inode_ref->fs, path, depth);
		}
		if (logical > iblock) {
			position = i;
			break;
		}
	}

	if (!create)
		return close_extent_path(inode_ref->fs, path, depth);
	cluster_logical = iblock & ~(cluster_ratio - 1);

	rc = ext4_fs_indirect_find_goal(inode_ref, &goal);
	if (rc != EOK) {
		close_extent_path(inode_ref->fs, path, depth);
		return rc;
	}
	allocated = 0;
	if (cluster_ratio > 1) {
		uint8_t *previous = position
			? node + PEARE_EXTENT_HEADER_SIZE +
			  (position - 1) * PEARE_EXTENT_ENTRY_SIZE : NULL;
		uint8_t *next = position < entries
			? node + PEARE_EXTENT_HEADER_SIZE +
			  position * PEARE_EXTENT_ENTRY_SIZE : NULL;
		bool cluster_found = false;
		if (previous) {
			uint32_t previous_logical = read_le32(previous);
			uint64_t previous_end = (uint64_t)previous_logical +
				extent_length(previous);
			if (previous_end > cluster_logical) {
				uint64_t previous_physical =
					extent_physical(previous);
				uint64_t base;
				if (previous_logical <= cluster_logical)
					base = previous_physical +
						(cluster_logical - previous_logical);
				else if (previous_physical >=
					 previous_logical - cluster_logical)
					base = previous_physical -
						(previous_logical - cluster_logical);
				else {
					close_extent_path(inode_ref->fs, path, depth);
					return EIO;
				}
				if (base & (cluster_ratio - 1)) {
					close_extent_path(inode_ref->fs, path, depth);
					return EIO;
				}
				allocated = base;
				cluster_found = true;
			}
		}
		if (next && read_le32(next) < cluster_logical + cluster_ratio) {
			uint32_t next_logical = read_le32(next);
			uint64_t next_physical = extent_physical(next);
			if (next_physical < next_logical - cluster_logical) {
				close_extent_path(inode_ref->fs, path, depth);
				return EIO;
			}
			uint64_t base = next_physical - (next_logical - cluster_logical);
			if ((base & (cluster_ratio - 1)) ||
			    (cluster_found && allocated != base)) {
				close_extent_path(inode_ref->fs, path, depth);
				return EIO;
			}
			allocated = base;
			cluster_found = true;
		}
		if (!cluster_found) {
			rc = ext4_balloc_alloc_block(inode_ref, goal, &allocated);
			if (rc != EOK) {
				close_extent_path(inode_ref->fs, path, depth);
				return rc;
			}
			allocated_cluster = true;
		}
		physical_block = allocated + (iblock - cluster_logical);
	} else {
		rc = ext4_balloc_alloc_block(inode_ref, goal, &allocated);
		if (rc != EOK) {
			close_extent_path(inode_ref->fs, path, depth);
			return rc;
		}
		allocated_cluster = true;
		physical_block = allocated;
	}
	if (physical_block > PEARE_EXTENT_PHYSICAL_MAX) {
		if (allocated_cluster)
			ext4_balloc_free_block(inode_ref, allocated);
		close_extent_path(inode_ref->fs, path, depth);
		return ENOTSUP;
	}

	{
		uint8_t *previous = position
			? node + PEARE_EXTENT_HEADER_SIZE +
			  (position - 1) * PEARE_EXTENT_ENTRY_SIZE : NULL;
		uint8_t *next = position < entries
			? node + PEARE_EXTENT_HEADER_SIZE +
			  position * PEARE_EXTENT_ENTRY_SIZE : NULL;

		merge_previous = previous &&
			(uint64_t)read_le32(previous) + extent_length(previous) ==
			iblock &&
			extent_physical(previous) + extent_length(previous) ==
				physical_block &&
			extent_length(previous) + 1 <=
				PEARE_EXTENT_MAX_BLOCKS;
		merge_next = next &&
			(uint64_t)iblock + 1 == read_le32(next) &&
			physical_block + 1 == extent_physical(next) &&
			extent_length(next) + 1 <=
				PEARE_EXTENT_MAX_BLOCKS;

		if (merge_previous) {
			uint32_t new_length = extent_length(previous) + 1;
			write_le16(previous + 4, (uint16_t)new_length);
			if (merge_next && new_length + extent_length(next) <=
					 PEARE_EXTENT_MAX_BLOCKS) {
				write_le16(previous + 4,
					   (uint16_t)(new_length + extent_length(next)));
				memmove(next, next + PEARE_EXTENT_ENTRY_SIZE,
					(entries - position - 1) * PEARE_EXTENT_ENTRY_SIZE);
				memset(node + PEARE_EXTENT_HEADER_SIZE +
				       (entries - 1) * PEARE_EXTENT_ENTRY_SIZE,
				       0, PEARE_EXTENT_ENTRY_SIZE);
				write_le16(node + 2, --entries);
				leaf->entries = entries;
			}
			rc = propagate_first_key(inode_ref, path, depth);
		} else if (merge_next) {
			set_extent(next, iblock, extent_length(next) + 1,
				   physical_block);
			rc = propagate_first_key(inode_ref, path, depth);
		} else {
			items = ext4_calloc((size_t)entries + 1,
					    PEARE_EXTENT_ENTRY_SIZE);
			if (!items) {
				close_extent_path(inode_ref->fs, path, depth);
				return ENOMEM;
			}
			memcpy(items, node + PEARE_EXTENT_HEADER_SIZE,
			       position * PEARE_EXTENT_ENTRY_SIZE);
			set_extent(items + position * PEARE_EXTENT_ENTRY_SIZE,
				   iblock, 1, physical_block);
			memcpy(items + (position + 1) * PEARE_EXTENT_ENTRY_SIZE,
			       node + PEARE_EXTENT_HEADER_SIZE +
			       position * PEARE_EXTENT_ENTRY_SIZE,
			       (entries - position) * PEARE_EXTENT_ENTRY_SIZE);
			if (entries < capacity) {
				replace_node_entries(node, 0, items, entries + 1);
				leaf->entries = entries + 1;
				rc = propagate_first_key(inode_ref, path, depth);
			} else if (!depth) {
				rc = grow_extent_root(inode_ref, 0, items, entries + 1);
			} else {
				uint32_t left_key;
				uint32_t right_key;
				ext4_fsblk_t right_block;
				rc = split_external_node(inode_ref, leaf, 0, items,
							 entries + 1, &left_key,
							 &right_key, &right_block);
				if (rc == EOK) {
					leaf->entries = (entries + 1) / 2;
					rc = insert_split_child(inode_ref, path, depth,
								depth - 1,
								path[depth - 1].slot,
								left_key, right_key,
								right_block);
				}
			}
			ext4_free(items);
		}
	}

	if (rc == EOK)
		rc = mark_changed_extent_path(inode_ref, path, depth);
	if (rc == EOK) {
		*result = physical_block;
		if (blocks_count)
			*blocks_count = 1;
	}
	{
		int close_rc = close_extent_path(inode_ref->fs, path, depth);
		if (rc == EOK)
			rc = close_rc;
	}
	return rc;
}

static int truncate_extent_node(struct ext4_inode_ref *ref, uint8_t *node,
				uint16_t depth, ext4_lblk_t from,
				struct ext4_block *block, bool *empty)
{
	uint16_t entries = node_entry_count(node);
	uint32_t cluster_bits = ext4_sb_cluster_ratio_bits(&ref->fs->sb);
	bool changed = false;
	int rc = EOK;
	if (cluster_bits == UINT32_MAX)
		return ENOTSUP;

	if (!depth) {
		while (entries) {
			uint8_t *entry = node + PEARE_EXTENT_HEADER_SIZE +
					 (entries - 1) * PEARE_EXTENT_ENTRY_SIZE;
			uint32_t logical = read_le32(entry);
			uint32_t length = extent_length(entry);
			uint64_t physical = extent_physical(entry);
			uint64_t end = (uint64_t)logical + length;
			uint32_t free_count = 0;
			uint64_t free_start;

			if (end <= from)
				break;
			if (cluster_bits) {
				uint64_t ratio = (uint64_t)1 << cluster_bits;
				uint64_t first_free =
					((uint64_t)from + ratio - 1) & ~(ratio - 1);
				free_start = logical < first_free ? first_free : logical;
				if (free_start < end) {
					free_count = (uint32_t)(end - free_start);
					rc = ext4_balloc_free_blocks(
						ref, physical + free_start - logical,
						free_count);
					if (rc != EOK)
						return rc;
				}
				if (logical < from) {
					write_le16(entry + 4, (uint16_t)(from - logical));
					changed = true;
					break;
				}
			} else {
				free_count = logical < from
					? (uint32_t)(end - from) : length;
				if (free_count) {
					rc = ext4_balloc_free_blocks(
						ref, physical + length - free_count,
						free_count);
					if (rc != EOK)
						return rc;
				}
			}
			changed = true;
			if (logical >= from) {
				memset(entry, 0, PEARE_EXTENT_ENTRY_SIZE);
				write_le16(node + 2, --entries);
			} else {
				write_le16(entry + 4, (uint16_t)(length - free_count));
				break;
			}
		}
	} else {
		while (entries) {
			uint8_t *index = node + PEARE_EXTENT_HEADER_SIZE +
					 (entries - 1) * PEARE_EXTENT_ENTRY_SIZE;
			ext4_fsblk_t child_block = extent_index_block(index);
			struct ext4_block child;
			uint16_t child_entries;
			bool child_empty;
			rc = ext4_trans_block_get(ref->fs->bdev, &child, child_block);
			if (rc != EOK)
				return rc;
			rc = validate_node(ref, child.data, false, depth - 1,
					   &child_entries);
			if (rc == EOK && child_entries &&
			    read_le32(index) != node_first_logical(child.data))
				rc = EIO;
			if (rc == EOK)
				rc = truncate_extent_node(ref, child.data, depth - 1,
							  from, &child, &child_empty);
			if (rc != EOK) {
				ext4_block_set(ref->fs->bdev, &child);
				return rc;
			}
			changed = true;
			if (child_empty) {
				rc = ext4_block_set(ref->fs->bdev, &child);
				if (rc != EOK)
					return rc;
				rc = ext4_balloc_free_block(ref, child_block);
				if (rc != EOK)
					return rc;
				memset(index, 0, PEARE_EXTENT_ENTRY_SIZE);
				write_le16(node + 2, --entries);
				continue;
			}
			write_le32(index, node_first_logical(child.data));
			rc = ext4_block_set(ref->fs->bdev, &child);
			if (rc != EOK)
				return rc;
			break;
		}
	}

	*empty = entries == 0;
	if (changed) {
		if (block) {
			update_extent_checksum(ref, node,
					       ext4_sb_get_block_size(&ref->fs->sb));
			rc = ext4_trans_set_block_dirty(block->buf);
		} else {
			ref->dirty = true;
		}
	}
	return rc;
}

static int collapse_extent_root(struct ext4_inode_ref *ref)
{
	uint8_t *root = extent_root(ref);
	uint16_t depth = read_le16(root + 6);
	int rc;

	while (depth) {
		uint16_t count = read_le16(root + 2);
		uint8_t *index;
		struct ext4_block child;
		ext4_fsblk_t child_block;
		uint16_t child_entries;

		if (!count) {
			memset(root + PEARE_EXTENT_HEADER_SIZE, 0,
			       PEARE_EXTENT_ROOT_CAPACITY * PEARE_EXTENT_ENTRY_SIZE);
			write_le16(root + 6, 0);
			ref->dirty = true;
			return EOK;
		}
		if (count != 1)
			break;
		index = root + PEARE_EXTENT_HEADER_SIZE;
		child_block = extent_index_block(index);
		rc = ext4_trans_block_get(ref->fs->bdev, &child, child_block);
		if (rc != EOK)
			return rc;
		rc = validate_node(ref, child.data, false, depth - 1, &child_entries);
		if (rc != EOK) {
			ext4_block_set(ref->fs->bdev, &child);
			return rc;
		}
		if (child_entries > PEARE_EXTENT_ROOT_CAPACITY) {
			rc = ext4_block_set(ref->fs->bdev, &child);
			if (rc != EOK)
				return rc;
			break;
		}
		memset(root + PEARE_EXTENT_HEADER_SIZE, 0,
		       PEARE_EXTENT_ROOT_CAPACITY * PEARE_EXTENT_ENTRY_SIZE);
		memcpy(root + PEARE_EXTENT_HEADER_SIZE,
		       child.data + PEARE_EXTENT_HEADER_SIZE,
		       child_entries * PEARE_EXTENT_ENTRY_SIZE);
		write_le16(root + 2, child_entries);
		write_le16(root + 4, PEARE_EXTENT_ROOT_CAPACITY);
		write_le16(root + 6, depth - 1);
		rc = ext4_block_set(ref->fs->bdev, &child);
		if (rc != EOK)
			return rc;
		rc = ext4_balloc_free_block(ref, child_block);
		if (rc != EOK)
			return rc;
		--depth;
		ref->dirty = true;
		if (!depth && !child_entries)
			break;
	}
	return EOK;
}

int ext4_extent_remove_space(struct ext4_inode_ref *inode_ref, ext4_lblk_t from,
			     ext4_lblk_t to)
{
	uint8_t *root;
	uint16_t depth;
	uint16_t entries;
	bool empty;
	int rc;

	if (!inode_ref || !inode_ref->inode || !inode_ref->fs || from > to)
		return EINVAL;
	if (to != EXT_MAX_BLOCKS)
		return ENOTSUP;
	root = extent_root(inode_ref);
	depth = read_le16(root + 6);
	if (depth > PEARE_EXTENT_MAX_WRITE_DEPTH)
		return ENOTSUP;
	rc = validate_node(inode_ref, root, true, depth, &entries);
	if (rc != EOK)
		return rc;
	rc = truncate_extent_node(inode_ref, root, depth, from, NULL, &empty);
	if (rc != EOK)
		return rc;
	if (depth) {
		rc = collapse_extent_root(inode_ref);
		if (rc != EOK)
			return rc;
	}
	inode_ref->dirty = true;
	return EOK;
}

struct peare_extent_collect_state {
	struct ext4_extent_map_entry *entries;
	size_t count;
	size_t capacity;
};

struct peare_extent_child {
	uint32_t logical;
	ext4_fsblk_t block;
};

static int collect_extent_node(struct ext4_inode_ref *ref, uint8_t *node,
			       bool root, uint16_t depth,
			       struct peare_extent_collect_state *state)
{
	uint16_t entries;
	uint16_t i;
	int rc = validate_node(ref, node, root, depth, &entries);

	if (rc != EOK)
		return rc;
	for (i = 0; i < entries; ++i) {
		uint8_t *entry = node + PEARE_EXTENT_HEADER_SIZE +
				 i * PEARE_EXTENT_ENTRY_SIZE;
		if (!depth) {
			struct ext4_extent_map_entry *grown;
			uint16_t raw_length = read_le16(entry + 4);
			uint32_t length = extent_length(entry);

			if (state->count == state->capacity) {
				size_t capacity = state->capacity ? state->capacity * 2 : 8;
				if (capacity < state->capacity ||
				    capacity > ref->fs->bdev->lg_bcnt)
					return ENOMEM;
				grown = ext4_realloc(state->entries,
					capacity * sizeof(*grown));
				if (!grown)
					return ENOMEM;
				state->entries = grown;
				state->capacity = capacity;
			}
			state->entries[state->count].logical = read_le32(entry);
			state->entries[state->count].length = length;
			state->entries[state->count].physical = extent_physical(entry);
			state->entries[state->count].unwritten =
				raw_length > PEARE_EXTENT_MAX_BLOCKS;
			state->count++;
			continue;
		}
		{
			struct ext4_block child;
			uint16_t child_entries;
			uint64_t block_no = extent_index_block(entry);
			rc = ext4_block_get(ref->fs->bdev, &child, block_no);
			if (rc != EOK)
				return rc;
			rc = validate_node(ref, child.data, false, depth - 1,
					   &child_entries);
			if (rc == EOK && (!child_entries ||
			    read_le32(entry) != node_first_logical(child.data)))
				rc = EIO;
			if (rc == EOK)
				rc = collect_extent_node(ref, child.data, false,
							 depth - 1, state);
			{
				int close_rc = ext4_block_set(ref->fs->bdev, &child);
				if (rc == EOK)
					rc = close_rc;
			}
			if (rc != EOK)
				return rc;
		}
	}
	return EOK;
}

int ext4_extent_collect(struct ext4_inode_ref *inode_ref,
			struct ext4_extent_map_entry **entries, size_t *count)
{
	struct peare_extent_collect_state state = {0};
	uint8_t *root;
	uint16_t depth;
	int rc;

	if (!inode_ref || !inode_ref->inode || !inode_ref->fs || !entries ||
	    !count)
		return EINVAL;
	*entries = NULL;
	*count = 0;
	root = extent_root(inode_ref);
	depth = read_le16(root + 6);
	if (depth > PEARE_EXTENT_MAX_DEPTH)
		return ENOTSUP;
	rc = collect_extent_node(inode_ref, root, true, depth, &state);
	if (rc != EOK) {
		ext4_free(state.entries);
		return rc;
	}
	*entries = state.entries;
	*count = state.count;
	return EOK;
}

static int validate_extent_map(struct ext4_inode_ref *ref,
			       const struct ext4_extent_map_entry *entries,
			       size_t count)
{
	uint64_t previous_logical_end = 0;
	uint32_t cluster_bits = ext4_sb_cluster_ratio_bits(&ref->fs->sb);
	uint64_t cluster_ratio;
	size_t i;
	if (cluster_bits == UINT32_MAX)
		return ENOTSUP;
	cluster_ratio = (uint64_t)1 << cluster_bits;

	for (i = 0; i < count; ++i) {
		uint64_t logical_end = (uint64_t)entries[i].logical +
				       entries[i].length;
		uint64_t physical_end = entries[i].physical + entries[i].length;
		if (!entries[i].length || entries[i].length > PEARE_EXTENT_MAX_BLOCKS ||
		    logical_end > (uint64_t)UINT32_MAX + 1 ||
		    physical_end > ref->fs->bdev->lg_bcnt ||
		    (cluster_ratio > 1 &&
		     (entries[i].logical % cluster_ratio) !=
			 (entries[i].physical % cluster_ratio)) ||
		    (i && entries[i].logical < previous_logical_end))
			return EINVAL;
		previous_logical_end = logical_end;
	}
	return EOK;
}

static int build_extent_tree(struct ext4_inode_ref *ref,
			     const struct ext4_extent_map_entry *entries,
			     size_t count)
{
	struct peare_extent_child *children = NULL;
	size_t child_count = 0;
	size_t child_capacity = 0;
	size_t offset = 0;
	uint16_t depth = 0;
	uint16_t external_capacity = extent_node_capacity(ref, false);
	uint8_t *root = extent_root(ref);
	int rc = EOK;

	if (count <= PEARE_EXTENT_ROOT_CAPACITY) {
		uint8_t leaf_entries[PEARE_EXTENT_ROOT_CAPACITY * PEARE_EXTENT_ENTRY_SIZE];
		size_t i;
		memset(leaf_entries, 0, sizeof(leaf_entries));
		for (i = 0; i < count; ++i) {
			uint8_t *entry = leaf_entries + i * PEARE_EXTENT_ENTRY_SIZE;
			set_extent(entry, entries[i].logical, entries[i].length,
				   entries[i].physical);
			if (entries[i].unwritten)
				write_le16(entry + 4,
					   (uint16_t)(entries[i].length +
						      PEARE_EXTENT_MAX_BLOCKS));
		}
		replace_node_entries(root, 0, leaf_entries, (uint16_t)count);
		ref->dirty = true;
		return EOK;
	}

	while (offset < count) {
		size_t chunk = count - offset;
		uint8_t *leaf_entries;
		struct peare_extent_child *grown;
		ext4_fsblk_t block;
		size_t i;
		if (chunk > external_capacity)
			chunk = external_capacity;
		leaf_entries = ext4_calloc(chunk, PEARE_EXTENT_ENTRY_SIZE);
		if (!leaf_entries) {
			rc = ENOMEM;
			goto out;
		}
		for (i = 0; i < chunk; ++i) {
			uint8_t *entry = leaf_entries + i * PEARE_EXTENT_ENTRY_SIZE;
			set_extent(entry, entries[offset + i].logical,
				   entries[offset + i].length,
				   entries[offset + i].physical);
			if (entries[offset + i].unwritten)
				write_le16(entry + 4,
					   (uint16_t)(entries[offset + i].length +
						      PEARE_EXTENT_MAX_BLOCKS));
		}
		rc = allocate_extent_node(ref, 0, leaf_entries,
					  (uint16_t)chunk, &block);
		ext4_free(leaf_entries);
		if (rc != EOK)
			goto out;
		if (child_count == child_capacity) {
			size_t capacity = child_capacity ? child_capacity * 2 : 8;
			grown = ext4_realloc(children, capacity * sizeof(*grown));
			if (!grown) {
				ext4_balloc_free_block(ref, block);
				rc = ENOMEM;
				goto out;
			}
			children = grown;
			child_capacity = capacity;
		}
		children[child_count].logical = entries[offset].logical;
		children[child_count].block = block;
		child_count++;
		offset += chunk;
	}
	depth = 1;
	while (child_count > PEARE_EXTENT_ROOT_CAPACITY) {
		struct peare_extent_child *parents = NULL;
		size_t parent_count = 0;
		size_t parent_capacity = 0;
		offset = 0;
		if (depth >= PEARE_EXTENT_MAX_WRITE_DEPTH) {
			rc = ENOTSUP;
			goto out;
		}
		while (offset < child_count) {
			size_t chunk = child_count - offset;
			uint8_t *index_entries;
			struct peare_extent_child *grown;
			ext4_fsblk_t block;
			size_t i;
			if (chunk > external_capacity)
				chunk = external_capacity;
			index_entries = ext4_calloc(chunk, PEARE_EXTENT_ENTRY_SIZE);
			if (!index_entries) {
				ext4_free(parents);
				rc = ENOMEM;
				goto out;
			}
			for (i = 0; i < chunk; ++i)
				set_extent_index(index_entries + i * PEARE_EXTENT_ENTRY_SIZE,
						 children[offset + i].logical,
						 children[offset + i].block);
			rc = allocate_extent_node(ref, depth, index_entries,
						  (uint16_t)chunk, &block);
			ext4_free(index_entries);
			if (rc != EOK) {
				ext4_free(parents);
				goto out;
			}
			if (parent_count == parent_capacity) {
				size_t capacity = parent_capacity ? parent_capacity * 2 : 8;
				grown = ext4_realloc(parents, capacity * sizeof(*grown));
				if (!grown) {
					ext4_balloc_free_block(ref, block);
					ext4_free(parents);
					rc = ENOMEM;
					goto out;
				}
				parents = grown;
				parent_capacity = capacity;
			}
			parents[parent_count].logical = children[offset].logical;
			parents[parent_count].block = block;
			parent_count++;
			offset += chunk;
		}
		ext4_free(children);
		children = parents;
		child_count = parent_count;
		child_capacity = parent_capacity;
		depth++;
	}
	{
		uint8_t root_entries[PEARE_EXTENT_ROOT_CAPACITY * PEARE_EXTENT_ENTRY_SIZE];
		size_t i;
		memset(root_entries, 0, sizeof(root_entries));
		for (i = 0; i < child_count; ++i)
			set_extent_index(root_entries + i * PEARE_EXTENT_ENTRY_SIZE,
					 children[i].logical, children[i].block);
		replace_node_entries(root, depth, root_entries,
				     (uint16_t)child_count);
		ref->dirty = true;
	}
out:
	ext4_free(children);
	return rc;
}

int ext4_extent_replace(struct ext4_inode_ref *inode_ref,
			const struct ext4_extent_map_entry *entries, size_t count)
{
	size_t i;
	int rc;

	if (!inode_ref || !inode_ref->inode || !inode_ref->fs ||
	    (count && !entries))
		return EINVAL;
	rc = validate_extent_map(inode_ref, entries, count);
	if (rc != EOK)
		return rc;
	rc = ext4_extent_remove_space(inode_ref, 0, EXT_MAX_BLOCKS);
	if (rc != EOK)
		return rc;
	ext4_extent_tree_init(inode_ref);
	for (i = 0; i < count; ++i) {
		uint32_t block;
		for (block = 0; block < entries[i].length; ++block) {
			bool was_free;
			rc = ext4_balloc_try_alloc_block(inode_ref,
						 entries[i].physical + block, &was_free);
			if (rc != EOK)
				return rc;
		}
	}
	return build_extent_tree(inode_ref, entries, count);
}
