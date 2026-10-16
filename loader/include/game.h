// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/game.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the platform passes the folders and arguments; no Switch applet handling.
#pragma once
#include "linux_dl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *root;         // native folder the client sees as "/" (its install is /game, temporary files /tmp, home /home/ps5)
    const char *client_path;  // native path of bin/linux/x64/PokeMMO inside root (the loader maps the file)
    struct {
        const char *guest, *native;
    } mounts[4];              // optional extra folders, e.g. {"/game/roms", "/app0/roms"}
    unsigned mount_count;
    const char *const *arguments;  // the client's argv after the program name (JVM options), NULL-terminated
    unsigned timeout_seconds;      // 0: wait for ever
    const LinuxVirtualLibrary *virtual_libraries;  // SDL3, EGL, GLX, OpenAL, GTK (PS5); none on a PC
    unsigned virtual_count;
    // Spotrps5: what the program writes to stdout (librespot's pipe backend writes raw PCM there). NULL: stdout is logged as text.
    int64_t (*stdout_sink)(void *context, const void *bytes, size_t count);
    void *stdout_context;
} GameConfig;

// Runs the official PokeMMO client (its `main`) inside the adapters and waits for it. True when main returned 0 or exit(0) was called.
bool gameRun(const GameConfig *config);
const char *gameFailure(void);  // a short sentence, valid after gameRun returned false
unsigned gameRecentCount(void);            // lines kept for the screen: the program's last stderr lines, refused imports, how it ended
const char *gameRecentLine(unsigned index);  // 0 is the oldest
