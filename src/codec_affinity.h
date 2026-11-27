/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
#include <sched.h>

/* Restore saved with sched_setaffinity(0, ...) when a change succeeds. */
gboolean line_codec_local_cpus(cpu_set_t *saved, guint threads);
