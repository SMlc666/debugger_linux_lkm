#include "nativeui/workspace_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "nativeui/imgui_controls.h"

namespace lkmdbg::nativeui {

void WorkspaceLayoutManager::Render(const WorkspaceLabels &labels,
				    const WorkspaceState &state, float density,
				    float delta_time, float time_seconds)
{
	if (!state.expanded) {
		RenderCollapsedBall(density, time_seconds);
		return;
	}
	RenderExpandedWorkspace(labels, state, density, delta_time);
}

void WorkspaceLayoutManager::RenderCollapsedBall(float density, float time_seconds)
{
	ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImDrawList *draw_list = ImGui::GetBackgroundDrawList(viewport);
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

void WorkspaceLayoutManager::RenderExpandedWorkspace(const WorkspaceLabels &labels,
						     const WorkspaceState &state,
						     float density, float delta_time)
{
	StepRailAnimations(delta_time);
	status_mix_.StepToward(state.connected ? 1.0f : 0.2f, 7.5f, delta_time);
	hook_mix_.StepToward(state.hook_active > 0 ? 1.0f : 0.1f, 7.5f, delta_time);
	for (int i = 0; i < static_cast<int>(lane_highlights_.size()); ++i) {
		lane_highlights_[i].StepToward(i < state.process_count ? 1.0f : 0.0f, 9.0f, delta_time);
	}

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
				 ImGuiWindowFlags_NoMove |
				 ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin("lkmdbg_workspace", nullptr, flags);

	const float rail_width = 132.0f * density;
	ImGui::BeginChild("left_rail", ImVec2(rail_width, 0.0f), ImGuiChildFlags_Borders);
	ImGui::TextUnformatted(labels.title.c_str());
	ImGui::Separator();

	const std::array<const char *, 5> rail_labels = {
		labels.session.c_str(),
		labels.processes.c_str(),
		labels.memory.c_str(),
		labels.threads.c_str(),
		labels.events.c_str(),
	};

	for (int i = 0; i < static_cast<int>(rail_labels.size()); ++i) {
		if (controls::RailButton(rail_labels[i],
					 i == static_cast<int>(selected_section_),
					 density,
					 rail_highlights_[i].value())) {
			selected_section_ = static_cast<Section>(i);
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("main_workspace", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	ImGui::Text("%s", labels.title.c_str());
	ImGui::Spacing();

	ImGui::BeginChild("summary_row", ImVec2(0.0f, 118.0f * density), ImGuiChildFlags_Borders);
	controls::MetricPill(labels.connected.c_str(), state.connected ? "on" : "off",
			     density, status_mix_.value());
	ImGui::SameLine();
	controls::MetricPill(labels.session_open.c_str(), state.session_open ? "on" : "off",
			     density, status_mix_.value());
	ImGui::SameLine();
	char hook_value[16];
	snprintf(hook_value, sizeof(hook_value), "%d", state.hook_active);
	controls::MetricPill(labels.hook.c_str(), hook_value, density, hook_mix_.value());
	ImGui::EndChild();

	ImGui::Spacing();
	ImGui::BeginChild("counts_row", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
	ImGui::Text("%s", rail_labels[static_cast<int>(selected_section_)]);
	ImGui::Separator();
	ImGui::BulletText("%s: %d", labels.process_count.c_str(), state.process_count);
	ImGui::BulletText("%s: %d", labels.thread_count.c_str(), state.thread_count);
	ImGui::BulletText("%s: %d", labels.event_count.c_str(), state.event_count);
	ImGui::Spacing();
	for (int i = 0; i < static_cast<int>(lane_highlights_.size()); ++i) {
		const bool hot = i < state.process_count;
		controls::FillLaneButton(hot ? "process lane" : "empty lane",
					 hot, density, lane_highlights_[i].value());
	}
	ImGui::EndChild();
	ImGui::EndChild();
	ImGui::End();
}

void WorkspaceLayoutManager::StepRailAnimations(float delta_time)
{
	for (int i = 0; i < static_cast<int>(rail_highlights_.size()); ++i) {
		const float target = (i == static_cast<int>(selected_section_)) ? 1.0f : 0.0f;
		rail_highlights_[i].StepToward(target, 8.0f, delta_time);
	}
}

} // namespace lkmdbg::nativeui
