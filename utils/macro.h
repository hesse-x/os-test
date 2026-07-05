/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MACRO_H
#define MACRO_H

#define ALIGN_UP(val, aligned) ((val + aligned - 1) & ~(aligned - 1))
#define ALIGN_DOWN(val, aligned) ((val) & ~(aligned - 1))

#endif // MACRO_H
