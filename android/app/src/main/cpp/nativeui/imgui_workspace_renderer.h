#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <android/native_window.h>

#include "nativeui/egl_window_context.h"
#include "nativeui/workspace_layout.h"
#include "nativeui/workspace_state.h"

namespace lkmdbg::nativeui {

class ImGuiWorkspaceRenderer {
public:
	ImGuiWorkspaceRenderer();
	~ImGuiWorkspaceRenderer();

	ImGuiWorkspaceRenderer(const ImGuiWorkspaceRenderer &) = delete;
	ImGuiWorkspaceRenderer &operator=(const ImGuiWorkspaceRenderer &) = delete;

	void SetSurface(ANativeWindow *window);
	void Resize(int width, int height, float density);
	void UpdateState(bool expanded, bool busy, int selected_section, bool connected,
			 bool session_open, int hook_active, int target_pid, int target_tid,
			 int event_queue_depth, int process_count, int thread_count,
			 int event_count, int image_count, int vma_count,
			 std::string session_primary, std::string session_secondary,
			 std::string process_primary, std::string process_secondary,
			 std::string memory_primary, std::string memory_secondary,
			 std::string thread_primary, std::string thread_secondary,
			 std::string event_primary, std::string event_secondary,
			 std::vector<WorkspaceActionChip> process_action_chips,
			 std::vector<WorkspaceListEntry> process_entries,
			 std::vector<WorkspaceActionChip> memory_action_chips,
			 std::vector<WorkspaceActionChip> memory_page_action_chips,
			 std::vector<WorkspaceListEntry> memory_result_entries,
			 std::vector<WorkspaceListEntry> memory_page_entries,
			 std::vector<std::string> memory_scalar_entries,
			 std::string footer_message);
	void UpdateLabels(const WorkspaceLabels &labels);
	void UpdateFontPaths(std::vector<std::string> font_paths);
	void OnTouch(int action, float x, float y);
	std::string ConsumeAction();
	void Render();

private:
	static double MonotonicSeconds();

	bool EnsureReadyLocked();
	void ShutdownLocked();
	void ApplyStyleLocked();
	void RebuildFontsLocked();
	void UpdateIoLocked();
	void BuildUiLocked();

	std::mutex mutex_;
	EglWindowContext egl_;
	WorkspaceState state_;
	WorkspaceLabels labels_;
	WorkspaceLayoutManager layout_;
	std::vector<std::string> font_paths_;
	std::vector<std::string> pending_actions_;
	float density_ = 1.0f;
	double last_frame_time_ = 0.0;
	bool imgui_ready_ = false;
	bool fonts_dirty_ = true;
};

} // namespace lkmdbg::nativeui
