// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
// A baseline JPEG decoder (grayscale or YCbCr, any sampling): what Spotify's cover images are. Progressive files are refused.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decodes to RGBA bytes (memory order R,G,B,A), malloc'd; the caller frees. NULL for anything it cannot decode.
uint32_t *jpegDecode(const uint8_t *data, size_t size, unsigned *width, unsigned *height);
