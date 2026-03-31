#include "nativeui/ui_tree.h"

#include "imgui.h"
#include "nativeui/imgui_controls.h"

namespace lkmdbg::nativeui {

WorkspaceUiResult RenderWorkspaceUi(const WorkspaceViewModel &model, float density)
{
	WorkspaceUiResult result{};

	if (model.portrait) {
		result.selected_section =
			controls::SectionTabs(model.sections.data(), kWorkspaceSectionCount, density);
		ImGui::Spacing();
	} else {
		result.selected_section = controls::SectionRail(
			model.rail_title.c_str(), model.sections.data(), kWorkspaceSectionCount,
			density);
		ImGui::SameLine();
	}

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
