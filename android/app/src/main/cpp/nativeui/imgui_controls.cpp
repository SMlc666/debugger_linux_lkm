#include "nativeui/imgui_controls.h"

#include <string>

namespace lkmdbg::nativeui::controls {

namespace {

ImVec4 mix(const ImVec4 &base, const ImVec4 &accent, float t)
{
	return ImVec4(
		base.x + (accent.x - base.x) * t,
		base.y + (accent.y - base.y) * t,
		base.z + (accent.z - base.z) * t,
		base.w + (accent.w - base.w) * t);
}

void muted_text(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.79f, 0.84f, 1.0f));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

} // namespace

bool RailButton(const char *label, bool selected, float density, float highlight_mix)
{
	const ImVec4 base(0.10f, 0.19f, 0.24f, 1.0f);
	const ImVec4 accent(0.23f, 0.71f, 0.76f, 0.92f);
	const ImVec4 text_base(0.95f, 0.97f, 0.98f, 1.0f);
	const ImVec4 text_accent(0.04f, 0.11f, 0.14f, 1.0f);

	ImGui::PushStyleColor(ImGuiCol_Header, mix(base, accent, highlight_mix));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, accent);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, accent);
	ImGui::PushStyleColor(ImGuiCol_Text, mix(text_base, text_accent, highlight_mix));
	const bool pressed = ImGui::Selectable(label, selected, 0, ImVec2(-1.0f, 28.0f * density));
	ImGui::PopStyleColor(4);
	return pressed;
}

void MetricPill(const char *label, const char *value, float density, float accent_mix)
{
	const ImVec4 base(0.09f, 0.18f, 0.22f, 1.0f);
	const ImVec4 accent(0.23f, 0.71f, 0.76f, 0.92f);

	ImGui::PushStyleColor(ImGuiCol_Button, mix(base, accent, accent_mix));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
	ImGui::Button((std::string(label) + ": " + value).c_str(), ImVec2(118.0f * density, 26.0f * density));
	ImGui::PopStyleColor(3);
}

void FillLaneButton(const char *label, bool hot, float density, float mix_value)
{
	const ImVec4 cold(0.10f, 0.19f, 0.24f, 1.0f);
	const ImVec4 warm(0.23f, 0.71f, 0.76f, 0.92f);
	ImGui::PushStyleColor(ImGuiCol_Button, mix(cold, warm, hot ? mix_value : mix_value * 0.25f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, warm);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, warm);
	ImGui::Button(label, ImVec2(-1.0f, 24.0f * density));
	ImGui::PopStyleColor(3);
}

int SectionTabs(const SectionItem *items, int count, float density)
{
	int pressed = -1;
	ImGui::BeginChild("section_tabs", ImVec2(0.0f, 46.0f * density), ImGuiChildFlags_Borders);
	for (int i = 0; i < count; ++i) {
		if (i > 0)
			ImGui::SameLine();
		ImGui::PushID(i);
		if (RailButton(items[i].label, items[i].selected, density, items[i].highlight_mix))
			pressed = i;
		ImGui::PopID();
	}
	ImGui::EndChild();
	return pressed;
}

int SectionRail(const char *title, const SectionItem *items, int count, float density)
{
	int pressed = -1;
	ImGui::BeginChild("section_rail", ImVec2(156.0f * density, 0.0f), ImGuiChildFlags_Borders);
	if (title && title[0] != '\0') {
		ImGui::TextUnformatted(title);
		ImGui::Separator();
	}
	for (int i = 0; i < count; ++i) {
		if (RailButton(items[i].label, items[i].selected, density, items[i].highlight_mix))
			pressed = i;
	}
	ImGui::EndChild();
	return pressed;
}

void SectionHeader(const char *title, const char *subtitle)
{
	ImGui::Text("%s", title);
	muted_text(subtitle);
}

void MetricStrip(const char *left_label, const char *left_value,
		 const char *mid_label, const char *mid_value,
		 const char *right_label, const char *right_value,
		 float density, float left_mix, float mid_mix, float right_mix)
{
	ImGui::BeginChild("metric_strip", ImVec2(0.0f, 74.0f * density), ImGuiChildFlags_Borders);
	MetricPill(left_label, left_value, density, left_mix);
	ImGui::SameLine();
	MetricPill(mid_label, mid_value, density, mid_mix);
	ImGui::SameLine();
	MetricPill(right_label, right_value, density, right_mix);
	ImGui::EndChild();
}

void StatLine(const char *label, const char *value)
{
	ImGui::BulletText("%s: %s", label, value);
}

void StatLineValue(const char *label, int value)
{
	ImGui::BulletText("%s: %d", label, value);
}

void InfoCard(const char *title, const char *body, float height)
{
	ImGui::BeginChild(title, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
	ImGui::Text("%s", title);
	ImGui::Spacing();
	muted_text(body);
	ImGui::EndChild();
}

} // namespace lkmdbg::nativeui::controls
