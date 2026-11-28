/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AVERNELLA_SECURITY_LIFF_FILTER_H
#define AVERNELLA_SECURITY_LIFF_FILTER_H

/*
 * Apply before starting WebKit. The filter covers existing threads and
 * is inherited by children. Returns 0 on success or -errno on failure.
 */
int line_liff_filter_apply(void);

#endif /* AVERNELLA_SECURITY_LIFF_FILTER_H */
