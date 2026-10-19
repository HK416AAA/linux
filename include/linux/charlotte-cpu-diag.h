/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CHARLOTTE_CPU_DIAG_H
#define _CHARLOTTE_CPU_DIAG_H
#include <linux/compiler.h>
#ifdef CONFIG_CHARLOTTE_BBOXLOG
void noinstr charlotte_cpu_diag(unsigned long stage, unsigned long a, unsigned long b);
void charlotte_cpu_diag_dump(void);
#else
static inline void charlotte_cpu_diag(unsigned long s, unsigned long a, unsigned long b) {}
static inline void charlotte_cpu_diag_dump(void) {}
#endif
#endif
