#include "kvm/virtio-pci-dev.h"
#include "kvm/virtio-net.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/types.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/irq.h"
#include "kvm/uip.h"
#include "kvm/ioeventfd.h"

#include <linux/virtio_net.h>
#include <linux/if_tun.h>

#include <arpa/inet.h>
#include <net/if.h>

#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define VIRTIO_NET_QUEUE_SIZE		128
#define VIRTIO_NET_NUM_QUEUES		2
#define VIRTIO_NET_RX_QUEUE		0
#define VIRTIO_NET_TX_QUEUE		1

static struct pci_device_header pci_header = {
	.vendor_id			= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id			= PCI_DEVICE_ID_VIRTIO_NET,
	.header_type			= PCI_HEADER_TYPE_NORMAL,
	.revision_id			= 0,
	.class				= 0x020000,
	.subsys_vendor_id		= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id			= VIRTIO_ID_NET,
};

struct net_dev;

struct net_dev_operations {
	int (*rx)(struct iovec *iov, u16 in, struct net_dev *ndev);
	int (*tx)(struct iovec *iov, u16 in, struct net_dev *ndev);
};

struct net_dev {
	pthread_mutex_t			mutex;

	struct virt_queue		vqs[VIRTIO_NET_NUM_QUEUES];
	struct virtio_net_config	config;
	u32				host_features;
	u32				guest_features;
	u16				config_vector;
	u8				status;
	u8				isr;
	u16				queue_selector;
	u16				base_addr;

	pthread_t			io_rx_thread;
	pthread_mutex_t			io_rx_lock;
	pthread_cond_t			io_rx_cond;

	pthread_t			io_tx_thread;
	pthread_mutex_t			io_tx_lock;
	pthread_cond_t			io_tx_cond;

	int				tap_fd;
	char				tap_name[IFNAMSIZ];

	int				mode;

	struct uip_info			info;
	struct net_dev_operations	*ops;
};

static struct net_dev ndev = {
	.mutex	= PTHREAD_MUTEX_INITIALIZER,

	.config = {
		.mac			= {0x00, 0x15, 0x15, 0x15, 0x15, 0x15},
		.status			= VIRTIO_NET_S_LINK_UP,
	},
	.host_features			= 1UL << VIRTIO_NET_F_MAC
					| 1UL << VIRTIO_NET_F_CSUM
					| 1UL << VIRTIO_NET_F_HOST_UFO
					| 1UL << VIRTIO_NET_F_HOST_TSO4
					| 1UL << VIRTIO_NET_F_HOST_TSO6
					| 1UL << VIRTIO_NET_F_GUEST_UFO
					| 1UL << VIRTIO_NET_F_GUEST_TSO4
					| 1UL << VIRTIO_NET_F_GUEST_TSO6,
	.info = {
		.host_mac.addr		= {0x00, 0x01, 0x01, 0x01, 0x01, 0x01},
		.guest_mac.addr		= {0x00, 0x15, 0x15, 0x15, 0x15, 0x15},
		.host_ip		= 0xc0a82101,
		.buf_nr			= 20,
	}
};

static void *virtio_net_rx_thread(void *p)
{
	struct iovec iov[VIRTIO_NET_QUEUE_SIZE];
	struct virt_queue *vq;
	struct kvm *kvm;
	u16 out, in;
	u16 head;
	int len;

	kvm	= p;
	vq	= &ndev.vqs[VIRTIO_NET_RX_QUEUE];

	while (1) {

		mutex_lock(&ndev.io_rx_lock);
		if (!virt_queue__available(vq))
			pthread_cond_wait(&ndev.io_rx_cond, &ndev.io_rx_lock);
		mutex_unlock(&ndev.io_rx_lock);

		while (virt_queue__available(vq)) {

			head = virt_queue__get_iov(vq, iov, &out, &in, kvm);

			len = ndev.ops->rx(iov, in, &ndev);

			virt_queue__set_used_elem(vq, head, len);

			/* We should interrupt guest right now, otherwise latency is huge. */
			virt_queue__trigger_irq(vq, pci_header.irq_line, &ndev.isr, kvm);
		}

	}

	pthread_exit(NULL);
	return NULL;

}

