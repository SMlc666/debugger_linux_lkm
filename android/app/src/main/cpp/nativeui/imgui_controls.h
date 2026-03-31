#pragma once

#include "imgui.h"

namespace lkmdbg::nativeui::controls {

struct SectionItem {
	const char *label;
	bool selected;
	float highlight_mix;
};

bool RailButton(const char *label, bool selected, float density, float highlight_mix);
void MetricPill(const char *label, const char *value, float density, float accent_mix);
void FillLaneButton(const char *label, bool hot, float density, float mix);
int SectionTabs(const SectionItem *items, int count, float density);
int SectionRail(const char *title, const SectionItem *items, int count, float density);
void SectionHeader(const char *title, const char *subtitle);
void MetricStrip(const char *left_label, const char *left_value,
		 const char *mid_label, const char *mid_value,
		 const char *right_label, const char *right_value,
		 float density, float left_mix, float mid_mix, float right_mix);
void StatLine(const char *label, const char *value);
void StatLineValue(const char *label, int value);
void InfoCard(const char *title, const char *body, float height);

} // namespace lkmdbg::nativeui::controls
