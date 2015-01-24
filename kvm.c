#include "kvm/kvm.h"

#include "kvm/boot-protocol.h"
#include "kvm/cpufeature.h"
#include "kvm/read-write.h"
#include "kvm/interrupt.h"
#include "kvm/mptable.h"
#include "kvm/util.h"

#include <linux/kvm.h>

#include <asm/bootparam.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>

#define DEFINE_KVM_EXIT_REASON(reason) [reason] = #reason

const char *kvm_exit_reasons[] = {
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_UNKNOWN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_EXCEPTION),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_IO),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_HYPERCALL),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_DEBUG),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_HLT),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_MMIO),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_IRQ_WINDOW_OPEN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_SHUTDOWN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_FAIL_ENTRY),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_INTR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_SET_TPR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_TPR_ACCESS),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_S390_SIEIC),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_S390_RESET),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_DCR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_NMI),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_INTERNAL_ERROR),
};

#define DEFINE_KVM_EXT(ext)		\
	.name = #ext,			\
	.code = ext

struct {
	const char *name;
	int code;
} kvm_req_ext[] = {
	{ DEFINE_KVM_EXT(KVM_CAP_COALESCED_MMIO) },
	{ DEFINE_KVM_EXT(KVM_CAP_SET_TSS_ADDR) },
	{ DEFINE_KVM_EXT(KVM_CAP_PIT2) },
	{ DEFINE_KVM_EXT(KVM_CAP_USER_MEMORY) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQ_ROUTING) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQCHIP) },
	{ DEFINE_KVM_EXT(KVM_CAP_HLT) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQ_INJECT_STATUS) },
	{ DEFINE_KVM_EXT(KVM_CAP_EXT_CPUID) },
};

static bool kvm__supports_extension(struct kvm *kvm, unsigned int extension)
{
	int ret;

	ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, extension);
	if (ret < 0)
		return false;

	return ret;
}

static int kvm__check_extensions(struct kvm *kvm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(kvm_req_ext); i++) {
		if (!kvm__supports_extension(kvm, kvm_req_ext[i].code)) {
			pr_error("Unsuppored KVM extension detected: %s",
				kvm_req_ext[i].name);
			return (int)-i;
		}
	}

	return 0;
}

static struct kvm *kvm__new(void)
{
	struct kvm *kvm = calloc(1, sizeof *kvm);

	if (!kvm)
		die("out of memory");

	return kvm;
}

void kvm__delete(struct kvm *kvm)
{
	kvm__stop_timer(kvm);

	munmap(kvm->ram_start, kvm->ram_size);
	free(kvm);
}

static bool kvm__cpu_supports_vm(void)
{
	struct cpuid_regs regs;
	u32 eax_base;
	int feature;

	regs	= (struct cpuid_regs) {
		.eax		= 0x00,
	};
	host_cpuid(&regs);

	switch (regs.ebx) {
	case CPUID_VENDOR_INTEL_1:
		eax_base	= 0x00;
		feature		= KVM__X86_FEATURE_VMX;
		break;

	case CPUID_VENDOR_AMD_1:
		eax_base	= 0x80000000;
		feature		= KVM__X86_FEATURE_SVM;
		break;

	default:
		return false;
	}

	regs	= (struct cpuid_regs) {
		.eax		= eax_base,
	};
	host_cpuid(&regs);

	if (regs.eax < eax_base + 0x01)
		return false;

	regs	= (struct cpuid_regs) {
		.eax		= eax_base + 0x01
	};
	host_cpuid(&regs);

	return regs.ecx & (1 << feature);
}

static void kvm_register_mem_slot(struct kvm *kvm, u32 slot, u64 guest_phys, u64 size, void *userspace_addr)
{
	struct kvm_userspace_memory_region mem;
	int ret;

	mem = (struct kvm_userspace_memory_region) {
		.slot			= slot,
		.guest_phys_addr	= guest_phys,
		.memory_size		= size,
		.userspace_addr		= (unsigned long)userspace_addr,
	};

	ret = ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
	if (ret < 0)
		die_perror("KVM_SET_USER_MEMORY_REGION ioctl");
}

/*
 * Allocating RAM size bigger than 4GB requires us to leave a gap
 * in the RAM which is used for PCI MMIO, hotplug, and unconfigured
 * devices (see documentation of e820_setup_gap() for details).
 *
 * If we're required to initialize RAM bigger than 4GB, we will create
 * a gap between 0xe0000000 and 0x100000000 in the guest virtual mem space.
 */