static void *virtio_net_tx_thread(void *p)
{
	struct iovec iov[VIRTIO_NET_QUEUE_SIZE];
	struct virt_queue *vq;
	struct kvm *kvm;
	u16 out, in;
	u16 head;
	int len;

	kvm	= p;
	vq	= &ndev.vqs[VIRTIO_NET_TX_QUEUE];

	while (1) {
		mutex_lock(&ndev.io_tx_lock);
		if (!virt_queue__available(vq))
			pthread_cond_wait(&ndev.io_tx_cond, &ndev.io_tx_lock);
		mutex_unlock(&ndev.io_tx_lock);

		while (virt_queue__available(vq)) {

			head = virt_queue__get_iov(vq, iov, &out, &in, kvm);

			len = ndev.ops->tx(iov, out, &ndev);

			virt_queue__set_used_elem(vq, head, len);
		}

		virt_queue__trigger_irq(vq, pci_header.irq_line, &ndev.isr, kvm);

	}

	pthread_exit(NULL);

	return NULL;

}

static bool virtio_net_pci_io_device_specific_in(void *data, unsigned long offset, int size, u32 count)
{
	u8 *config_space = (u8 *)&ndev.config;

	if (size != 1 || count != 1)
		return false;

	if ((offset - VIRTIO_MSI_CONFIG_VECTOR) > sizeof(struct virtio_net_config))
		pr_error("config offset is too big: %li", offset - VIRTIO_MSI_CONFIG_VECTOR);

	ioport__write8(data, config_space[offset - VIRTIO_MSI_CONFIG_VECTOR]);

	return true;
}

static bool virtio_net_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	unsigned long	offset	= port - ndev.base_addr;
	bool		ret	= true;

	mutex_lock(&ndev.mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, ndev.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret = false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, ndev.vqs[ndev.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_NET_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret = false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, ndev.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, ndev.isr);
		kvm__irq_line(kvm, pci_header.irq_line, VIRTIO_IRQ_LOW);
		ndev.isr = VIRTIO_IRQ_LOW;
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, ndev.config_vector);
		break;
	default:
		ret = virtio_net_pci_io_device_specific_in(data, offset, size, count);
	};

	mutex_unlock(&ndev.mutex);

	return ret;
}

static void virtio_net_handle_callback(struct kvm *kvm, u16 queue_index)
{
	switch (queue_index) {
	case VIRTIO_NET_TX_QUEUE:
		mutex_lock(&ndev.io_tx_lock);
		pthread_cond_signal(&ndev.io_tx_cond);
		mutex_unlock(&ndev.io_tx_lock);
		break;
	case VIRTIO_NET_RX_QUEUE:
		mutex_lock(&ndev.io_rx_lock);
		pthread_cond_signal(&ndev.io_rx_cond);
		mutex_unlock(&ndev.io_rx_lock);
		break;
	default:
		pr_warning("Unknown queue index %u", queue_index);
	}
}

static bool virtio_net_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	unsigned long	offset		= port - ndev.base_addr;
	bool		ret		= true;

	mutex_lock(&ndev.mutex);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		ndev.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		assert(ndev.queue_selector < VIRTIO_NET_NUM_QUEUES);

		queue			= &ndev.vqs[ndev.queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		vring_init(&queue->vring, VIRTIO_NET_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		ndev.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;

		queue_index		= ioport__read16(data);
		virtio_net_handle_callback(kvm, queue_index);
		break;
	}
	case VIRTIO_PCI_STATUS:
		ndev.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ndev.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret			= false;
	};

	mutex_unlock(&ndev.mutex);

	return ret;
}

static void ioevent_callback(struct kvm *kvm, void *param)
{
	virtio_net_handle_callback(kvm, (u64)(long)param);
}

static struct ioport_operations virtio_net_io_ops = {
	.io_in	= virtio_net_pci_io_in,
	.io_out	= virtio_net_pci_io_out,
};

