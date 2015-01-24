#ifndef KVM__BLK_VIRTIO_H
#define KVM__BLK_VIRTIO_H

#include "kvm/disk-image.h"

struct kvm;

void virtio_blk__init(struct kvm *kvm, struct disk_image *disk);
void virtio_blk__init_all(struct kvm *kvm);

#endif /* KVM__BLK_VIRTIO_H */
