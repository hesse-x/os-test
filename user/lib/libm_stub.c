/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libm_stub — empty. libm.so is a stub .so: math implementation lives in libc
// (musl_math_objs merged into c/c_so). This object exists only so the link has
// an input (ld rejects "no input files"); it defines nothing. -lm resolves at
// link time against this .so, runtime math comes from libc.so. See
// user/CMakeLists.txt "musl single-libc philosophy".
