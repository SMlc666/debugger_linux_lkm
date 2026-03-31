#include "nativeui/imgui_controls.h"

#include <string>

#include "nativeui/workspace_theme.h"

namespace lkmdbg::nativeui::controls {

namespace {

void muted_text(const char *text)
{
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	ImGui::PushStyleColor(ImGuiCol_Text, theme.on_surface_variant);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

} // namespace

bool ActionChipButton(const char *label, bool active, float density, float width)
{
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	const ImVec4 fill = active ? theme.primary_container :
				     MixColor(theme.surface_container_high,
					      theme.primary_container, 0.45f);
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.primary);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.primary);
	ImGui::PushStyleColor(ImGuiCol_Text, active ? theme.on_primary_container :
					     theme.on_surface);
	const float resolved_width =
		width > 0.0f ? width : ImGui::CalcTextSize(label).x + 28.0f * density;
	const bool pressed = ImGui::Button(label, ImVec2(resolved_width, 30.0f * density));
	ImGui::PopStyleColor(4);
	return pressed;
}

bool RailButton(const char *label, bool selected, float density, float highlight_mix)
{
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	const ImVec4 base = theme.surface_container;
	const ImVec4 accent = theme.primary_container;
	const ImVec4 text_base = theme.on_surface;
	const ImVec4 text_accent = theme.on_primary_container;

	ImGui::PushStyleColor(ImGuiCol_Header, MixColor(base, accent, highlight_mix));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.primary);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme.primary_container);
	ImGui::PushStyleColor(ImGuiCol_Text, MixColor(text_base, text_accent, highlight_mix));
	const bool pressed = ImGui::Selectable(label, selected, 0, ImVec2(-1.0f, 38.0f * density));
	ImGui::PopStyleColor(4);
	return pressed;
}

void MetricPill(const MetricItem &item, float density)
{
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	const ImVec4 base = theme.surface_container_high;
	const ImVec4 accent = theme.secondary;

	ImGui::PushStyleColor(ImGuiCol_Button, MixColor(base, accent, item.accent_mix));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.secondary_container);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.secondary);
	ImGui::PushStyleColor(ImGuiCol_Text, MixColor(theme.on_surface,
						     theme.on_secondary, item.accent_mix * 0.85f));
	ImGui::Button(item.text.c_str(), ImVec2(118.0f * density, 26.0f * density));
	ImGui::PopStyleColor(4);
}

bool FillLaneButton(const char *label, bool hot, float density, float mix_value,
		    float width)
{
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	ImGui::PushStyleColor(
		ImGuiCol_Button,
		MixColor(theme.surface_container, theme.tertiary, hot ? mix_value : mix_value * 0.25f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.tertiary_container);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.tertiary);
	ImGui::PushStyleColor(ImGuiCol_Text, hot ? theme.on_tertiary : theme.on_surface);
	const float resolved_width =
		width > 0.0f ? width : ImGui::CalcTextSize(label).x + 24.0f * density;
	const bool pressed = ImGui::Button(label, ImVec2(resolved_width, 28.0f * density));
	ImGui::PopStyleColor(4);
	return pressed;
}

int SectionTabs(const SectionItem *items, int count, float density)
{
	int pressed = -1;
	ImGui::BeginChild("section_tabs", ImVec2(0.0f, 54.0f * density), ImGuiChildFlags_Borders);
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
	ImGui::BeginChild("section_rail", ImVec2(188.0f * density, 0.0f), ImGuiChildFlags_Borders);
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
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	const ImVec4 fill = selected ? WithAlpha(theme.primary_container, 0.96f) :
				       WithAlpha(theme.surface_container, 0.94f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, fill);
	ImGui::PushStyleColor(ImGuiCol_Border,
			      selected ? theme.primary : theme.outline_variant);
	ImGui::BeginChild(title, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
	ImGui::PushID(title);
	const bool pressed = ImGui::InvisibleButton("entry", ImVec2(-1.0f, height - 8.0f * density));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (height - 8.0f * density));
	ImGui::PushStyleColor(ImGuiCol_Text, selected ? theme.on_primary_container :
					     theme.on_surface);
	ImGui::TextUnformatted(title);
	if (badge && badge[0] != '\0') {
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, theme.secondary);
		ImGui::TextUnformatted(badge);
		ImGui::PopStyleColor();
	}
	if (subtitle && subtitle[0] != '\0') {
		ImGui::Spacing();
		muted_text(subtitle);
	}
	ImGui::PopStyleColor();
	ImGui::PopID();
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
	return pressed;
}

} // namespace lkmdbg::nativeui::controls
