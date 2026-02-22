/**************************************************************************/
/*  aristotle_tokens.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Aristotle Theme - Token-based design system                            */
/* All Aristotle visual constants in one place for easy tuning.           */
/*                                                                        */
/* Tuning guide:                                                          */
/*   - BG_0..BG_3: background layers (darkest to lightest)                */
/*   - ACCENT_*: primary interactive color (blue)                         */
/*   - TEXT_*: text hierarchy                                             */
/*   - BORDER_*: edge separation                                          */
/*   - STATUS_*: semantic feedback colors                                 */
/*   - Metrics: corner radius, padding, border width                      */
/**************************************************************************/

#pragma once

#include "core/math/color.h"

namespace Aristotle {

// ── Background hierarchy (near-true-black) ──────────────────────────────
static const Color BG_0 = Color(0.08, 0.08, 0.09, 1.0);       // #141417 - Deepest background / code editor
static const Color BG_1 = Color(0.11, 0.11, 0.13, 1.0);       // #1C1C21 - Panel background / cards
static const Color BG_2 = Color(0.14, 0.14, 0.16, 1.0);       // #242428 - Input fields / buttons
static const Color BG_3 = Color(0.18, 0.18, 0.20, 1.0);       // #2E2E33 - Hover states / raised

// ── Borders ─────────────────────────────────────────────────────────────
static const Color BORDER = Color(0.25, 0.25, 0.28, 1.0);     // #404047 - Subtle borders
static const Color BORDER_LIGHT = Color(0.35, 0.35, 0.38, 1.0); // #595961 - Focus / hover borders

// ── Text hierarchy ──────────────────────────────────────────────────────
static const Color TEXT_PRIMARY = Color(0.93, 0.93, 0.95, 1.0);   // #EDEFF2 - Primary text
static const Color TEXT_SECONDARY = Color(0.7, 0.7, 0.73, 1.0);  // #B3B3BA - Secondary / labels
static const Color TEXT_MUTED = Color(0.5, 0.5, 0.53, 1.0);      // #808087 - Placeholder / muted
static const Color TEXT_DISABLED = Color(0.35, 0.35, 0.38, 1.0); // #595961 - Disabled text

// ── Accent (blue) ───────────────────────────────────────────────────────
static const Color ACCENT = Color(0.30, 0.52, 0.90, 1.0);         // #4D85E6 - Primary accent
static const Color ACCENT_HOVER = Color(0.35, 0.57, 0.95, 1.0);  // #5991F2 - Hover
static const Color ACCENT_PRESSED = Color(0.25, 0.45, 0.80, 1.0); // #4073CC - Pressed
static const Color ACCENT_MUTED = Color(0.20, 0.32, 0.55, 0.95); // #33528C - Muted accent

// ── Status / semantic ───────────────────────────────────────────────────
static const Color STATUS_SUCCESS = Color(0.35, 0.78, 0.45, 1.0);  // #59C773
static const Color STATUS_ERROR = Color(0.90, 0.40, 0.40, 1.0);    // #E66666
static const Color STATUS_WARNING = Color(0.95, 0.75, 0.25, 1.0);  // #F2BF40

// ── Selection / highlight ───────────────────────────────────────────────
static const Color SELECTION = Color(0.30, 0.52, 0.90, 0.20);     // Accent at 20% alpha
static const Color HIGHLIGHT = Color(0.30, 0.52, 0.90, 0.275);    // Accent at 27.5% alpha

// ── Metrics ─────────────────────────────────────────────────────────────
static const int RADIUS_SM = 3;    // Small rounding (buttons, inputs)
static const int RADIUS_MD = 5;    // Medium rounding (panels, cards)
static const int RADIUS_LG = 8;    // Large rounding (popups, dialogs)

static const int PAD_XS = 2;
static const int PAD_SM = 6;
static const int PAD_MD = 10;
static const int PAD_LG = 14;

static const int BORDER_WIDTH = 1;     // Default border width
static const int FOCUS_WIDTH = 2;      // Focus ring width
static const float SEPARATOR_ALPHA = 0.06f; // Very subtle separators

// ── Preset parameters (fed into Godot's theme generation) ───────────────
// These are the 3 inputs to Godot's built-in color derivation system.
static const Color PRESET_BASE_COLOR = Color(0.11, 0.11, 0.13);
static const Color PRESET_ACCENT_COLOR = Color(0.30, 0.52, 0.90);
static const float PRESET_CONTRAST = 0.25f;

} // namespace Aristotle