void kvm__init_ram(struct kvm *kvm)
{
	u64	phys_start, phys_size;
	void	*host_mem;

	if (kvm->ram_size < KVM_32BIT_GAP_START) {
		/* Use a single block of RAM for 32bit RAM */

		phys_start = 0;
		phys_size  = kvm->ram_size;
		host_mem   = kvm->ram_start;

		kvm_register_mem_slot(kvm, 0, 0, kvm->ram_size, kvm->ram_start);
	} else {
		/* First RAM range from zero to the PCI gap: */

		phys_start = 0;
		phys_size  = KVM_32BIT_GAP_START;
		host_mem   = kvm->ram_start;

		kvm_register_mem_slot(kvm, 0, phys_start, phys_size, host_mem);

		/* Second RAM range from 4GB to the end of RAM: */

		phys_start = 0x100000000ULL;
		phys_size  = kvm->ram_size - phys_size;
		host_mem   = kvm->ram_start + phys_start;

		kvm_register_mem_slot(kvm, 1, phys_start, phys_size, host_mem);
	}
}

int kvm__max_cpus(struct kvm *kvm)
{
	int ret;

	ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_NR_VCPUS);
	if (ret < 0)
		die_perror("KVM_CAP_NR_VCPUS");

	return ret;
}

struct kvm *kvm__init(const char *kvm_dev, unsigned long ram_size)
{
	struct kvm_pit_config pit_config = { .flags = 0, };
	struct kvm *kvm;
	int ret;

	if (!kvm__cpu_supports_vm())
		die("Your CPU does not support hardware virtualization");

	kvm = kvm__new();

	kvm->sys_fd = open(kvm_dev, O_RDWR);
	if (kvm->sys_fd < 0) {
		if (errno == ENOENT)
			die("'%s' not found. Please make sure your kernel has CONFIG_KVM enabled and that the KVM modules are loaded.", kvm_dev);
		if (errno == ENODEV)
			die("'%s' KVM driver not available.\n  # (If the KVM module is loaded then 'dmesg' may offer further clues about the failure.)", kvm_dev);

		fprintf(stderr, "  Fatal, could not open %s: ", kvm_dev);
		perror(NULL);
		exit(1);
	}

	ret = ioctl(kvm->sys_fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION)
		die_perror("KVM_API_VERSION ioctl");

	kvm->vm_fd = ioctl(kvm->sys_fd, KVM_CREATE_VM, 0);
	if (kvm->vm_fd < 0)
		die_perror("KVM_CREATE_VM ioctl");

	if (kvm__check_extensions(kvm))
		die("A required KVM extention is not supported by OS");

	ret = ioctl(kvm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000);
	if (ret < 0)
		die_perror("KVM_SET_TSS_ADDR ioctl");

	ret = ioctl(kvm->vm_fd, KVM_CREATE_PIT2, &pit_config);
	if (ret < 0)
		die_perror("KVM_CREATE_PIT2 ioctl");

	kvm->ram_size		= ram_size;

	if (kvm->ram_size < KVM_32BIT_GAP_START) {
		kvm->ram_start = mmap(NULL, ram_size, PROT_RW, MAP_ANON_NORESERVE, -1, 0);
	} else {
		kvm->ram_start = mmap(NULL, ram_size + KVM_32BIT_GAP_SIZE, PROT_RW, MAP_ANON_NORESERVE, -1, 0);
		if (kvm->ram_start != MAP_FAILED) {
			/*
			 * We mprotect the gap (see kvm__init_ram() for details) PROT_NONE so that
			 * if we accidently write to it, we will know.
			 */
			mprotect(kvm->ram_start + KVM_32BIT_GAP_START, KVM_32BIT_GAP_SIZE, PROT_NONE);
		}
	}
	if (kvm->ram_start == MAP_FAILED)
		die("out of memory");

	ret = ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP);
	if (ret < 0)
		die_perror("KVM_CREATE_IRQCHIP ioctl");

	return kvm;
}

#define BOOT_LOADER_SELECTOR	0x1000
#define BOOT_LOADER_IP		0x0000
#define BOOT_LOADER_SP		0x8000
#define BOOT_CMDLINE_OFFSET	0x20000

