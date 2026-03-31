#include "nativeui/workspace_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "nativeui/imgui_controls.h"

namespace lkmdbg::nativeui {

namespace {

constexpr int kSectionCount = 5;

using Section = WorkspaceLayoutManager::Section;

const char *BoolText(const WorkspaceLabels &labels, bool value)
{
	return value ? labels.bool_yes.c_str() : labels.bool_no.c_str();
}

const char *SectionTitle(const WorkspaceLabels &labels, Section section)
{
	switch (section) {
	case Section::Session:
		return labels.session.c_str();
	case Section::Processes:
		return labels.processes.c_str();
	case Section::Memory:
		return labels.memory.c_str();
	case Section::Threads:
		return labels.threads.c_str();
	case Section::Events:
	default:
		return labels.events.c_str();
	}
}

const char *SectionSubtitle(const WorkspaceLabels &labels, Section section)
{
	switch (section) {
	case Section::Session:
		return labels.session_subtitle.c_str();
	case Section::Processes:
		return labels.processes_subtitle.c_str();
	case Section::Memory:
		return labels.memory_subtitle.c_str();
	case Section::Threads:
		return labels.threads_subtitle.c_str();
	case Section::Events:
	default:
		return labels.events_subtitle.c_str();
	}
}

std::array<controls::SectionItem, kSectionCount>
BuildSectionItems(const std::array<const char *, kSectionCount> &labels,
		  const std::array<AnimatedFloat, kSectionCount> &highlights,
		  Section selected)
{
	std::array<controls::SectionItem, kSectionCount> items{};
	for (int i = 0; i < kSectionCount; ++i) {
		items[i] = controls::SectionItem{
			.label = labels[i],
			.selected = i == static_cast<int>(selected),
			.highlight_mix = highlights[i].value(),
		};
	}
	return items;
}

} // namespace

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

void WorkspaceLayoutManager::RenderExpandedWorkspace(const WorkspaceLabels &labels,
						     const WorkspaceState &state,
						     float density, float delta_time)
{
	StepRailAnimations(delta_time);
	status_mix_.StepToward(state.connected ? 1.0f : 0.2f, 7.5f, delta_time);
	hook_mix_.StepToward(state.hook_active > 0 ? 1.0f : 0.1f, 7.5f, delta_time);

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
				 ImGuiWindowFlags_NoMove |
				 ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin("lkmdbg_workspace", nullptr, flags);

	const std::array<const char *, 5> rail_labels = {
		labels.session.c_str(),
		labels.processes.c_str(),
		labels.memory.c_str(),
		labels.threads.c_str(),
		labels.events.c_str(),
	};

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const bool portrait = display.x < display.y;
	const auto section_items =
		BuildSectionItems(rail_labels, rail_highlights_, selected_section_);

	if (portrait) {
		const int pressed = controls::SectionTabs(section_items.data(), kSectionCount, density);
		if (pressed >= 0)
			selected_section_ = static_cast<Section>(pressed);
		ImGui::Spacing();
	} else {
		const int pressed =
			controls::SectionRail(labels.title.c_str(), section_items.data(),
					      kSectionCount, density);
		if (pressed >= 0)
			selected_section_ = static_cast<Section>(pressed);
		ImGui::SameLine();
	}

	ImGui::BeginChild("main_workspace", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	char workspace_title[192];
	snprintf(workspace_title, sizeof(workspace_title), "%s · %s",
		 labels.title.c_str(), SectionTitle(labels, selected_section_));
	controls::SectionHeader(workspace_title,
				SectionSubtitle(labels, selected_section_));
	ImGui::Spacing();

	char hook_value[16];
	snprintf(hook_value, sizeof(hook_value), "%d", state.hook_active);
	controls::MetricStrip(labels.connected.c_str(),
			      BoolText(labels, state.connected),
			      labels.session_open.c_str(),
			      BoolText(labels, state.session_open),
			      labels.hook.c_str(),
			      hook_value,
			      density,
			      status_mix_.value(),
			      status_mix_.value(),
			      hook_mix_.value());

	ImGui::Spacing();
	ImGui::BeginChild("section_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
	controls::SectionHeader(SectionTitle(labels, selected_section_),
				SectionSubtitle(labels, selected_section_));
	ImGui::Separator();

	switch (selected_section_) {
	case Section::Session:
		controls::StatLine(labels.connected.c_str(),
				   BoolText(labels, state.connected));
		controls::StatLine(labels.session_open.c_str(),
				   BoolText(labels, state.session_open));
		controls::StatLineValue(labels.hook.c_str(), state.hook_active);
		controls::StatLineValue(labels.process_count.c_str(), state.process_count);
		ImGui::Spacing();
		controls::InfoCard(labels.session.c_str(),
				   SectionSubtitle(labels, selected_section_),
				   92.0f * density);
		ImGui::Spacing();
		controls::InfoCard(labels.events.c_str(),
				   SectionSubtitle(labels, Section::Events),
				   92.0f * density);
		break;
	case Section::Processes:
		controls::StatLineValue(labels.process_count.c_str(), state.process_count);
		controls::StatLineValue(labels.thread_count.c_str(), state.thread_count);
		controls::StatLine(labels.session_open.c_str(),
				   BoolText(labels, state.session_open));
		ImGui::Spacing();
		controls::InfoCard(labels.processes.c_str(),
				   SectionSubtitle(labels, selected_section_),
				   92.0f * density);
		ImGui::Spacing();
		controls::InfoCard(labels.session.c_str(),
				   SectionSubtitle(labels, Section::Session),
				   92.0f * density);
		break;
	case Section::Memory:
		controls::StatLine(labels.session_open.c_str(),
				   BoolText(labels, state.session_open));
		controls::StatLineValue(labels.process_count.c_str(), state.process_count);
		controls::StatLineValue(labels.thread_count.c_str(), state.thread_count);
		ImGui::Spacing();
		controls::InfoCard(labels.memory.c_str(),
				   SectionSubtitle(labels, selected_section_),
				   96.0f * density);
		ImGui::Spacing();
		controls::InfoCard(labels.processes.c_str(),
				   SectionSubtitle(labels, Section::Processes),
				   92.0f * density);
		break;
	case Section::Threads:
		controls::StatLineValue(labels.thread_count.c_str(), state.thread_count);
		controls::StatLineValue(labels.event_count.c_str(), state.event_count);
		controls::StatLine(labels.session_open.c_str(),
				   BoolText(labels, state.session_open));
		ImGui::Spacing();
		controls::InfoCard(labels.threads.c_str(),
				   SectionSubtitle(labels, selected_section_),
				   92.0f * density);
		ImGui::Spacing();
		controls::InfoCard(labels.memory.c_str(),
				   SectionSubtitle(labels, Section::Memory),
				   92.0f * density);
		break;
	case Section::Events:
	default:
		controls::StatLineValue(labels.event_count.c_str(), state.event_count);
		controls::StatLineValue(labels.process_count.c_str(), state.process_count);
		controls::StatLine(labels.session_open.c_str(),
				   BoolText(labels, state.session_open));
		ImGui::Spacing();
		controls::InfoCard(labels.events.c_str(),
				   SectionSubtitle(labels, selected_section_),
				   92.0f * density);
		ImGui::Spacing();
		controls::InfoCard(labels.session.c_str(),
				   SectionSubtitle(labels, Section::Session),
				   92.0f * density);
		break;
	}

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
