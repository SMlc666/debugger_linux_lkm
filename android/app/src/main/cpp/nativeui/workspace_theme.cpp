#include "nativeui/workspace_theme.h"

#include <algorithm>

namespace lkmdbg::nativeui {

namespace {

ImVec4 rgb(int r, int g, int b, float alpha = 1.0f)
{
	return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, alpha);
}

const WorkspaceTheme kTheme = {
	rgb(0x09, 0x12, 0x16),
	rgb(0x10, 0x1B, 0x20),
	rgb(0x16, 0x25, 0x2B),
	rgb(0x20, 0x33, 0x3A),
	rgb(0x31, 0x45, 0x4C),
	rgb(0x4E, 0xD7, 0xC4),
	rgb(0x03, 0x20, 0x1B),
	rgb(0x0A, 0x52, 0x4A),
	rgb(0x9A, 0xF4, 0xE5),
	rgb(0xD7, 0xB3, 0x77),
	rgb(0x2A, 0x19, 0x00),
	rgb(0x4E, 0x39, 0x10),
	rgb(0xF7, 0xDE, 0xA6),
	rgb(0x91, 0xC4, 0xDD),
	rgb(0x08, 0x20, 0x2A),
	rgb(0x28, 0x46, 0x51),
	rgb(0xD1, 0xEC, 0xF8),
	rgb(0x8A, 0x94, 0x97),
	rgb(0x41, 0x51, 0x56),
	rgb(0xE2, 0xEE, 0xF0),
	rgb(0xBC, 0xC8, 0xCB),
};

} // namespace

const WorkspaceTheme &GetWorkspaceTheme()
{
	return kTheme;
}

ImVec4 MixColor(const ImVec4 &base, const ImVec4 &accent, float t)
{
	const float clamped = std::clamp(t, 0.0f, 1.0f);
	return ImVec4(
		base.x + (accent.x - base.x) * clamped,
		base.y + (accent.y - base.y) * clamped,
		base.z + (accent.z - base.z) * clamped,
		base.w + (accent.w - base.w) * clamped);
}

ImVec4 WithAlpha(const ImVec4 &color, float alpha)
{
	return ImVec4(color.x, color.y, color.z, alpha);
}

ImU32 ToImU32(const ImVec4 &color)
{
	return ImGui::ColorConvertFloat4ToU32(color);
}

} // namespace lkmdbg::nativeui
