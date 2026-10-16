// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
//
// The Spotrps5 title: opens the screen, runs librespot (Spotify Connect) inside the loader and plays what it outputs. librespot's
// "pipe" backend writes raw S16 stereo 44.1 kHz PCM to stdout; the loader hands those bytes to pcmSink, which resamples to the
// console's 48 kHz and writes them to the audio output. Logs go over UDP (port 18194, tools/udplog.py).
#include "diagnostics.h"
#include "game.h"
#include "overlay.h"
#include "platform.h"
#include "prospero_version.h"  // PROSPERO_RELEASE, PROSPERO_TITLE_ID (generated at build time)
#include <EGL/egl.h>
#include <GL/gl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

// ps5-opengl's app heap (malloc of the loader and of everything the program allocates with malloc): from direct memory.
const size_t ps5_opengl_heap_size = 768u << 20;

int sceKernelUsleep(unsigned int microseconds);

#define MILESTONE "spotrps5-3a"
#define DEVICE_NAME "Spotrps5"
#define ROOT "/download0/spotrps5"

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

// Milestone 2 probe: can a title point %gs at a block of its own? (The loader needs it for programs written in Rust.) Done on a thread of
// its own so that the result is the same one the loader will get on the threads it starts.
static _Atomic int tls_result;  // 0 running, 1 works, 2 does not
static void *tlsProbe(void *argument) {
    (void)argument;
    static unsigned char block[256] __attribute__((aligned(64)));
    void *pointer = block + 64;
    *(void **)pointer = pointer;  // gs:0 holds the block's own address
    bool ok = platformSetThreadPointer(pointer);
    void *read_back = NULL;
    if (ok) __asm__ volatile("movq %%gs:0, %0" : "=r"(read_back));
    diagnosticsTrace("tls.probe ok=%d read_back=%p expected=%p method=%s", ok, read_back, pointer, platformThreadPointerNote());
    atomic_store(&tls_result, ok && read_back == pointer ? 1 : 2);
    return NULL;
}


// ---- audio: PCM from librespot (44.1 kHz) to the console (48 kHz) -------------------------------------------------------------------
#define BLOCK_FRAMES 1024u
static struct {
    int handle;
    int16_t out[BLOCK_FRAMES * 2];
    unsigned out_frames;
    uint8_t carry[4];
    unsigned carry_bytes;
    int16_t previous[2];
    double position;
    bool primed;
} pcm = {.handle = -1};
static _Atomic unsigned long long pcm_bytes;

static void pcmEmit(int16_t left, int16_t right) {
    pcm.out[pcm.out_frames * 2] = left;
    pcm.out[pcm.out_frames * 2 + 1] = right;
    if (++pcm.out_frames == BLOCK_FRAMES) {
        if (pcm.handle >= 0) platformAudioWrite(pcm.handle, pcm.out);
        pcm.out_frames = 0;
    }
}
static void pcmFrame(int16_t left, int16_t right) {
    if (!pcm.primed) {
        pcm.previous[0] = left;
        pcm.previous[1] = right;
        pcm.primed = true;
        return;
    }
    const double step = 44100.0 / 48000.0;
    while (pcm.position < 1.0) {  // linear interpolation between the previous and the current input frame
        double t = pcm.position;
        pcmEmit((int16_t)(pcm.previous[0] + (left - pcm.previous[0]) * t), (int16_t)(pcm.previous[1] + (right - pcm.previous[1]) * t));
        pcm.position += step;
    }
    pcm.position -= 1.0;
    pcm.previous[0] = left;
    pcm.previous[1] = right;
}
static int64_t pcmSink(void *context, const void *bytes, size_t count) {
    (void)context;
    if (pcm.handle < 0) {
        pcm.handle = platformAudioOpen(BLOCK_FRAMES);
        diagnosticsTrace("audio: open handle=%d", pcm.handle);
        if (pcm.handle < 0) pcm.handle = -2;  // do not retry on every write
    }
    const uint8_t *in = bytes;
    size_t left = count;
    while (left) {
        while (left && pcm.carry_bytes < 4) {
            pcm.carry[pcm.carry_bytes++] = *in++;
            --left;
        }
        if (pcm.carry_bytes < 4) break;
        int16_t l = (int16_t)(pcm.carry[0] | (pcm.carry[1] << 8)), r = (int16_t)(pcm.carry[2] | (pcm.carry[3] << 8));
        pcm.carry_bytes = 0;
        pcmFrame(l, r);
    }
    atomic_fetch_add(&pcm_bytes, count);
    return (int64_t)count;
}

