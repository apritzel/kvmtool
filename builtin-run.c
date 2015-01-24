#include "kvm/builtin-run.h"

#include "kvm/builtin-setup.h"
#include "kvm/virtio-balloon.h"
#include "kvm/virtio-console.h"
#include "kvm/parse-options.h"
#include "kvm/8250-serial.h"
#include "kvm/framebuffer.h"
#include "kvm/disk-image.h"
#include "kvm/threadpool.h"
#include "kvm/virtio-blk.h"
#include "kvm/virtio-net.h"
#include "kvm/virtio-rng.h"
#include "kvm/ioeventfd.h"
#include "kvm/virtio-9p.h"
#include "kvm/barrier.h"
#include "kvm/kvm-cpu.h"
#include "kvm/ioport.h"
#include "kvm/symbol.h"
#include "kvm/i8042.h"
#include "kvm/mutex.h"
#include "kvm/term.h"
#include "kvm/util.h"
#include "kvm/vesa.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/rtc.h"
#include "kvm/sdl.h"
#include "kvm/vnc.h"
#include "kvm/guest_compat.h"
#include "kvm/pci-shmem.h"
#include "kvm/kvm-ipc.h"

#include <linux/types.h>

#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#define DEFAULT_KVM_DEV		"/dev/kvm"
#define DEFAULT_CONSOLE		"serial"
#define DEFAULT_NETWORK		"user"
#define DEFAULT_HOST_ADDR	"192.168.33.1"
#define DEFAULT_GUEST_ADDR	"192.168.33.15"
#define DEFAULT_GUEST_MAC	"02:15:15:15:15:15"
#define DEFAULT_HOST_MAC	"02:01:01:01:01:01"
#define DEFAULT_SCRIPT		"none"

#define MB_SHIFT		(20)
#define KB_SHIFT		(10)
#define GB_SHIFT		(30)
#define MIN_RAM_SIZE_MB		(64ULL)
#define MIN_RAM_SIZE_BYTE	(MIN_RAM_SIZE_MB << MB_SHIFT)

struct kvm *kvm;
struct kvm_cpu *kvm_cpus[KVM_NR_CPUS];
__thread struct kvm_cpu *current_kvm_cpu;

static u64 ram_size;
static u8  image_count;
static u8 num_net_devices;
static bool virtio_rng;
static const char *kernel_cmdline;
static const char *kernel_filename;
static const char *vmlinux_filename;
static const char *initrd_filename;
static const char *image_filename[MAX_DISK_IMAGES];
static const char *console;
static const char *dev;
static const char *network;
static const char *host_ip;
static const char *guest_ip;
static const char *guest_mac;
static const char *host_mac;
static const char *script;
static const char *guest_name;
static struct virtio_net_params *net_params;
static bool single_step;
static bool readonly_image[MAX_DISK_IMAGES];
static bool vnc;
static bool sdl;
static bool balloon;
static bool using_rootfs;
static bool custom_rootfs;
static bool no_net;
static bool no_dhcp;
extern bool ioport_debug;
extern int  active_console;
extern int  debug_iodelay;

bool do_debug_print = false;

static int nrcpus;
static int vidmode = -1;

static const char * const run_usage[] = {
	"kvm run [<options>] [<kernel image>]",
	NULL
};

static int img_name_parser(const struct option *opt, const char *arg, int unset)
{
	char *sep;
	struct stat st;
	char path[PATH_MAX];

	if (stat(arg, &st) == 0 &&
	    S_ISDIR(st.st_mode)) {
		char tmp[PATH_MAX];

		if (realpath(arg, tmp) == 0 ||
		    virtio_9p__register(kvm, tmp, "/dev/root") < 0)
			die("Unable to initialize virtio 9p");
		using_rootfs = 1;
		return 0;
	}

	snprintf(path, PATH_MAX, "%s%s", kvm__get_dir(), arg);

	if (stat(path, &st) == 0 &&
	    S_ISDIR(st.st_mode)) {
		char tmp[PATH_MAX];

		if (realpath(path, tmp) == 0 ||
		    virtio_9p__register(kvm, tmp, "/dev/root") < 0)
			die("Unable to initialize virtio 9p");
		if (virtio_9p__register(kvm, "/", "hostfs") < 0)
			die("Unable to initialize virtio 9p");
		kvm_setup_resolv(arg);
		using_rootfs = custom_rootfs = 1;
		return 0;
	}

	if (image_count >= MAX_DISK_IMAGES)
		die("Currently only 4 images are supported");

	image_filename[image_count] = arg;
	sep = strstr(arg, ",");
	if (sep) {
		if (strcmp(sep + 1, "ro") == 0)
			readonly_image[image_count] = 1;
		*sep = 0;
	}

	image_count++;

	return 0;
}

