// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
//
// The screens: library, home, playlists, search with an on-screen keyboard, and the playing bar, driven by the controller.
#pragma once
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>

void uiInit(void);
// One step: the controller's state (read once per frame), the time, and the answers the Web API has for us.
void uiUpdate(const PlatformPad *pad, uint64_t now_ns);
// Draws the whole screen (between overlayBegin and overlayEnd); `now_ns` as for uiUpdate.
void uiDraw(uint64_t now_ns);
bool uiReady(void);          // librespot has a session and the Web API answers: the screens are usable
bool uiWantsQuit(void);      // the person asked to leave (hold Circle on the home screen)
// What the title has seen of librespot, for the status line when something is wrong.
void uiSetDiagnostics(const char *line);
