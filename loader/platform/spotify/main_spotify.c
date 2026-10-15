// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
//
// The Spotrps5 title, milestone 1: its own title (own id, own name), no PokeMMO parts. It opens the screen, shows the
// state of the title and the console's address, and logs over UDP (port 18194, tools/udplog.py). librespot and its audio
// come in the next milestones (docs/spotify-plan.md).
#include "diagnostics.h"
#include "overlay.h"
#include "platform.h"
#include "prospero_version.h"  // PROSPERO_RELEASE, PROSPERO_TITLE_ID (generated at build time)
#include <EGL/egl.h>
#include <GL/gl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

// ps5-opengl's app heap (malloc of the loader and of everything the program allocates with malloc): from direct memory.
const size_t ps5_opengl_heap_size = 768u << 20;

int sceKernelUsleep(unsigned int microseconds);

#define MILESTONE "spotrps5-1"

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext screen_context = EGL_NO_CONTEXT;
static EGLint width, height;

static bool screenOpen(void) {
    static const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0, major = 0, minor = 0;
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        return false;
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    screen_context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (surface == EGL_NO_SURFACE || screen_context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, screen_context)) return false;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    return true;
}

static void drawScreen(const char *address, unsigned frame) {
    if (!overlayBegin((unsigned)width, (unsigned)height)) {  // no overlay: a plain colour so that something shows
        glViewport(0, 0, width, height);
        glClearColor(0.07f, 0.45f, 0.20f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    overlayClear(0x0B1A12FF);
    overlayRect(0, 0, OVERLAY_WIDTH, 8, 0x1DB954FF);
    overlayTextCentered(OVERLAY_WIDTH / 2, 330, "Spotrps5", 96, 0x1DB954FF);
    overlayTextCentered(OVERLAY_WIDTH / 2, 450, "Spotify Connect para PS5", 40, 0xFFFFFFFF);
    static const char *const dots[] = {"", ".", "..", "..."};
    char line[160];
    snprintf(line, sizeof(line), "Hito 1: el titulo arranca%s", dots[(frame / 30) % 4]);
    overlayTextCentered(OVERLAY_WIDTH / 2, 560, line, 36, 0xE8E8E8FF);
    overlayTextCentered(OVERLAY_WIDTH / 2, 620, "librespot todavia no esta incluido", 30, 0xA0A0A0FF);
    snprintf(line, sizeof(line), "IP de la consola: %s", address);
    overlayTextCentered(OVERLAY_WIDTH / 2, 760, line, 30, 0xA0A0A0FF);
    snprintf(line, sizeof(line), "v%s (%s)  -  Circulo: salir", PROSPERO_RELEASE, MILESTONE);
    overlayText(40, OVERLAY_HEIGHT - 60, line, 26, 0x707070FF);
    overlayEnd();
}

int main(void) {
    diagnosticsTrace("Spotrps5 v%s, %s (%s) starting; UDP log port 18194", PROSPERO_RELEASE, MILESTONE, PROSPERO_TITLE_ID);
    signal(SIGPIPE, SIG_IGN);  // a write to a closed socket must fail with EPIPE, not end the title
    bool screen = screenOpen();
    diagnosticsTrace("screen: %s %dx%d", screen ? "ready" : "unavailable", width, height);
    char address[16];
    if (!platformLocalIPv4(address)) snprintf(address, sizeof(address), "?");
    diagnosticsTrace("network: local address %s", address);
    for (unsigned frame = 1;; ++frame) {
        if (screen) {
            drawScreen(address, frame);
            eglSwapBuffers(display, surface);
        }
        PlatformPad pad;
        if (platformPadRead(&pad) && (pad.buttons & PLATFORM_PAD_CIRCLE)) {
            diagnosticsTrace("Circle pressed: closing the title");
            platformQuit();
        }
        sceKernelUsleep(16000);
    }
}
