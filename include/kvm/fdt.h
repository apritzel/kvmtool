#ifndef KVM__FDT_H
#define KVM__FDT_H

#include "libfdt.h"

#include <linux/types.h>

#define FDT_MAX_SIZE	0x10000

/* Those definitions are generic FDT values for specifying IRQ
 * types and are used in the Linux kernel internally as well as in
 * the dts files and their documentation.
 */
enum irq_type {
	IRQ_TYPE_NONE		= 0x00000000,
	IRQ_TYPE_EDGE_RISING	= 0x00000001,
	IRQ_TYPE_EDGE_FALLING	= 0x00000002,
	IRQ_TYPE_EDGE_BOTH	= (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING),
	IRQ_TYPE_LEVEL_HIGH	= 0x00000004,
	IRQ_TYPE_LEVEL_LOW	= 0x00000008,
	IRQ_TYPE_LEVEL_MASK	= (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH),
};

/* Helper for the various bits of code that generate FDT nodes */
#define _FDT(exp)							\
	do {								\
		int ret = (exp);					\
		if (ret < 0) {						\
			die("Error creating device tree: %s: %s\n",	\
			    #exp, fdt_strerror(ret));			\
		}							\
	} while (0)

static inline u32 fdt__alloc_phandle(void)
{
	static u32 phandle = 0;
	return ++phandle;
}

#endif /* KVM__FDT_H */