#define BOOT_PROTOCOL_REQUIRED	0x206
#define LOAD_HIGH		0x01

static int load_flat_binary(struct kvm *kvm, int fd)
{
	void *p;
	int nr;

	if (lseek(fd, 0, SEEK_SET) < 0)
		die_perror("lseek");

	p = guest_real_to_host(kvm, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);

	while ((nr = read(fd, p, 65536)) > 0)
		p += nr;

	kvm->boot_selector	= BOOT_LOADER_SELECTOR;
	kvm->boot_ip		= BOOT_LOADER_IP;
	kvm->boot_sp		= BOOT_LOADER_SP;

	return true;
}

static const char *BZIMAGE_MAGIC	= "HdrS";

static bool load_bzimage(struct kvm *kvm, int fd_kernel,
			int fd_initrd, const char *kernel_cmdline, u16 vidmode)
{
	struct boot_params *kern_boot;
	unsigned long setup_sects;
	struct boot_params boot;
	size_t cmdline_size;
	ssize_t setup_size;
	void *p;
	int nr;

	/*
	 * See Documentation/x86/boot.txt for details no bzImage on-disk and
	 * memory layout.
	 */

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	if (read(fd_kernel, &boot, sizeof(boot)) != sizeof(boot))
		return false;

	if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)))
		return false;

	if (boot.hdr.version < BOOT_PROTOCOL_REQUIRED)
		die("Too old kernel");

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	if (!boot.hdr.setup_sects)
		boot.hdr.setup_sects = BZ_DEFAULT_SETUP_SECTS;
	setup_sects = boot.hdr.setup_sects + 1;

	setup_size = setup_sects << 9;
	p = guest_real_to_host(kvm, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);

	/* copy setup.bin to mem*/
	if (read(fd_kernel, p, setup_size) != setup_size)
		die_perror("read");

	/* copy vmlinux.bin to BZ_KERNEL_START*/
	p = guest_flat_to_host(kvm, BZ_KERNEL_START);

	while ((nr = read(fd_kernel, p, 65536)) > 0)
		p += nr;

	p = guest_flat_to_host(kvm, BOOT_CMDLINE_OFFSET);
	if (kernel_cmdline) {
		cmdline_size = strlen(kernel_cmdline) + 1;
		if (cmdline_size > boot.hdr.cmdline_size)
			cmdline_size = boot.hdr.cmdline_size;

		memset(p, 0, boot.hdr.cmdline_size);
		memcpy(p, kernel_cmdline, cmdline_size - 1);
	}

	kern_boot	= guest_real_to_host(kvm, BOOT_LOADER_SELECTOR, 0x00);

	kern_boot->hdr.cmd_line_ptr	= BOOT_CMDLINE_OFFSET;
	kern_boot->hdr.type_of_loader	= 0xff;
	kern_boot->hdr.heap_end_ptr	= 0xfe00;
	kern_boot->hdr.loadflags	|= CAN_USE_HEAP;
	kern_boot->hdr.vid_mode		= vidmode;

	/*
	 * Read initrd image into guest memory
	 */
	if (fd_initrd >= 0) {
		struct stat initrd_stat;
		unsigned long addr;

		if (fstat(fd_initrd, &initrd_stat))
			die_perror("fstat");

		addr = boot.hdr.initrd_addr_max & ~0xfffff;
		for (;;) {
			if (addr < BZ_KERNEL_START)
				die("Not enough memory for initrd");
			else if (addr < (kvm->ram_size - initrd_stat.st_size))
				break;
			addr -= 0x100000;
		}

		p = guest_flat_to_host(kvm, addr);
		nr = read(fd_initrd, p, initrd_stat.st_size);
		if (nr != initrd_stat.st_size)
			die("Failed to read initrd");

		kern_boot->hdr.ramdisk_image	= addr;
		kern_boot->hdr.ramdisk_size	= initrd_stat.st_size;
	}

	kvm->boot_selector	= BOOT_LOADER_SELECTOR;
	/*
	 * The real-mode setup code starts at offset 0x200 of a bzImage. See
	 * Documentation/x86/boot.txt for details.
	 */
	kvm->boot_ip		= BOOT_LOADER_IP + 0x200;
	kvm->boot_sp		= BOOT_LOADER_SP;

	return true;
}

