#pragma once

#include <array>
#include <string>
#include <vector>

#include "nativeui/imgui_controls.h"
#include "nativeui/workspace_animation.h"
#include "nativeui/workspace_state.h"

namespace lkmdbg::nativeui {

enum class WorkspaceSectionId : int {
	Session = 0,
	Processes = 1,
	Memory = 2,
	Threads = 3,
	Events = 4,
};

struct WorkspaceCardViewModel {
	std::string title;
	std::string body;
	float height = 0.0f;
};

struct WorkspaceActionViewModel {
	std::string key;
	std::string label;
	bool active = false;
};

struct WorkspaceListItemViewModel {
	std::string key;
	std::string title;
	std::string subtitle;
	std::string badge;
	bool selected = false;
	float height = 0.0f;
};

struct WorkspacePanelViewModel {
	std::string title;
	std::string subtitle;
	std::vector<controls::StatItem> stats;
	std::vector<WorkspaceActionViewModel> primary_actions;
	std::vector<WorkspaceActionViewModel> secondary_actions;
	std::vector<WorkspaceListItemViewModel> list_entries;
	std::vector<WorkspaceListItemViewModel> detail_entries;
	std::vector<std::string> detail_lines;
	std::vector<WorkspaceCardViewModel> cards;
};

struct WorkspaceViewModel {
	bool portrait = true;
	std::string rail_title;
	std::string workspace_title;
	std::string workspace_subtitle;
	std::array<controls::SectionItem, kWorkspaceSectionCount> sections{};
	std::array<controls::MetricItem, 3> metrics{};
	WorkspacePanelViewModel panel;
};

WorkspaceViewModel BuildWorkspaceViewModel(const WorkspaceLabels &labels,
					   const WorkspaceState &state,
					   WorkspaceSectionId selected_section,
					   const WorkspaceAnimationSnapshot &animation,
					   bool portrait, float density);

} // namespace lkmdbg::nativeui
