// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors (from PokeMMO-Prospero's overlay)
//
// What this project draws: the Spotrps5 screens, batched and drawn with the overlay's own shader on whichever context is current; the
// state of that context is saved and restored around it. Coordinates are a virtual 1920x1080 screen, colors are 0xRRGGBBAA.
// Shapes with a radius (rounded rectangles, circles, rings, images with rounded corners) are anti-aliased in the shader; when the system's
// OpenGL refuses that shader the overlay falls back to plain rectangles (overlayRoundedAvailable() tells).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OVERLAY_WIDTH 1920.0f
#define OVERLAY_HEIGHT 1080.0f

bool overlayBegin(unsigned width, unsigned height);  // false (and nothing to end) when OpenGL cannot be used
void overlayEnd(void);
bool overlayRoundedAvailable(void);
void overlayClear(uint32_t rgba);  // the whole picture, for screens of our own

// ---- shapes -------------------------------------------------------------------------------------------------------------------------
void overlayRect(float x, float y, float w, float h, uint32_t rgba);
void overlayFrame(float x, float y, float w, float h, float thickness, uint32_t rgba);  // outline inside the rectangle
void overlayRoundRect(float x, float y, float w, float h, float radius, uint32_t rgba);
void overlayRoundFrame(float x, float y, float w, float h, float radius, float thickness, uint32_t rgba);  // ring inside the rectangle
void overlayCircle(float cx, float cy, float radius, uint32_t rgba);
void overlayRectGradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);  // vertical
void overlayRoundGradient(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom);
void overlayClip(float x, float y, float w, float h);  // everything drawn until overlayClipEnd stays inside the rectangle
void overlayClipEnd(void);

// ---- text (UTF-8; DejaVu Sans; Latin-1 and Latin Extended-A) ----------------------------------------------------------------------------
// `size` is the line height in pixels, y the top of the line. Codes 1-4 are the controller's face buttons (overlay_assets.h).
// Returns the width drawn.
float overlayText(float x, float y, const char *text, float size, uint32_t rgba);
float overlayTextBold(float x, float y, const char *text, float size, uint32_t rgba);
float overlayTextWidth(const char *text, float size);
float overlayTextBoldWidth(const char *text, float size);
float overlayTextCentered(float centre_x, float y, const char *text, float size, uint32_t rgba);
// Text cut to fit `max_width`, with "..." where it was cut.
void overlayTextFit(float x, float y, const char *text, float size, float max_width, uint32_t rgba);
void overlayTextFitBold(float x, float y, const char *text, float size, float max_width, uint32_t rgba);

// ---- icons (overlay_icons.h) -------------------------------------------------------------------------------------------------------------
void overlayIcon(unsigned icon, float centre_x, float centre_y, float size, uint32_t rgba);  // `size` is the icon's height and width

// ---- pictures ------------------------------------------------------------------------------------------------------------------------
typedef struct OverlayImage OverlayImage;
OverlayImage *overlayImageCreate(unsigned width, unsigned height, const uint32_t *rgba);  // RGBA bytes in memory order; copied
void overlayImageDestroy(OverlayImage *image);
// The picture stretched over the rectangle, corners rounded by `radius` (0: square), multiplied by `tint` (0xFFFFFFFF: as it is).
void overlayImageDraw(OverlayImage *image, float x, float y, float w, float h, float radius, uint32_t tint);

// The context the overlay's objects were made in is gone (a new one may get its address): make them again next time.
void overlayContextLost(void);
// Whether the picture drawn so far shows anything but black (reads a few pixels back: call it seldom, outside Begin/End).
bool overlayPictureHasContent(unsigned width, unsigned height);
void overlayLogo(float x, float y, float height);  // kept for old callers: draws nothing useful
