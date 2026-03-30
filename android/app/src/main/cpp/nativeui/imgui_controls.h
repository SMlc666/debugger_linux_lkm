#pragma once

#include "imgui.h"

namespace lkmdbg::nativeui::controls {

bool RailButton(const char *label, bool selected, float density, float highlight_mix);
void MetricPill(const char *label, const char *value, float density, float accent_mix);
void FillLaneButton(const char *label, bool hot, float density, float mix);

} // namespace lkmdbg::nativeui::controls
