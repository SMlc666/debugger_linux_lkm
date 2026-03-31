#pragma once

#include "imgui.h"

namespace lkmdbg::nativeui {

struct WorkspaceTheme {
	ImVec4 background;
	ImVec4 surface;
	ImVec4 surface_container;
	ImVec4 surface_container_high;
	ImVec4 surface_variant;
	ImVec4 primary;
	ImVec4 on_primary;
	ImVec4 primary_container;
	ImVec4 on_primary_container;
	ImVec4 secondary;
	ImVec4 on_secondary;
	ImVec4 secondary_container;
	ImVec4 on_secondary_container;
	ImVec4 tertiary;
	ImVec4 on_tertiary;
	ImVec4 tertiary_container;
	ImVec4 on_tertiary_container;
	ImVec4 outline;
	ImVec4 outline_variant;
	ImVec4 on_surface;
	ImVec4 on_surface_variant;
};

const WorkspaceTheme &GetWorkspaceTheme();
ImVec4 MixColor(const ImVec4 &base, const ImVec4 &accent, float t);
ImVec4 WithAlpha(const ImVec4 &color, float alpha);
ImU32 ToImU32(const ImVec4 &color);

} // namespace lkmdbg::nativeui
