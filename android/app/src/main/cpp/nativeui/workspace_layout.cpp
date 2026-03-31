#include "nativeui/workspace_layout.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "nativeui/ui_tree.h"
#include "nativeui/workspace_view_model.h"

namespace lkmdbg::nativeui {

namespace {

constexpr int kSectionCount = 5;

} // namespace

std::string WorkspaceLayoutManager::Render(const WorkspaceLabels &labels,
					   const WorkspaceState &state, float density,
					   float delta_time, float time_seconds)
{
	if (!state.expanded) {
		RenderCollapsedBall(density, time_seconds);
		return {};
	}
	return RenderExpandedWorkspace(labels, state, density, delta_time);
}

void WorkspaceLayoutManager::RenderCollapsedBall(float density, float time_seconds)
{
	ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
	const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
			    viewport->Pos.y + viewport->Size.y * 0.5f);
	const float pulse = 0.5f + 0.5f * sinf(time_seconds * 2.2f);
	const float radius = std::min(viewport->Size.x, viewport->Size.y) * 0.5f -
			     (6.0f - pulse * 2.0f) * density;

	draw_list->AddCircleFilled(center, radius, IM_COL32(73, 216, 210, 255), 64);
	draw_list->AddCircleFilled(center, radius - 10.0f * density,
				   IM_COL32(12, 25, 34, 255), 64);
	draw_list->AddCircle(center, radius - 22.0f * density,
			     IM_COL32(73, 216, 210, static_cast<int>(200 + pulse * 55.0f)),
			     64, 6.0f * density);
	draw_list->AddText(ImVec2(center.x - 18.0f * density, center.y - 10.0f * density),
			   IM_COL32(255, 255, 255, 255), "DBG");
}

std::string WorkspaceLayoutManager::RenderExpandedWorkspace(
	const WorkspaceLabels &labels, const WorkspaceState &state,
	float density, float delta_time)
{
	const int clamped_section =
		std::clamp(state.selected_section, 0, kSectionCount - 1);
	animation_manager_.Advance(state.expanded, state.connected, state.session_open,
				   state.hook_active,
				   clamped_section, delta_time);

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
				 ImGuiWindowFlags_NoMove |
				 ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin("lkmdbg_workspace", nullptr, flags);

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const bool portrait = display.x < display.y;
	const WorkspaceViewModel model = BuildWorkspaceViewModel(
		labels, state, static_cast<WorkspaceSectionId>(clamped_section),
		animation_manager_.Snapshot(), portrait, density);
	const WorkspaceUiResult ui_result = RenderWorkspaceUi(model, density);

	ImGui::End();
	return ui_result.action_key;
}

} // namespace lkmdbg::nativeui