static int virtio_9p_rootdir_parser(const struct option *opt, const char *arg, int unset)
{
	char *tag_name;
	char tmp[PATH_MAX];

	/*
	 * 9p dir can be of the form dirname,tag_name or
	 * just dirname. In the later case we use the
	 * default tag name
	 */
	tag_name = strstr(arg, ",");
	if (tag_name) {
		*tag_name = '\0';
		tag_name++;
	}
	if (realpath(arg, tmp)) {
		if (virtio_9p__register(kvm, tmp, tag_name) < 0)
			die("Unable to initialize virtio 9p");
	} else
		die("Failed resolving 9p path");
	return 0;
}

static int tty_parser(const struct option *opt, const char *arg, int unset)
{
	int tty = atoi(arg);

	term_set_tty(tty);

	return 0;
}

static inline void str_to_mac(const char *str, char *mac)
{
	sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		mac, mac+1, mac+2, mac+3, mac+4, mac+5);
}
static int set_net_param(struct virtio_net_params *p, const char *param,
				const char *val)
{
	if (strcmp(param, "guest_mac") == 0) {
		str_to_mac(val, p->guest_mac);
	} else if (strcmp(param, "mode") == 0) {
		if (!strncmp(val, "user", 4)) {
			int i;

			for (i = 0; i < num_net_devices; i++)
				if (net_params[i].mode == NET_MODE_USER)
					die("Only one usermode network device allowed at a time");
			p->mode = NET_MODE_USER;
		} else if (!strncmp(val, "tap", 3)) {
			p->mode = NET_MODE_TAP;
		} else if (!strncmp(val, "none", 4)) {
			no_net = 1;
			return -1;
		} else
			die("Unkown network mode %s, please use user, tap or none", network);
	} else if (strcmp(param, "script") == 0) {
		p->script = strdup(val);
	} else if (strcmp(param, "guest_ip") == 0) {
		p->guest_ip = strdup(val);
	} else if (strcmp(param, "host_ip") == 0) {
		p->host_ip = strdup(val);
	} else if (strcmp(param, "vhost") == 0) {
		p->vhost = atoi(val);
	}

	return 0;
}

static int netdev_parser(const struct option *opt, const char *arg, int unset)
{
	struct virtio_net_params p;
	char *buf = NULL, *cmd = NULL, *cur = NULL;
	bool on_cmd = true;

	if (arg) {
		buf = strdup(arg);
		if (buf == NULL)
			die("Failed allocating new net buffer");
		cur = strtok(buf, ",=");
	}

	p = (struct virtio_net_params) {
		.guest_ip	= DEFAULT_GUEST_ADDR,
		.host_ip	= DEFAULT_HOST_ADDR,
		.script		= DEFAULT_SCRIPT,
		.mode		= NET_MODE_TAP,
	};

	str_to_mac(DEFAULT_GUEST_MAC, p.guest_mac);
	p.guest_mac[5] += num_net_devices;

	while (cur) {
		if (on_cmd) {
			cmd = cur;
		} else {
			if (set_net_param(&p, cmd, cur) < 0)
				goto done;
		}
		on_cmd = !on_cmd;

		cur = strtok(NULL, ",=");
	};

	num_net_devices++;

	net_params = realloc(net_params, num_net_devices * sizeof(*net_params));
	if (net_params == NULL)
		die("Failed adding new network device");

	net_params[num_net_devices - 1] = p;

done:
	free(buf);
	return 0;
}