// ---- librespot ----------------------------------------------------------------------------------------------------------------------
static _Atomic int player_state;  // 0 starting, 1 running, 2 ended with an error
static char player_failure[160];
static void *librespotThread(void *argument) {
    (void)argument;
    mkdir(ROOT, 0755);
    mkdir(ROOT "/game", 0755);
    static const char *const arguments[] = {"--name", DEVICE_NAME, "--backend", "pipe", "--format", "S16", "--disable-audio-cache", "--cache",
                                            "/home/ps5/cache", "--initial-volume", "70", "-v", NULL};
    GameConfig config = {.root = ROOT,
                         .client_path = "/app0/assets/librespot",
                         .mounts = {{"/game", ROOT "/game"}},
                         .mount_count = 1,
                         .arguments = arguments,
                         .stdout_sink = pcmSink};
    atomic_store(&player_state, 1);
    bool ok = gameRun(&config);
    snprintf(player_failure, sizeof(player_failure), "%s", ok ? "librespot termino" : gameFailure());
    diagnosticsTrace("librespot ended ok=%d: %s", ok, player_failure);
    atomic_store(&player_state, 2);
    return NULL;
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
    int state = atomic_load(&player_state);
    if (state == 2) {
        overlayTextCentered(OVERLAY_WIDTH / 2, 560, "librespot se detuvo", 36, 0xFF6060FF);
        overlayTextCentered(OVERLAY_WIDTH / 2, 605, player_failure, 26, 0xA0A0A0FF);
        unsigned lines = gameRecentCount(), first = lines > 9 ? lines - 9 : 0;
        for (unsigned i = first; i < lines; ++i) {
            char shown[100];
            snprintf(shown, sizeof(shown), "%s", gameRecentLine(i));
            overlayText(60, 650 + (int)(i - first) * 26, shown, 20, 0xFFD080FF);
        }
    } else {
        snprintf(line, sizeof(line), "Abre Spotify (Premium) y elige el dispositivo \"%s\"%s", DEVICE_NAME, dots[(frame / 30) % 4]);
        overlayTextCentered(OVERLAY_WIDTH / 2, 560, line, 34, 0xE8E8E8FF);
        snprintf(line, sizeof(line), "Audio recibido: %llu KB", atomic_load(&pcm_bytes) / 1024);
        overlayTextCentered(OVERLAY_WIDTH / 2, 620, line, 30, 0xA0A0A0FF);
    }
    snprintf(line, sizeof(line), "IP de la consola: %s", address);
    overlayTextCentered(OVERLAY_WIDTH / 2, 940, line, 24, 0xA0A0A0FF);
    int probe = atomic_load(&tls_result);
    snprintf(line, sizeof(line), "Prueba de hilos (%%gs): %s%s%s", probe == 0 ? "comprobando" : probe == 1 ? "FUNCIONA, via " : "NO FUNCIONA: ",
             probe == 0 ? "" : platformThreadPointerNote(), "");
    overlayTextCentered(OVERLAY_WIDTH / 2, 985, line, 24, probe == 1 ? 0x1DB954FF : probe == 2 ? 0xFF6060FF : 0xA0A0A0FF);
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
    pthread_t probe_thread, player_thread;
    if (pthread_create(&probe_thread, NULL, tlsProbe, NULL)) atomic_store(&tls_result, 2);
    pthread_create(&player_thread, NULL, librespotThread, NULL);
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
