#include "nativeui/ui_tree.h"

#include "imgui.h"
#include "nativeui/imgui_controls.h"

namespace lkmdbg::nativeui {

WorkspaceUiResult RenderWorkspaceUi(const WorkspaceViewModel &model, float density)
{
	WorkspaceUiResult result{};
	int selected_section = -1;

	if (model.portrait) {
		selected_section =
			controls::SectionTabs(model.sections.data(), kWorkspaceSectionCount, density);
		ImGui::Spacing();
	} else {
		selected_section = controls::SectionRail(
			model.rail_title.c_str(), model.sections.data(), kWorkspaceSectionCount,
			density);
		ImGui::SameLine();
	}
	if (selected_section >= 0)
		result.action_key = "section:" + std::to_string(selected_section);

	ImGui::BeginChild("main_workspace", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	controls::SectionHeader(model.workspace_title.c_str(),
				model.workspace_subtitle.c_str());
	ImGui::Spacing();
	controls::MetricStrip(model.metrics[0], model.metrics[1], model.metrics[2], density);
	ImGui::Spacing();

	ImGui::BeginChild("section_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
	controls::SectionHeader(model.panel.title.c_str(), model.panel.subtitle.c_str());
	ImGui::Separator();
	for (const auto &stat : model.panel.stats)
		controls::StatLine(stat);
	if (!model.panel.stats.empty())
		ImGui::Spacing();
	for (const auto &action : model.panel.primary_actions) {
		if (controls::ActionChipButton(action.label.c_str(), action.active, density) &&
		    result.action_key.empty())
			result.action_key = action.key;
	}
	if (!model.panel.primary_actions.empty())
		ImGui::Spacing();
	for (const auto &action : model.panel.secondary_actions) {
		if (controls::FillLaneButton(action.label.c_str(), action.active, density, 0.9f) &&
		    result.action_key.empty())
			result.action_key = action.key;
	}
	if (!model.panel.secondary_actions.empty())
		ImGui::Spacing();
	for (const auto &entry : model.panel.list_entries) {
		ImGui::PushID(entry.key.c_str());
		if (controls::ListEntryCard(entry.title.c_str(), entry.subtitle.c_str(),
				       entry.badge.c_str(), entry.selected, density,
				       entry.height) &&
		    result.action_key.empty()) {
			result.action_key = entry.key;
		}
		ImGui::PopID();
		ImGui::Spacing();
	}
	for (const auto &entry : model.panel.detail_entries) {
		ImGui::PushID(entry.key.c_str());
		if (controls::ListEntryCard(entry.title.c_str(), entry.subtitle.c_str(),
				       entry.badge.c_str(), entry.selected, density,
				       entry.height) &&
		    result.action_key.empty()) {
			result.action_key = entry.key;
		}
		ImGui::PopID();
		ImGui::Spacing();
	}
	for (const auto &line : model.panel.detail_lines)
		controls::StatLine(controls::StatItem{line});
	if (!model.panel.detail_lines.empty())
		ImGui::Spacing();
	for (std::size_t i = 0; i < model.panel.cards.size(); ++i) {
		const auto &card = model.panel.cards[i];
		controls::InfoCard(card.title.c_str(), card.body.c_str(), card.height);
		if (i + 1 < model.panel.cards.size())
			ImGui::Spacing();
	}
	ImGui::EndChild();
	ImGui::EndChild();
	return result;
}

} // namespace lkmdbg::nativeui
