/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETD_SNTP_EXCHANGE_H
#define XOS_NETD_SNTP_EXCHANGE_H

#include "control_protocol.h"
#include "diagnostics.h"

int netd_sntp4_exchange(struct netd_diag_context *ctx,
                        const struct netd_sntp4_request *request,
                        struct netd_sntp4_result *result);

#endif