static bool virtio_net__tap_init(const struct virtio_net_parameters *params)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	int i, pid, status, offload, hdr_len;
	struct sockaddr_in sin = {0};
	struct ifreq ifr;

	for (i = 0 ; i < 6 ; i++)
		ndev.config.mac[i] = params->guest_mac[i];

	ndev.tap_fd = open("/dev/net/tun", O_RDWR);
	if (ndev.tap_fd < 0) {
		pr_warning("Unable to open /dev/net/tun");
		goto fail;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;
	if (ioctl(ndev.tap_fd, TUNSETIFF, &ifr) < 0) {
		pr_warning("Config tap device error. Are you root?");
		goto fail;
	}

	strncpy(ndev.tap_name, ifr.ifr_name, sizeof(ndev.tap_name));

	if (ioctl(ndev.tap_fd, TUNSETNOCSUM, 1) < 0) {
		pr_warning("Config tap device TUNSETNOCSUM error");
		goto fail;
	}

	hdr_len = sizeof(struct virtio_net_hdr);
	if (ioctl(ndev.tap_fd, TUNSETVNETHDRSZ, &hdr_len) < 0) {
		pr_warning("Config tap device TUNSETVNETHDRSZ error");
	}

	offload = TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6 | TUN_F_UFO;
	if (ioctl(ndev.tap_fd, TUNSETOFFLOAD, offload) < 0) {
		pr_warning("Config tap device TUNSETOFFLOAD error");
		goto fail;
	}

	if (strcmp(params->script, "none")) {
		pid = fork();
		if (pid == 0) {
			execl(params->script, params->script, ndev.tap_name, NULL);
			_exit(1);
		} else {
			waitpid(pid, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
				pr_warning("Fail to setup tap by %s", params->script);
				goto fail;
			}
		}
	} else {
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, ndev.tap_name, sizeof(ndev.tap_name));
		sin.sin_addr.s_addr = inet_addr(params->host_ip);
		memcpy(&(ifr.ifr_addr), &sin, sizeof(ifr.ifr_addr));
		ifr.ifr_addr.sa_family = AF_INET;
		if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
			pr_warning("Could not set ip address on tap device");
			goto fail;
		}
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ndev.tap_name, sizeof(ndev.tap_name));
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
		pr_warning("Could not bring tap device up");

	close(sock);

	return 1;

fail:
	if (sock >= 0)
		close(sock);
	if (ndev.tap_fd >= 0)
		close(ndev.tap_fd);

	return 0;
}

static void virtio_net__io_thread_init(struct kvm *kvm)
{
	pthread_mutex_init(&ndev.io_rx_lock, NULL);
	pthread_cond_init(&ndev.io_tx_cond, NULL);

	pthread_mutex_init(&ndev.io_rx_lock, NULL);
	pthread_cond_init(&ndev.io_tx_cond, NULL);

	pthread_create(&ndev.io_rx_thread, NULL, virtio_net_rx_thread, (void *)kvm);
	pthread_create(&ndev.io_tx_thread, NULL, virtio_net_tx_thread, (void *)kvm);
}

static inline int tap_ops_tx(struct iovec *iov, u16 out, struct net_dev *ndev)
{
	return writev(ndev->tap_fd, iov, out);
}

static inline int tap_ops_rx(struct iovec *iov, u16 in, struct net_dev *ndev)
{
	return readv(ndev->tap_fd, iov, in);
}

static inline int uip_ops_tx(struct iovec *iov, u16 out, struct net_dev *ndev)
{
	return uip_tx(iov, out, &ndev->info);
}

static inline int uip_ops_rx(struct iovec *iov, u16 in, struct net_dev *ndev)
{
	return uip_rx(iov, in, &ndev->info);
}

static struct net_dev_operations tap_ops = {
	.rx	= tap_ops_rx,
	.tx	= tap_ops_tx,
};

static struct net_dev_operations uip_ops = {
	.rx	= uip_ops_rx,
	.tx	= uip_ops_tx,
};

void virtio_net__init(const struct virtio_net_parameters *params)
{
	struct ioevent ioevent;
	u8 dev, line, pin;
	u16 net_base_addr;
	int i;

	if (irq__register_device(VIRTIO_ID_NET, &dev, &pin, &line) < 0)
		return;

	pci_header.irq_pin  = pin;
	pci_header.irq_line = line;
	net_base_addr	    = ioport__register(IOPORT_EMPTY, &virtio_net_io_ops, IOPORT_SIZE, NULL);
	pci_header.bar[0]   = net_base_addr | PCI_BASE_ADDRESS_SPACE_IO;
	ndev.base_addr	    = net_base_addr;
	pci__register(&pci_header, dev);

	ndev.mode = params->mode;
	if (ndev.mode == NET_MODE_TAP) {
		virtio_net__tap_init(params);
		ndev.ops = &tap_ops;
	} else {
		uip_init(&ndev.info);
		ndev.ops = &uip_ops;
	}

	virtio_net__io_thread_init(params->kvm);

	for (i = 0; i < VIRTIO_NET_NUM_QUEUES; i++) {
		ioevent = (struct ioevent) {
			.io_addr	= net_base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
			.io_len		= sizeof(u16),
			.fn		= ioevent_callback,
			.datamatch	= i,
			.fn_ptr		= (void *)(long)i,
			.fn_kvm		= params->kvm,
			.fd		= eventfd(0, 0),
		};

		ioeventfd__add_event(&ioevent);
	}
}
