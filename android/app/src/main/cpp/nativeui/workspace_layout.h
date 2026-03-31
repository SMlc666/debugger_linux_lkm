#pragma once

#include <string>

#include "nativeui/workspace_animation.h"
#include "nativeui/workspace_state.h"

namespace lkmdbg::nativeui {

class WorkspaceLayoutManager {
public:
	std::string Render(const WorkspaceLabels &labels, const WorkspaceState &state,
			   float density, float delta_time, float time_seconds);

private:
	void RenderCollapsedBall(float density, float time_seconds);
	std::string RenderExpandedWorkspace(const WorkspaceLabels &labels,
					    const WorkspaceState &state,
					    float density, float delta_time);

	WorkspaceAnimationManager animation_manager_{};
};

} // namespace lkmdbg::nativeui
