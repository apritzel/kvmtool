#include "kvm/virtio-rng.h"

#include "kvm/virtio-pci-dev.h"

#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/irq.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_rng.h>

#include <linux/list.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#define NUM_VIRT_QUEUES				1
#define VIRTIO_RNG_QUEUE_SIZE			128

struct rng_dev_job {
	struct virt_queue		*vq;
	struct rng_dev			*rdev;
	void				*job_id;
};

struct rng_dev {
	struct pci_device_header pci_hdr;
	struct list_head	list;

	u16			base_addr;
	u8			status;
	u8			isr;
	u16			config_vector;
	int			fd;

	/* virtio queue */
	u16			queue_selector;
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct rng_dev_job	jobs[NUM_VIRT_QUEUES];
};

static LIST_HEAD(rdevs);

static bool virtio_rng_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	unsigned long offset;
	bool ret = true;
	struct rng_dev *rdev;

	rdev = ioport->priv;
	offset = port - rdev->base_addr;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
	case VIRTIO_PCI_GUEST_FEATURES:
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret		= false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, rdev->vqs[rdev->queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_RNG_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, rdev->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, rdev->isr);
		kvm__irq_line(kvm, rdev->pci_hdr.irq_line, VIRTIO_IRQ_LOW);
		rdev->isr = VIRTIO_IRQ_LOW;
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, rdev->config_vector);
		break;
	default:
		ret		= false;
		break;
	};

	return ret;
}

static bool virtio_rng_do_io_request(struct kvm *kvm, struct rng_dev *rdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_RNG_QUEUE_SIZE];
	unsigned int len = 0;
	u16 out, in, head;

	head		= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	len		= readv(rdev->fd, iov, in);

	virt_queue__set_used_elem(queue, head, len);

	return true;
}

static void virtio_rng_do_io(struct kvm *kvm, void *param)
{
	struct rng_dev_job *job = param;
	struct virt_queue *vq = job->vq;
	struct rng_dev *rdev = job->rdev;

	while (virt_queue__available(vq)) {
		virtio_rng_do_io_request(kvm, rdev, vq);
		virt_queue__trigger_irq(vq, rdev->pci_hdr.irq_line, &rdev->isr, kvm);
	}
}

static bool virtio_rng_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	unsigned long offset;
	bool ret = true;
	struct rng_dev *rdev;

	rdev = ioport->priv;
	offset = port - rdev->base_addr;

	switch (offset) {
	case VIRTIO_MSI_QUEUE_VECTOR:
	case VIRTIO_PCI_GUEST_FEATURES:
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		struct rng_dev_job *job;
		void *p;

		queue			= &rdev->vqs[rdev->queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		job = &rdev->jobs[rdev->queue_selector];

		vring_init(&queue->vring, VIRTIO_RNG_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

		*job			= (struct rng_dev_job) {
			.vq			= queue,
			.rdev			= rdev,
		};

		job->job_id = thread_pool__add_job(kvm, virtio_rng_do_io, job);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		rdev->queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;
		queue_index		= ioport__read16(data);
		thread_pool__do_job(rdev->jobs[queue_index].job_id);
		break;
	}
	case VIRTIO_PCI_STATUS:
		rdev->status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		rdev->config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	default:
		ret			= false;
		break;
	};

	return ret;
}

static struct ioport_operations virtio_rng_io_ops = {
	.io_in				= virtio_rng_pci_io_in,
	.io_out				= virtio_rng_pci_io_out,
};

void virtio_rng__init(struct kvm *kvm)
{
	u8 pin, line, dev;
	u16 rdev_base_addr;
	struct rng_dev *rdev;

	rdev = malloc(sizeof(*rdev));
	if (rdev == NULL)
		return;

	rdev_base_addr = ioport__register(IOPORT_EMPTY, &virtio_rng_io_ops, IOPORT_SIZE, rdev);

	rdev->pci_hdr = (struct pci_device_header) {
		.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
		.device_id		= PCI_DEVICE_ID_VIRTIO_RNG,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= 0,
		.class			= 0x010000,
		.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
		.subsys_id		= VIRTIO_ID_RNG,
		.bar[0]			= rdev_base_addr | PCI_BASE_ADDRESS_SPACE_IO,
	};

	rdev->base_addr = rdev_base_addr;
	rdev->fd = open("/dev/urandom", O_RDONLY);
	if (rdev->fd < 0)
		die("Failed initializing RNG");

	if (irq__register_device(VIRTIO_ID_RNG, &dev, &pin, &line) < 0)
		return;

	rdev->pci_hdr.irq_pin	= pin;
	rdev->pci_hdr.irq_line	= line;
	pci__register(&rdev->pci_hdr, dev);

	list_add_tail(&rdev->list, &rdevs);
}

void virtio_rng__delete_all(struct kvm *kvm)
{
	while (!list_empty(&rdevs)) {
		struct rng_dev *rdev;

		rdev = list_first_entry(&rdevs, struct rng_dev, list);
		list_del(&rdev->list);
		free(rdev);
	}
}
