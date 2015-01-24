#ifndef KVM__QCOW_H
#define KVM__QCOW_H

#include <linux/types.h>

#define QCOW_MAGIC		(('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)

#define QCOW1_VERSION		1
#define QCOW2_VERSION		2

#define QCOW1_OFLAG_COMPRESSED	(1LL << 63)

#define QCOW1_OFLAG_MASK	QCOW1_OFLAG_COMPRESSED

#define QCOW2_OFLAG_COPIED	(1LL << 63)
#define QCOW2_OFLAG_COMPRESSED	(1LL << 62)
#define QCOW2_OFLAG_MASK	(QCOW2_OFLAG_COPIED|QCOW2_OFLAG_COMPRESSED)

struct qcow_table {
	u32			table_size;
	u64			*l1_table;
};

struct qcow {
	void			*header;
	struct qcow_table	table;
	int			fd;
};

struct qcow_header {
	u64			size; /* in bytes */
	u64			l1_table_offset;
	u32			l1_size;
	u8			cluster_bits;
	u8			l2_bits;
	uint64_t		oflag_mask;
};

struct qcow1_header_disk {
	u32			magic;
	u32			version;

	u64			backing_file_offset;
	u32 			backing_file_size;
	u32			mtime;

	u64			size; /* in bytes */

	u8			cluster_bits;
	u8			l2_bits;
	u32			crypt_method;

	u64			l1_table_offset;
};

struct qcow2_header_disk {
	u32			magic;
	u32			version;

	u64			backing_file_offset;
	u32			backing_file_size;

	u32			cluster_bits;
	u64			size; /* in bytes */
	u32			crypt_method;

	u32			l1_size;
	u64			l1_table_offset;

	u64			refcount_table_offset;
	u32			refcount_table_clusters;

	u32			nb_snapshots;
	u64			snapshots_offset;
};

struct disk_image *qcow_probe(int fd);

#endif /* KVM__QCOW_H */