static int shmem_parser(const struct option *opt, const char *arg, int unset)
{
	const u64 default_size = SHMEM_DEFAULT_SIZE;
	const u64 default_phys_addr = SHMEM_DEFAULT_ADDR;
	const char *default_handle = SHMEM_DEFAULT_HANDLE;
	struct shmem_info *si = malloc(sizeof(struct shmem_info));
	u64 phys_addr;
	u64 size;
	char *handle = NULL;
	int create = 0;
	const char *p = arg;
	char *next;
	int base = 10;
	int verbose = 0;

	const int skip_pci = strlen("pci:");
	if (verbose)
		pr_info("shmem_parser(%p,%s,%d)", opt, arg, unset);
	/* parse out optional addr family */
	if (strcasestr(p, "pci:")) {
		p += skip_pci;
	} else if (strcasestr(p, "mem:")) {
		die("I can't add to E820 map yet.\n");
	}
	/* parse out physical addr */
	base = 10;
	if (strcasestr(p, "0x"))
		base = 16;
	phys_addr = strtoll(p, &next, base);
	if (next == p && phys_addr == 0) {
		pr_info("shmem: no physical addr specified, using default.");
		phys_addr = default_phys_addr;
	}
	if (*next != ':' && *next != '\0')
		die("shmem: unexpected chars after phys addr.\n");
	if (*next == '\0')
		p = next;
	else
		p = next + 1;
	/* parse out size */
	base = 10;
	if (strcasestr(p, "0x"))
		base = 16;
	size = strtoll(p, &next, base);
	if (next == p && size == 0) {
		pr_info("shmem: no size specified, using default.");
		size = default_size;
	}
	/* look for [KMGkmg][Bb]*  uses base 2. */
	int skip_B = 0;
	if (strspn(next, "KMGkmg")) {	/* might have a prefix */
		if (*(next + 1) == 'B' || *(next + 1) == 'b')
			skip_B = 1;
		switch (*next) {
		case 'K':
		case 'k':
			size = size << KB_SHIFT;
			break;
		case 'M':
		case 'm':
			size = size << MB_SHIFT;
			break;
		case 'G':
		case 'g':
			size = size << GB_SHIFT;
			break;
		default:
			die("shmem: bug in detecting size prefix.");
			break;
		}
		next += 1 + skip_B;
	}
	if (*next != ':' && *next != '\0') {
		die("shmem: unexpected chars after phys size. <%c><%c>\n",
		    *next, *p);
	}
	if (*next == '\0')
		p = next;
	else
		p = next + 1;
	/* parse out optional shmem handle */
	const int skip_handle = strlen("handle=");
	next = strcasestr(p, "handle=");
	if (*p && next) {
		if (p != next)
			die("unexpected chars before handle\n");
		p += skip_handle;
		next = strchrnul(p, ':');
		if (next - p) {
			handle = malloc(next - p + 1);
			strncpy(handle, p, next - p);
			handle[next - p] = '\0';	/* just in case. */
		}
		if (*next == '\0')
			p = next;
		else
			p = next + 1;
	}
	/* parse optional create flag to see if we should create shm seg. */
	if (*p && strcasestr(p, "create")) {
		create = 1;
		p += strlen("create");
	}
	if (*p != '\0')
		die("shmem: unexpected trailing chars\n");
	if (handle == NULL) {
		handle = malloc(strlen(default_handle) + 1);
		strcpy(handle, default_handle);
	}
	if (verbose) {
		pr_info("shmem: phys_addr = %llx", phys_addr);
		pr_info("shmem: size      = %llx", size);
		pr_info("shmem: handle    = %s", handle);
		pr_info("shmem: create    = %d", create);
	}

	si->phys_addr = phys_addr;
	si->size = size;
	si->handle = handle;
	si->create = create;
	pci_shmem__register_mem(si);	/* ownership of si, etc. passed on. */
	return 0;
}

