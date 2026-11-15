// The setup's background (runtime/ui/backdrop.mm): a calm night sky for the menu - a charcoal gradient, a few
// twinkling stars and the project's crescent moon (theme.h) in the accent red, drawn faintly. Scales with the window,
// no image files.
#pragma once
#include "imgui.h"

// The scene over the rectangle [p0, p0 + size), cropped to fill it; t: seconds (the animation).
void backdrop_draw(ImDrawList *dl, ImVec2 p0, ImVec2 size, double t);
