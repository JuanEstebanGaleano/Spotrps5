// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
//
// Linux ABI additions for programs written in Rust (librespot): scatter/gather socket messages, the list of network interfaces
// (mDNS needs it) and a few small functions the existing tables did not have.
#pragma once
#include <stddef.h>
#include <stdint.h>

int64_t linuxAbiSendmsg(int fd, const void *message, int flags);
int64_t linuxAbiRecvmsg(int fd, void *message, int flags);
int linuxAbiGetifaddrs(void **result);
void linuxAbiFreeifaddrs(void *list);
unsigned linuxAbiIfNameToIndex(const char *name);
int64_t linuxAbiReadv(int fd, const void *iov, int count);
int64_t linuxAbiWritev(int fd, const void *iov, int count);