static const struct option options[] = {
	OPT_GROUP("Basic options:"),
	OPT_STRING('\0', "name", &guest_name, "guest name",
			"A name for the guest"),
	OPT_INTEGER('c', "cpus", &nrcpus, "Number of CPUs"),
	OPT_U64('m', "mem", &ram_size, "Virtual machine memory size in MiB."),
	OPT_CALLBACK('\0', "shmem", NULL,
		     "[pci:]<addr>:<size>[:handle=<handle>][:create]",
		     "Share host shmem with guest via pci device",
		     shmem_parser),
	OPT_CALLBACK('d', "disk", NULL, "image or rootfs_dir", "Disk image or rootfs directory", img_name_parser),
	OPT_BOOLEAN('\0', "balloon", &balloon, "Enable virtio balloon"),
	OPT_BOOLEAN('\0', "vnc", &vnc, "Enable VNC framebuffer"),
	OPT_BOOLEAN('\0', "sdl", &sdl, "Enable SDL framebuffer"),
	OPT_BOOLEAN('\0', "rng", &virtio_rng, "Enable virtio Random Number Generator"),
	OPT_CALLBACK('\0', "9p", NULL, "dir_to_share,tag_name",
		     "Enable virtio 9p to share files between host and guest", virtio_9p_rootdir_parser),
	OPT_STRING('\0', "console", &console, "serial or virtio",
			"Console to use"),
	OPT_STRING('\0', "dev", &dev, "device_file", "KVM device file"),
	OPT_CALLBACK('\0', "tty", NULL, "tty id",
		     "Remap guest TTY into a pty on the host",
		     tty_parser),

	OPT_GROUP("Kernel options:"),
	OPT_STRING('k', "kernel", &kernel_filename, "kernel",
			"Kernel to boot in virtual machine"),
	OPT_STRING('i', "initrd", &initrd_filename, "initrd",
			"Initial RAM disk image"),
	OPT_STRING('p', "params", &kernel_cmdline, "params",
			"Kernel command line arguments"),

	OPT_GROUP("Networking options:"),
	OPT_CALLBACK_DEFAULT('n', "network", NULL, "network params",
		     "Create a new guest NIC",
		     netdev_parser, NULL),
	OPT_BOOLEAN('\0', "no-dhcp", &no_dhcp, "Disable kernel DHCP in rootfs mode"),

	OPT_GROUP("BIOS options:"),
	OPT_INTEGER('\0', "vidmode", &vidmode,
		    "Video mode"),

	OPT_GROUP("Debug options:"),
	OPT_BOOLEAN('\0', "debug", &do_debug_print,
			"Enable debug messages"),
	OPT_BOOLEAN('\0', "debug-single-step", &single_step,
			"Enable single stepping"),
	OPT_BOOLEAN('\0', "debug-ioport", &ioport_debug,
			"Enable ioport debugging"),
	OPT_INTEGER('\0', "debug-iodelay", &debug_iodelay,
			"Delay IO by millisecond"),
	OPT_END()
};

/*
 * Serialize debug printout so that the output of multiple vcpus does not
 * get mixed up:
 */
static int printout_done;

static void handle_sigusr1(int sig)
{
	struct kvm_cpu *cpu = current_kvm_cpu;
	int fd = kvm_cpu__get_debug_fd();

	if (!cpu)
		return;

	dprintf(fd, "\n #\n # vCPU #%ld's dump:\n #\n", cpu->cpu_id);
	kvm_cpu__show_registers(cpu);
	kvm_cpu__show_code(cpu);
	kvm_cpu__show_page_tables(cpu);
	fflush(stdout);
	printout_done = 1;
	mb();
}

/* Pause/resume the guest using SIGUSR2 */
static int is_paused;

static void handle_pause(int fd, u32 type, u32 len, u8 *msg)
{
	if (type == KVM_IPC_RESUME && is_paused)
		kvm__continue();
	else if (type == KVM_IPC_PAUSE && !is_paused)
		kvm__pause();
	else
		return;

	is_paused = !is_paused;
	pr_info("Guest %s\n", is_paused ? "paused" : "resumed");
}

static void handle_debug(int fd, u32 type, u32 len, u8 *msg)
{
	int i;

	for (i = 0; i < nrcpus; i++) {
		struct kvm_cpu *cpu = kvm_cpus[i];

		if (!cpu)
			continue;

		printout_done = 0;

		kvm_cpu__set_debug_fd(fd);
		pthread_kill(cpu->thread, SIGUSR1);
		/*
		 * Wait for the vCPU to dump state before signalling
		 * the next thread. Since this is debug code it does
		 * not matter that we are burning CPU time a bit:
		 */
		while (!printout_done)
			mb();
	}

	close(fd);

	serial8250__inject_sysrq(kvm);
}

static void handle_sigalrm(int sig)
{
	serial8250__inject_interrupt(kvm);
	virtio_console__inject_interrupt(kvm);
}

static void handle_stop(int fd, u32 type, u32 len, u8 *msg)
{
	kvm_cpu__reboot();
}

