#pragma once

#include <string>

#include "imgui.h"

namespace lkmdbg::nativeui::controls {

struct SectionItem {
	const char *label;
	bool selected;
	float highlight_mix;
};

struct MetricItem {
	std::string text;
	float accent_mix;
};

struct StatItem {
	std::string text;
};

bool RailButton(const char *label, bool selected, float density, float highlight_mix);
void MetricPill(const MetricItem &item, float density);
void FillLaneButton(const char *label, bool hot, float density, float mix);
int SectionTabs(const SectionItem *items, int count, float density);
int SectionRail(const char *title, const SectionItem *items, int count, float density);
void SectionHeader(const char *title, const char *subtitle);
void MetricStrip(const MetricItem &left, const MetricItem &mid,
		 const MetricItem &right, float density);
void StatLine(const StatItem &item);
void InfoCard(const char *title, const char *body, float height);

} // namespace lkmdbg::nativeui::controls
