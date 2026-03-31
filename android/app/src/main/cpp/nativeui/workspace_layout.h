#pragma once

#include <array>

#include "nativeui/ui_animation.h"
#include "nativeui/workspace_state.h"

namespace lkmdbg::nativeui {

class WorkspaceLayoutManager {
public:
	enum class Section : int {
		Session = 0,
		Processes = 1,
		Memory = 2,
		Threads = 3,
		Events = 4,
	};

	void Render(const WorkspaceLabels &labels, const WorkspaceState &state,
		    float density, float delta_time, float time_seconds);

private:
	void RenderCollapsedBall(float density, float time_seconds);
	void RenderExpandedWorkspace(const WorkspaceLabels &labels,
				     const WorkspaceState &state,
				     float density, float delta_time);
	void StepRailAnimations(float delta_time);

	Section selected_section_ = Section::Session;
	std::array<AnimatedFloat, 5> rail_highlights_ = {
		AnimatedFloat(1.0f),
		AnimatedFloat(0.0f),
		AnimatedFloat(0.0f),
		AnimatedFloat(0.0f),
		AnimatedFloat(0.0f),
	};
	AnimatedFloat status_mix_{0.0f};
	AnimatedFloat hook_mix_{0.0f};
};

} // namespace lkmdbg::nativeui