static void *kvm_cpu_thread(void *arg)
{
	current_kvm_cpu		= arg;

	if (kvm_cpu__start(current_kvm_cpu))
		goto panic_kvm;

	kvm_cpu__delete(current_kvm_cpu);

	return (void *) (intptr_t) 0;

panic_kvm:
	fprintf(stderr, "KVM exit reason: %u (\"%s\")\n",
		current_kvm_cpu->kvm_run->exit_reason,
		kvm_exit_reasons[current_kvm_cpu->kvm_run->exit_reason]);
	if (current_kvm_cpu->kvm_run->exit_reason == KVM_EXIT_UNKNOWN)
		fprintf(stderr, "KVM exit code: 0x%Lu\n",
			current_kvm_cpu->kvm_run->hw.hardware_exit_reason);

	kvm_cpu__set_debug_fd(STDOUT_FILENO);
	kvm_cpu__show_registers(current_kvm_cpu);
	kvm_cpu__show_code(current_kvm_cpu);
	kvm_cpu__show_page_tables(current_kvm_cpu);

	kvm_cpu__delete(current_kvm_cpu);

	return (void *) (intptr_t) 1;
}

static char kernel[PATH_MAX];

static const char *host_kernels[] = {
	"/boot/vmlinuz",
	"/boot/bzImage",
	NULL
};

static const char *default_kernels[] = {
	"./bzImage",
	"../../arch/x86/boot/bzImage",
	NULL
};

static const char *default_vmlinux[] = {
	"../../../vmlinux",
	"../../vmlinux",
	NULL
};

static void kernel_usage_with_options(void)
{
	const char **k;
	struct utsname uts;

	fprintf(stderr, "Fatal: could not find default kernel image in:\n");
	k = &default_kernels[0];
	while (*k) {
		fprintf(stderr, "\t%s\n", *k);
		k++;
	}

	if (uname(&uts) < 0)
		return;

	k = &host_kernels[0];
	while (*k) {
		if (snprintf(kernel, PATH_MAX, "%s-%s", *k, uts.release) < 0)
			return;
		fprintf(stderr, "\t%s\n", kernel);
		k++;
	}
	fprintf(stderr, "\nPlease see 'kvm run --help' for more options.\n\n");
}

static u64 host_ram_size(void)
{
	long page_size;
	long nr_pages;

	nr_pages	= sysconf(_SC_PHYS_PAGES);
	if (nr_pages < 0) {
		pr_warning("sysconf(_SC_PHYS_PAGES) failed");
		return 0;
	}

	page_size	= sysconf(_SC_PAGE_SIZE);
	if (page_size < 0) {
		pr_warning("sysconf(_SC_PAGE_SIZE) failed");
		return 0;
	}

	return (nr_pages * page_size) >> MB_SHIFT;
}

/*
 * If user didn't specify how much memory it wants to allocate for the guest,
 * avoid filling the whole host RAM.
 */
#define RAM_SIZE_RATIO		0.8

static u64 get_ram_size(int nr_cpus)
{
	u64 available;
	u64 ram_size;

	ram_size	= 64 * (nr_cpus + 3);

	available	= host_ram_size() * RAM_SIZE_RATIO;
	if (!available)
		available = MIN_RAM_SIZE_MB;

	if (ram_size > available)
		ram_size	= available;

	return ram_size;
}

static const char *find_kernel(void)
{
	const char **k;
	struct stat st;
	struct utsname uts;

	k = &default_kernels[0];
	while (*k) {
		if (stat(*k, &st) < 0 || !S_ISREG(st.st_mode)) {
			k++;
			continue;
		}
		strncpy(kernel, *k, PATH_MAX);
		return kernel;
	}

	if (uname(&uts) < 0)
		return NULL;

	k = &host_kernels[0];
	while (*k) {
		if (snprintf(kernel, PATH_MAX, "%s-%s", *k, uts.release) < 0)
			return NULL;

		if (stat(kernel, &st) < 0 || !S_ISREG(st.st_mode)) {
			k++;
			continue;
		}
		return kernel;

	}
	return NULL;
}

static const char *find_vmlinux(void)
{
	const char **vmlinux;

	vmlinux = &default_vmlinux[0];
	while (*vmlinux) {
		struct stat st;

		if (stat(*vmlinux, &st) < 0 || !S_ISREG(st.st_mode)) {
			vmlinux++;
			continue;
		}
		return *vmlinux;
	}
	return NULL;
}

void kvm_run_help(void)
{
	usage_with_options(run_usage, options);
}

