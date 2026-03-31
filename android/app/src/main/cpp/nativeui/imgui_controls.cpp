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
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

} // namespace

bool ActionChipButton(const char *label, bool active, float density)
{
	const ImVec4 cold(0.10f, 0.19f, 0.24f, 1.0f);
	const ImVec4 warm(0.23f, 0.71f, 0.76f, 0.92f);
	const ImVec4 fill = active ? warm : mix(cold, warm, 0.25f);
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, warm);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, warm);
	const bool pressed = ImGui::Button(label, ImVec2(-1.0f, 28.0f * density));
	ImGui::PopStyleColor(3);
	return pressed;
}

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

void MetricPill(const MetricItem &item, float density)
{
	const ImVec4 base(0.09f, 0.18f, 0.22f, 1.0f);
	const ImVec4 accent(0.23f, 0.71f, 0.76f, 0.92f);

	ImGui::PushStyleColor(ImGuiCol_Button, mix(base, accent, item.accent_mix));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
	ImGui::Button(item.text.c_str(), ImVec2(118.0f * density, 26.0f * density));
	ImGui::PopStyleColor(3);
}

bool FillLaneButton(const char *label, bool hot, float density, float mix_value)
{
	const ImVec4 cold(0.10f, 0.19f, 0.24f, 1.0f);
	const ImVec4 warm(0.23f, 0.71f, 0.76f, 0.92f);
	ImGui::PushStyleColor(ImGuiCol_Button, mix(cold, warm, hot ? mix_value : mix_value * 0.25f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, warm);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, warm);
	const bool pressed = ImGui::Button(label, ImVec2(-1.0f, 24.0f * density));
	ImGui::PopStyleColor(3);
	return pressed;
}

int SectionTabs(const SectionItem *items, int count, float density)
{
	int pressed = -1;
	ImGui::BeginChild("section_tabs", ImVec2(0.0f, 46.0f * density), ImGuiChildFlags_Borders);
	for (int i = 0; i < count; ++i) {
		if (i > 0)
			ImGui::SameLine();
		ImGui::PushID(i);
		if (RailButton(items[i].label.c_str(), items[i].selected, density,
			       items[i].highlight_mix))
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
		if (RailButton(items[i].label.c_str(), items[i].selected, density,
			       items[i].highlight_mix))
			pressed = i;
	}
	ImGui::EndChild();
	return pressed;
}

void SectionHeader(const char *title, const char *subtitle)
{
	ImGui::TextUnformatted(title);
	muted_text(subtitle);
}

void MetricStrip(const MetricItem &left, const MetricItem &mid,
		 const MetricItem &right, float density)
{
	ImGui::BeginChild("metric_strip", ImVec2(0.0f, 74.0f * density), ImGuiChildFlags_Borders);
	MetricPill(left, density);
	ImGui::SameLine();
	MetricPill(mid, density);
	ImGui::SameLine();
	MetricPill(right, density);
	ImGui::EndChild();
}

void StatLine(const StatItem &item)
{
	ImGui::Bullet();
	ImGui::SameLine();
	ImGui::TextUnformatted(item.text.c_str());
}

void InfoCard(const char *title, const char *body, float height)
{
	ImGui::BeginChild(title, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
	ImGui::TextUnformatted(title);
	ImGui::Spacing();
	muted_text(body);
	ImGui::EndChild();
}

bool ListEntryCard(const char *title, const char *subtitle, const char *badge,
		   bool selected, float density, float height)
{
	const ImVec4 cold(0.08f, 0.15f, 0.19f, 0.92f);
	const ImVec4 warm(0.23f, 0.71f, 0.76f, selected ? 0.92f : 0.28f);
	const ImVec4 fill = selected ? warm : cold;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, fill);
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.41f, 0.47f, 0.72f));
	ImGui::BeginChild(title, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
	ImGui::PushID(title);
	const bool pressed = ImGui::InvisibleButton("entry", ImVec2(-1.0f, height - 8.0f * density));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (height - 8.0f * density));
	ImGui::TextUnformatted(title);
	if (badge && badge[0] != '\0') {
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.61f, 0.92f, 0.88f, 1.0f));
		ImGui::TextUnformatted(badge);
		ImGui::PopStyleColor();
	}
	if (subtitle && subtitle[0] != '\0') {
		ImGui::Spacing();
		muted_text(subtitle);
	}
	ImGui::PopID();
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
	return pressed;
}

} // namespace lkmdbg::nativeui::controls
