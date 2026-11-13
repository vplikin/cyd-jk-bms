#pragma once

#include "config.h"

// UI string for the configured UI_LANG, picked at compile time.
constexpr const char *tr(const char *en, const char *ru) { return UI_LANG == UI_LANG_RU ? ru : en; }
