/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

void line_shared_clear(Line *s);
J *line_shared_content(Line *s, const char *method, J *params, GError **error);
GBytes *line_shared_photo(Line *s, J *params, GError **error);
