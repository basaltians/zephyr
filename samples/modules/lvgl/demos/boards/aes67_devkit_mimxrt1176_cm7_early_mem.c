/*
 * Copyright (c) Basalte bv
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>
#include <zephyr/arch/common/init.h>
#include <zephyr/linker/section_tags.h>

/*
 * memcpy() and memset() run from the ITCM, which arch_data_copy() only fills after arch_bss_zero()
 * and the stack init in reset.S ran. The weak defaults of these call memcpy() and memset(). The
 * volatile accesses keep the compiler from turning the loops back into those calls.
 */

__boot_func void arch_early_memset(void *dst, int c, size_t n)
{
	volatile uint8_t *d = dst;
	uint32_t word = 0x01010101U * (uint8_t)c;

	if (((uintptr_t)d & 0x3U) == 0U) {
		for (; n >= 4U; n -= 4U, d += 4) {
			*(volatile uint32_t *)d = word;
		}
	}

	for (; n > 0U; n--) {
		*d++ = (uint8_t)c;
	}
}

__boot_func void arch_early_memcpy(void *dst, const void *src, size_t n)
{
	volatile uint8_t *d = dst;
	const volatile uint8_t *s = src;

	if ((((uintptr_t)d | (uintptr_t)s) & 0x3U) == 0U) {
		for (; n >= 4U; n -= 4U, d += 4, s += 4) {
			*(volatile uint32_t *)d = *(const volatile uint32_t *)s;
		}
	}

	for (; n > 0U; n--) {
		*d++ = *s++;
	}
}
