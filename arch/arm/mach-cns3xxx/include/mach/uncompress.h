/*
 * Copyright 2003 ARM Limited
 * Copyright 2008 Cavium Networks
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, Version 2, as
 * published by the Free Software Foundation.
 */

#include <asm/mach-types.h>
#include <mach/cns3xxx.h>

#if (0)
#define AMBA_UART_DR(base)	(*(volatile unsigned char *)((base) + 0x00))
#define AMBA_UART_LCRH(base)	(*(volatile unsigned char *)((base) + 0x2c))
#define AMBA_UART_CR(base)	(*(volatile unsigned char *)((base) + 0x30))
#define AMBA_UART_FR(base)	(*(volatile unsigned char *)((base) + 0x18))
#else

#define AMBA_UART_DR(base)	(*(volatile unsigned long *)((base) + 0x00))
#define AMBA_UART_LCRH(base)	(*(volatile unsigned long *)((base) + 0x0c))
#define AMBA_UART_CR(base)	(*(volatile unsigned long *)((base) + 0x10))
#define AMBA_UART_LSR(base)	(*(volatile unsigned long *)((base) + 0x14))
#endif

/*
 * Return the UART base address
 */
static inline unsigned long get_uart_base(void)
{
	if (machine_is_cns3420vb() || machine_is_ts43xx())
		return CNS3XXX_UART0_BASE;
	else
		return 0;
}

/*
 * This does not append a newline
 */
static inline void putc(int c)
{
	unsigned long base = get_uart_base();

	while ((AMBA_UART_LSR(base) & (1 << 5)) == 0)
		barrier();

	AMBA_UART_DR(base) = c;
}

static inline void flush(void)
{
	unsigned long base = get_uart_base();

	while ((AMBA_UART_LSR(base) & (1 << 5)) == 0)
		barrier();
}

/*
 * nothing to do
 */
#define arch_decomp_setup()
#define arch_decomp_wdog()