/* RFC 1952 */
#define GZIP_ID1		0x1f
#define GZIP_ID2		0x8b

static bool initrd_check(int fd)
{
	unsigned char id[2];

	if (read_in_full(fd, id, ARRAY_SIZE(id)) < 0)
		return false;

	if (lseek(fd, 0, SEEK_SET) < 0)
		die_perror("lseek");

	return id[0] == GZIP_ID1 && id[1] == GZIP_ID2;
}

bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
		const char *initrd_filename, const char *kernel_cmdline, u16 vidmode)
{
	bool ret;
	int fd_kernel = -1, fd_initrd = -1;

	fd_kernel = open(kernel_filename, O_RDONLY);
	if (fd_kernel < 0)
		die("Unable to open kernel %s", kernel_filename);

	if (initrd_filename) {
		fd_initrd = open(initrd_filename, O_RDONLY);
		if (fd_initrd < 0)
			die("Unable to open initrd %s", initrd_filename);

		if (!initrd_check(fd_initrd))
			die("%s is not an initrd", initrd_filename);
	}

	ret = load_bzimage(kvm, fd_kernel, fd_initrd, kernel_cmdline, vidmode);

	if (initrd_filename)
		close(fd_initrd);

	if (ret)
		goto found_kernel;

	pr_warning("%s is not a bzImage. Trying to load it as a flat binary...", kernel_filename);

	ret = load_flat_binary(kvm, fd_kernel);
	if (ret)
		goto found_kernel;

	close(fd_kernel);

	die("%s is not a valid bzImage or flat binary", kernel_filename);

found_kernel:
	close(fd_kernel);

	return ret;
}

/**
 * kvm__setup_bios - inject BIOS into guest system memory
 * @kvm - guest system descriptor
 *
 * This function is a main routine where we poke guest memory
 * and install BIOS there.
 */
void kvm__setup_bios(struct kvm *kvm)
{
	/* standart minimal configuration */
	setup_bios(kvm);

	/* FIXME: SMP, ACPI and friends here */

	/* MP table */
	mptable_setup(kvm, kvm->nrcpus);
}

#define TIMER_INTERVAL_NS 1000000	/* 1 msec */

/*
 * This function sets up a timer that's used to inject interrupts from the
 * userspace hypervisor into the guest at periodical intervals. Please note
 * that clock interrupt, for example, is not handled here.
 */
void kvm__start_timer(struct kvm *kvm)
{
	struct itimerspec its;
	struct sigevent sev;

	memset(&sev, 0, sizeof(struct sigevent));
	sev.sigev_value.sival_int	= 0;
	sev.sigev_notify		= SIGEV_SIGNAL;
	sev.sigev_signo			= SIGALRM;

	if (timer_create(CLOCK_REALTIME, &sev, &kvm->timerid) < 0)
		die("timer_create()");

	its.it_value.tv_sec		= TIMER_INTERVAL_NS / 1000000000;
	its.it_value.tv_nsec		= TIMER_INTERVAL_NS % 1000000000;
	its.it_interval.tv_sec		= its.it_value.tv_sec;
	its.it_interval.tv_nsec		= its.it_value.tv_nsec;

	if (timer_settime(kvm->timerid, 0, &its, NULL) < 0)
		die("timer_settime()");
}

void kvm__stop_timer(struct kvm *kvm)
{
	if (kvm->timerid)
		if (timer_delete(kvm->timerid) < 0)
			die("timer_delete()");

	kvm->timerid = 0;
}

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	struct kvm_irq_level irq_level;

	irq_level	= (struct kvm_irq_level) {
		{
			.irq		= irq,
		},
		.level		= level,
	};

	if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
		die_perror("KVM_IRQ_LINE failed");
}

void kvm__dump_mem(struct kvm *kvm, unsigned long addr, unsigned long size)
{
	unsigned char *p;
	unsigned long n;

	size &= ~7; /* mod 8 */
	if (!size)
		return;

	p = guest_flat_to_host(kvm, addr);

	for (n = 0; n < size; n += 8) {
		if (!host_ptr_in_ram(kvm, p + n))
			break;

		printf("  0x%08lx: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			addr + n, p[n + 0], p[n + 1], p[n + 2], p[n + 3],
				  p[n + 4], p[n + 5], p[n + 6], p[n + 7]);
	}
}
