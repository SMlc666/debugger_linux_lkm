#pragma once

#include <array>

#include "nativeui/ui_animation.h"

namespace lkmdbg::nativeui {

constexpr int kWorkspaceSectionCount = 5;

struct WorkspaceAnimationSnapshot {
	std::array<float, kWorkspaceSectionCount> section_mix{};
	float connected_mix = 0.0f;
	float session_mix = 0.0f;
	float hook_mix = 0.0f;
	float expanded_mix = 0.0f;
};

class WorkspaceAnimationManager {
public:
	void Advance(bool expanded, bool connected, bool session_open, int hook_active,
		     int selected_section, float delta_time);

	WorkspaceAnimationSnapshot Snapshot() const;

private:
	AnimatedFloatArray<kWorkspaceSectionCount> section_mix_;
	AnimatedFloat connected_mix_{0.0f};
	AnimatedFloat session_mix_{0.0f};
	AnimatedFloat hook_mix_{0.0f};
	AnimatedFloat expanded_mix_{0.0f};
};

} // namespace lkmdbg::nativeui
