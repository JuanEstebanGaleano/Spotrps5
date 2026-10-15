// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
#include "linux_spotrps5.h"
#include "linux_abi.h"
#include "linux_files.h"
#include "linux_net.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *base;
    size_t length;
} LinuxIovec;
typedef struct {
    void *name;
    uint32_t name_length, pad;
    LinuxIovec *iov;
    size_t iov_count;
    void *control;
    size_t control_length;
    int flags, pad2;
} LinuxMsghdr;  // glibc x86-64 struct msghdr, 56 bytes
_Static_assert(sizeof(LinuxMsghdr) == 56 && sizeof(LinuxIovec) == 16, "glibc x86-64 msghdr");
#define MESSAGE_MAX (256u * 1024u)
enum { LINUX_EMSGSIZE = 90, LINUX_ENODEV = 19 };  // errno values not in linux_abi.h

static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}
static size_t iovTotal(const LinuxIovec *iov, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (iov[i].length > MESSAGE_MAX - total) return MESSAGE_MAX + 1;
        total += iov[i].length;
    }
    return total;
}

int64_t linuxAbiSendmsg(int fd, const void *message, int flags) {
    const LinuxMsghdr *m = message;
    if (!m || (m->iov_count && !m->iov)) return fail(LINUX_EFAULT);
    size_t total = iovTotal(m->iov, m->iov_count);
    if (total > MESSAGE_MAX) return fail(LINUX_EMSGSIZE);
    unsigned char *buffer = malloc(total ? total : 1);
    if (!buffer) return fail(LINUX_ENOMEM);
    size_t at = 0;
    for (size_t i = 0; i < m->iov_count; ++i) {
        memcpy(buffer + at, m->iov[i].base, m->iov[i].length);
        at += m->iov[i].length;
    }
    int64_t sent = linuxAbiSendto(fd, buffer, total, flags, m->name, m->name_length);
    free(buffer);
    return sent;
}
int64_t linuxAbiRecvmsg(int fd, void *message, int flags) {
    LinuxMsghdr *m = message;
    if (!m || (m->iov_count && !m->iov)) return fail(LINUX_EFAULT);
    size_t total = iovTotal(m->iov, m->iov_count);
    if (total > MESSAGE_MAX) total = MESSAGE_MAX;
    unsigned char *buffer = malloc(total ? total : 1);
    if (!buffer) return fail(LINUX_ENOMEM);
    unsigned address_length = m->name ? m->name_length : 0;
    int64_t got = linuxAbiRecvfrom(fd, buffer, total, flags, m->name, m->name ? &address_length : NULL);
    if (got >= 0) {
        size_t left = (size_t)got, at = 0;
        for (size_t i = 0; i < m->iov_count && left; ++i) {
            size_t take = left < m->iov[i].length ? left : m->iov[i].length;
            memcpy(m->iov[i].base, buffer + at, take);
            at += take;
            left -= take;
        }
        if (m->name) m->name_length = address_length;
        m->control_length = 0;  // no ancillary data (packet info, timestamps)
        m->flags = 0;
    }
    free(buffer);
    return got;
}

// readv / writev: one call per vector.
int64_t linuxAbiReadv(int fd, const void *vectors, int count) {
    const LinuxIovec *iov = vectors;
    if (count < 0 || (count && !iov)) return fail(LINUX_EINVAL);
    int64_t total = 0;
    for (int i = 0; i < count; ++i) {
        if (!iov[i].length) continue;
        int64_t got = linuxAbiRead(fd, iov[i].base, iov[i].length);
        if (got < 0) return total ? total : -1;
        total += got;
        if ((size_t)got < iov[i].length) break;
    }
    return total;
}
int64_t linuxAbiWritev(int fd, const void *vectors, int count) {
    const LinuxIovec *iov = vectors;
    if (count < 0 || (count && !iov)) return fail(LINUX_EINVAL);
    int64_t total = 0;
    for (int i = 0; i < count; ++i) {
        if (!iov[i].length) continue;
        int64_t put = linuxAbiWrite(fd, iov[i].base, iov[i].length);
        if (put < 0) return total ? total : -1;
        total += put;
        if ((size_t)put < iov[i].length) break;
    }
    return total;
}

// ---- network interfaces ------------------------------------------------------------------------------------------------------------------
// One interface, "eth0", with the console's IPv4 address: enough for mDNS to join its multicast group.
typedef struct {
    void *next;
    char *name;
    unsigned flags, pad;
    void *address, *netmask, *broadcast, *data;
} LinuxIfaddrs;  // glibc struct ifaddrs, 56 bytes
typedef struct {
    uint16_t family, port;
    uint8_t address[4];
    uint8_t zero[8];
} LinuxSockaddrIn;
_Static_assert(sizeof(LinuxIfaddrs) == 56 && sizeof(LinuxSockaddrIn) == 16, "glibc x86-64 ifaddrs / sockaddr_in");
typedef struct {
    LinuxIfaddrs entry;
    char name[8];
    LinuxSockaddrIn address, netmask, broadcast;
} InterfaceBlock;
static bool parseIPv4(const char *text, uint8_t out[4]) {
    for (int part = 0; part < 4; ++part) {
        if (*text < '0' || *text > '9') return false;
        unsigned value = 0;
        while (*text >= '0' && *text <= '9') value = value * 10 + (unsigned)(*text++ - '0');
        if (value > 255 || (part < 3 && *text++ != '.')) return false;
        out[part] = (uint8_t)value;
    }
    return !*text;
}
int linuxAbiGetifaddrs(void **result) {
    char text[16];
    uint8_t ip[4];
    if (!result) return fail(LINUX_EFAULT);
    if (!platformLocalIPv4(text) || !parseIPv4(text, ip)) return fail(LINUX_ENODEV);
    InterfaceBlock *block = calloc(1, sizeof(*block));
    if (!block) return fail(LINUX_ENOMEM);
    strcpy(block->name, "eth0");
    block->address = (LinuxSockaddrIn){.family = 2};
    memcpy(block->address.address, ip, 4);
    block->netmask = (LinuxSockaddrIn){.family = 2, .address = {255, 255, 255, 0}};
    block->broadcast = (LinuxSockaddrIn){.family = 2, .address = {ip[0], ip[1], ip[2], 255}};
    block->entry = (LinuxIfaddrs){.name = block->name, .flags = 0x1043 /* UP | BROADCAST | RUNNING | MULTICAST */, .address = &block->address,
                                  .netmask = &block->netmask, .broadcast = &block->broadcast};
    *result = block;
    return 0;
}
void linuxAbiFreeifaddrs(void *list) { free(list); }
unsigned linuxAbiIfNameToIndex(const char *name) {
    if (name && !strcmp(name, "eth0")) return 2;
    *linuxAbiErrnoLocation() = LINUX_ENODEV;
    return 0;
}

// ---- libgcc_s unwinder: panic=abort never unwinds, but std still references these; report "no frames" ----------------------------
#define URC_END_OF_STACK 5
int linuxUnwindBacktrace(void *callback, void *argument) {
    (void)callback;
    (void)argument;
    return URC_END_OF_STACK;
}
int linuxUnwindRaise(void *exception) {
    (void)exception;
    return URC_END_OF_STACK;
}
void linuxUnwindResume(void *exception) {
    (void)exception;
    linuxRuntimeAbort();
}
void linuxUnwindDelete(void *exception) { (void)exception; }
uintptr_t linuxUnwindZero(void *context) {
    (void)context;
    return 0;
}
uintptr_t linuxUnwindGetIPInfo(void *context, int *before) {
    (void)context;
    if (before) *before = 0;
    return 0;
}