int kvm_cmd_run(int argc, const char **argv, const char *prefix)
{
	static char real_cmdline[2048], default_name[20];
	struct framebuffer *fb = NULL;
	unsigned int nr_online_cpus;
	int exit_code = 0;
	int max_cpus, recommended_cpus;
	int i;
	void *ret;

	signal(SIGALRM, handle_sigalrm);
	kvm_ipc__register_handler(KVM_IPC_DEBUG, handle_debug);
	signal(SIGUSR1, handle_sigusr1);
	kvm_ipc__register_handler(KVM_IPC_PAUSE, handle_pause);
	kvm_ipc__register_handler(KVM_IPC_RESUME, handle_pause);
	kvm_ipc__register_handler(KVM_IPC_STOP, handle_stop);

	nr_online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	while (argc != 0) {
		argc = parse_options(argc, argv, options, run_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0) {
			if (kernel_filename) {
				fprintf(stderr, "Cannot handle parameter: "
						"%s\n", argv[0]);
				usage_with_options(run_usage, options);
				return EINVAL;
			}
			/* first unhandled parameter is treated as a kernel
			   image
			 */
			kernel_filename = argv[0];
			argv++;
			argc--;
		}

	}

	if (!kernel_filename)
		kernel_filename = find_kernel();

	if (!kernel_filename) {
		kernel_usage_with_options();
		return EINVAL;
	}

	vmlinux_filename = find_vmlinux();

	if (nrcpus == 0)
		nrcpus = nr_online_cpus;
	else if (nrcpus < 1 || nrcpus > KVM_NR_CPUS)
		die("Number of CPUs %d is out of [1;%d] range", nrcpus, KVM_NR_CPUS);

	if (!ram_size)
		ram_size	= get_ram_size(nrcpus);

	if (ram_size < MIN_RAM_SIZE_MB)
		die("Not enough memory specified: %lluMB (min %lluMB)", ram_size, MIN_RAM_SIZE_MB);

	if (ram_size > host_ram_size())
		pr_warning("Guest memory size %lluMB exceeds host physical RAM size %lluMB", ram_size, host_ram_size());

	ram_size <<= MB_SHIFT;

	if (!dev)
		dev = DEFAULT_KVM_DEV;

	if (!console)
		console = DEFAULT_CONSOLE;

	if (!strncmp(console, "virtio", 6))
		active_console  = CONSOLE_VIRTIO;
	else
		active_console  = CONSOLE_8250;

	if (!host_ip)
		host_ip = DEFAULT_HOST_ADDR;

	if (!guest_ip)
		guest_ip = DEFAULT_GUEST_ADDR;

	if (!guest_mac)
		guest_mac = DEFAULT_GUEST_MAC;

	if (!host_mac)
		host_mac = DEFAULT_HOST_MAC;

	if (!script)
		script = DEFAULT_SCRIPT;

	symbol__init(vmlinux_filename);

	term_init();

	if (!guest_name) {
		sprintf(default_name, "guest-%u", getpid());
		guest_name = default_name;
	}

	kvm = kvm__init(dev, ram_size, guest_name);

	irq__init(kvm);

	kvm->single_step = single_step;

	ioeventfd__init();

	max_cpus = kvm__max_cpus(kvm);
	recommended_cpus = kvm__recommended_cpus(kvm);

	if (nrcpus > max_cpus) {
		printf("  # Limit the number of CPUs to %d\n", max_cpus);
		kvm->nrcpus	= max_cpus;
	} else if (nrcpus > recommended_cpus) {
		printf("  # Warning: The maximum recommended amount of VCPUs"
			" is %d\n", recommended_cpus);
	}

	kvm->nrcpus = nrcpus;

	/*
	 * vidmode should be either specified
	 * either set by default
	 */
	if (vnc || sdl) {
		if (vidmode == -1)
			vidmode = 0x312;
	} else
		vidmode = 0;

	memset(real_cmdline, 0, sizeof(real_cmdline));
	strcpy(real_cmdline, "noapic noacpi pci=conf1 reboot=k panic=1 i8042.direct=1 "
				"i8042.dumbkbd=1 i8042.nopnp=1");
	if (vnc || sdl) {
		strcat(real_cmdline, " video=vesafb console=tty0");
	} else
		strcat(real_cmdline, " console=ttyS0 earlyprintk=serial i8042.noaux=1");
	strcat(real_cmdline, " ");
	if (kernel_cmdline)
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));

	if (!using_rootfs && !image_filename[0]) {
		char tmp[PATH_MAX];

		kvm_setup_create_new("default");
		kvm_setup_resolv("default");

		snprintf(tmp, PATH_MAX, "%s%s", kvm__get_dir(), "default");
		if (virtio_9p__register(kvm, tmp, "/dev/root") < 0)
			die("Unable to initialize virtio 9p");
		if (virtio_9p__register(kvm, "/", "hostfs") < 0)
			die("Unable to initialize virtio 9p");
		using_rootfs = custom_rootfs = 1;

		if (!strstr(real_cmdline, "init="))
			strlcat(real_cmdline, " init=/bin/sh ", sizeof(real_cmdline));
	}

	if (using_rootfs) {
		strcat(real_cmdline, " root=/dev/root rw rootflags=rw,trans=virtio,version=9p2000.L rootfstype=9p");
		if (custom_rootfs) {
			strcat(real_cmdline, " init=/virt/init");
			if (!no_dhcp)
				strcat(real_cmdline, "  ip=dhcp");
		}
	} else if (!strstr(real_cmdline, "root=")) {
		strlcat(real_cmdline, " root=/dev/vda rw ", sizeof(real_cmdline));
	}

	if (image_count) {
		kvm->nr_disks = image_count;
		kvm->disks    = disk_image__open_all(image_filename, readonly_image, image_count);
		if (!kvm->disks)
			die("Unable to load all disk images.");

		virtio_blk__init_all(kvm);
	}

	printf("  # kvm run -k %s -m %Lu -c %d --name %s\n", kernel_filename, ram_size / 1024 / 1024, nrcpus, guest_name);

	if (!kvm__load_kernel(kvm, kernel_filename, initrd_filename,
				real_cmdline, vidmode))
		die("unable to load kernel %s", kernel_filename);

	kvm->vmlinux		= vmlinux_filename;

	ioport__setup_legacy();

	rtc__init();

	serial8250__init(kvm);

	pci__init();

	if (active_console == CONSOLE_VIRTIO)
		virtio_console__init(kvm);

	if (virtio_rng)
		virtio_rng__init(kvm);

	if (balloon)
		virtio_bln__init(kvm);

	if (!network)
		network = DEFAULT_NETWORK;

	virtio_9p__init(kvm);

	for (i = 0; i < num_net_devices; i++) {
		net_params[i].kvm = kvm;
		virtio_net__init(&net_params[i]);
	}

	if (num_net_devices == 0 && no_net == 0) {
		struct virtio_net_params net_params;

		net_params = (struct virtio_net_params) {
			.guest_ip	= guest_ip,
			.host_ip	= host_ip,
			.kvm		= kvm,
			.script		= script,
			.mode		= NET_MODE_USER,
		};
		str_to_mac(guest_mac, net_params.guest_mac);
		str_to_mac(host_mac, net_params.host_mac);

		virtio_net__init(&net_params);
	}

	kvm__start_timer(kvm);

	kvm__setup_bios(kvm);

	for (i = 0; i < nrcpus; i++) {
		kvm_cpus[i] = kvm_cpu__init(kvm, i);
		if (!kvm_cpus[i])
			die("unable to initialize KVM VCPU");
	}

	kvm__init_ram(kvm);

	kbd__init(kvm);

	pci_shmem__init(kvm);

	if (vnc || sdl)
		fb = vesa__init(kvm);

	if (vnc) {
		if (fb)
			vnc__init(fb);
	}

	if (sdl) {
		if (fb)
			sdl__init(fb);
	}

	fb__start();

	thread_pool__init(nr_online_cpus);
	ioeventfd__start();

	for (i = 0; i < nrcpus; i++) {
		if (pthread_create(&kvm_cpus[i]->thread, NULL, kvm_cpu_thread, kvm_cpus[i]) != 0)
			die("unable to create KVM VCPU thread");
	}

	/* Only VCPU #0 is going to exit by itself when shutting down */
	if (pthread_join(kvm_cpus[0]->thread, &ret) != 0)
		exit_code = 1;

	for (i = 1; i < nrcpus; i++) {
		if (kvm_cpus[i]->is_running) {
			pthread_kill(kvm_cpus[i]->thread, SIGKVMEXIT);
			if (pthread_join(kvm_cpus[i]->thread, &ret) != 0)
				die("pthread_join");
		}
		if (ret != NULL)
			exit_code = 1;
	}

	compat__print_all_messages();

	fb__stop();

	virtio_blk__delete_all(kvm);
	virtio_rng__delete_all(kvm);

	disk_image__close_all(kvm->disks, image_count);
	kvm__delete(kvm);

	if (!exit_code)
		printf("\n  # KVM session ended normally.\n");

	return exit_code;
}
