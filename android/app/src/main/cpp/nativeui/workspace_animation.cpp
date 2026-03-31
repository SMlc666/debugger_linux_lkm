#include "nativeui/workspace_animation.h"

namespace lkmdbg::nativeui {

namespace {

constexpr AnimationConfig kSectionConfig{8.5f, AnimationCurve::Smooth};
constexpr AnimationConfig kMetricConfig{7.5f, AnimationCurve::Smooth};
constexpr AnimationConfig kExpandedConfig{6.0f, AnimationCurve::Smooth};

} // namespace

void WorkspaceAnimationManager::Advance(bool expanded, bool connected,
					bool session_open, int hook_active,
					int selected_section, float delta_time)
{
	for (int i = 0; i < kWorkspaceSectionCount; ++i) {
		const float target = i == selected_section ? 1.0f : 0.0f;
		section_mix_[i].StepToward(target, kSectionConfig, delta_time);
	}
	connected_mix_.StepToward(connected ? 1.0f : 0.2f, kMetricConfig, delta_time);
	session_mix_.StepToward(session_open ? 1.0f : 0.2f, kMetricConfig, delta_time);
	hook_mix_.StepToward(hook_active > 0 ? 1.0f : 0.1f, kMetricConfig, delta_time);
	expanded_mix_.StepToward(expanded ? 1.0f : 0.0f, kExpandedConfig, delta_time);
}

WorkspaceAnimationSnapshot WorkspaceAnimationManager::Snapshot() const
{
	WorkspaceAnimationSnapshot snapshot;
	snapshot.section_mix = section_mix_.Snapshot();
	snapshot.connected_mix = connected_mix_.value();
	snapshot.session_mix = session_mix_.value();
	snapshot.hook_mix = hook_mix_.value();
	snapshot.expanded_mix = expanded_mix_.value();
	return snapshot;
}

} // namespace lkmdbg::nativeui
