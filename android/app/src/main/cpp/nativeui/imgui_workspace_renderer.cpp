#include "nativeui/imgui_workspace_renderer.h"

#include <GLES3/gl3.h>
#include <time.h>

#include <algorithm>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "nativeui/workspace_theme.h"

namespace lkmdbg::nativeui {

ImGuiWorkspaceRenderer::ImGuiWorkspaceRenderer() = default;

ImGuiWorkspaceRenderer::~ImGuiWorkspaceRenderer()
{
	std::scoped_lock lock(mutex_);
	ShutdownLocked();
}

void ImGuiWorkspaceRenderer::SetSurface(ANativeWindow *window)
{
	std::scoped_lock lock(mutex_);
	egl_.SetWindow(window);
}

void ImGuiWorkspaceRenderer::Resize(int width, int height, float density)
{
	std::scoped_lock lock(mutex_);
	egl_.Resize(width, height);
	density_ = std::max(1.0f, density);
	fonts_dirty_ = true;
}

void ImGuiWorkspaceRenderer::UpdateState(bool expanded, bool busy, int selected_section,
					  bool connected, bool session_open, int hook_active,
					  int target_pid, int target_tid,
					  int event_queue_depth, int process_count,
					  int thread_count, int event_count,
					  int image_count, int vma_count,
					  std::string session_primary,
					  std::string session_secondary,
					  std::string process_primary,
					  std::string process_secondary,
					  std::string memory_primary,
					  std::string memory_secondary,
					  std::string thread_primary,
					  std::string thread_secondary,
					  std::string event_primary,
					  std::string event_secondary,
					  std::vector<WorkspaceActionChip> process_action_chips,
					  std::vector<WorkspaceListEntry> process_entries,
					  std::vector<WorkspaceActionChip> memory_action_chips,
					  std::vector<WorkspaceActionChip> memory_page_action_chips,
					  std::vector<WorkspaceListEntry> memory_result_entries,
					  std::vector<WorkspaceListEntry> memory_page_entries,
					  std::vector<std::string> memory_scalar_entries,
					  std::string footer_message)
{
	std::scoped_lock lock(mutex_);
	state_.expanded = expanded;
	state_.busy = busy;
	state_.selected_section = selected_section;
	state_.connected = connected;
	state_.session_open = session_open;
	state_.hook_active = hook_active;
	state_.target_pid = target_pid;
	state_.target_tid = target_tid;
	state_.event_queue_depth = event_queue_depth;
	state_.process_count = process_count;
	state_.thread_count = thread_count;
	state_.event_count = event_count;
	state_.image_count = image_count;
	state_.vma_count = vma_count;
	state_.session_primary = std::move(session_primary);
	state_.session_secondary = std::move(session_secondary);
	state_.process_primary = std::move(process_primary);
	state_.process_secondary = std::move(process_secondary);
	state_.memory_primary = std::move(memory_primary);
	state_.memory_secondary = std::move(memory_secondary);
	state_.thread_primary = std::move(thread_primary);
	state_.thread_secondary = std::move(thread_secondary);
	state_.event_primary = std::move(event_primary);
	state_.event_secondary = std::move(event_secondary);
	state_.process_action_chips = std::move(process_action_chips);
	state_.process_entries = std::move(process_entries);
	state_.memory_action_chips = std::move(memory_action_chips);
	state_.memory_page_action_chips = std::move(memory_page_action_chips);
	state_.memory_result_entries = std::move(memory_result_entries);
	state_.memory_page_entries = std::move(memory_page_entries);
	state_.memory_scalar_entries = std::move(memory_scalar_entries);
	state_.footer_message = std::move(footer_message);
}

void ImGuiWorkspaceRenderer::UpdateLabels(const WorkspaceLabels &labels)
{
	std::scoped_lock lock(mutex_);
	labels_ = labels;
}

void ImGuiWorkspaceRenderer::UpdateFontPaths(std::vector<std::string> font_paths)
{
	std::scoped_lock lock(mutex_);
	font_paths_ = std::move(font_paths);
	fonts_dirty_ = true;
}

void ImGuiWorkspaceRenderer::OnTouch(int action, float x, float y)
{
	std::scoped_lock lock(mutex_);
	state_.touch_x = x;
	state_.touch_y = y;
	switch (action) {
	case 0:
	case 2:
		state_.touch_down = true;
		break;
	case 1:
	case 3:
	default:
		state_.touch_down = false;
		break;
	}
}

void ImGuiWorkspaceRenderer::Render()
{
	std::scoped_lock lock(mutex_);
	if (!EnsureReadyLocked())
		return;

	UpdateIoLocked();
	if (fonts_dirty_)
		RebuildFontsLocked();

	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();
	BuildUiLocked();
	ImGui::Render();

	glViewport(0, 0, egl_.width(), egl_.height());
	if (state_.expanded)
		glClearColor(GetWorkspaceTheme().background.x, GetWorkspaceTheme().background.y,
			     GetWorkspaceTheme().background.z, 0.98f);
	else
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	egl_.SwapBuffers();
}

std::string ImGuiWorkspaceRenderer::ConsumeAction()
{
	std::scoped_lock lock(mutex_);
	if (pending_actions_.empty())
		return {};
	std::string action = std::move(pending_actions_.front());
	pending_actions_.erase(pending_actions_.begin());
	return action;
}

double ImGuiWorkspaceRenderer::MonotonicSeconds()
{
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) +
	       static_cast<double>(ts.tv_nsec) / 1000000000.0;
}

bool ImGuiWorkspaceRenderer::EnsureReadyLocked()
{
	if (!egl_.EnsureCurrent())
		return false;

	if (!imgui_ready_) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.BackendPlatformName = "lkmdbg_native_host";
		ApplyStyleLocked();
		if (!ImGui_ImplOpenGL3_Init("#version 300 es"))
			return false;
		imgui_ready_ = true;
	}

	return true;
}

void ImGuiWorkspaceRenderer::ShutdownLocked()
{
	if (imgui_ready_) {
		ImGui_ImplOpenGL3_Shutdown();
		ImGui::DestroyContext();
		imgui_ready_ = false;
	}
	egl_.Shutdown();
}

void ImGuiWorkspaceRenderer::ApplyStyleLocked()
{
	const WorkspaceTheme &theme = GetWorkspaceTheme();
	ImGuiStyle &style = ImGui::GetStyle();
	style = ImGuiStyle{};
	style.WindowRounding = 22.0f;
	style.ChildRounding = 18.0f;
	style.FrameRounding = 14.0f;
	style.PopupRounding = 12.0f;
	style.ScrollbarRounding = 10.0f;
	style.GrabRounding = 10.0f;
	style.TabRounding = 14.0f;
	style.WindowPadding = ImVec2(18.0f, 18.0f);
	style.ItemSpacing = ImVec2(12.0f, 10.0f);
	style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f;
	style.Colors[ImGuiCol_WindowBg] = WithAlpha(theme.background, 0.97f);
	style.Colors[ImGuiCol_ChildBg] = WithAlpha(theme.surface, 0.94f);
	style.Colors[ImGuiCol_PopupBg] = WithAlpha(theme.surface_container_high, 0.98f);
	style.Colors[ImGuiCol_Border] = WithAlpha(theme.outline_variant, 0.92f);
	style.Colors[ImGuiCol_BorderShadow] = WithAlpha(theme.background, 0.0f);
	style.Colors[ImGuiCol_FrameBg] = WithAlpha(theme.surface_container_high, 1.0f);
	style.Colors[ImGuiCol_FrameBgHovered] = WithAlpha(theme.surface_variant, 1.0f);
	style.Colors[ImGuiCol_FrameBgActive] = WithAlpha(theme.primary_container, 1.0f);
	style.Colors[ImGuiCol_TitleBg] = WithAlpha(theme.surface, 1.0f);
	style.Colors[ImGuiCol_TitleBgActive] = WithAlpha(theme.surface_container, 1.0f);
	style.Colors[ImGuiCol_Header] = WithAlpha(theme.surface_container, 1.0f);
	style.Colors[ImGuiCol_HeaderHovered] = WithAlpha(theme.primary_container, 0.95f);
	style.Colors[ImGuiCol_HeaderActive] = WithAlpha(theme.primary, 0.92f);
	style.Colors[ImGuiCol_Button] = WithAlpha(theme.surface_container_high, 1.0f);
	style.Colors[ImGuiCol_ButtonHovered] = WithAlpha(theme.primary_container, 0.96f);
	style.Colors[ImGuiCol_ButtonActive] = WithAlpha(theme.primary, 0.90f);
	style.Colors[ImGuiCol_Separator] = WithAlpha(theme.outline_variant, 1.0f);
	style.Colors[ImGuiCol_ResizeGrip] = WithAlpha(theme.primary_container, 0.65f);
	style.Colors[ImGuiCol_ResizeGripHovered] = WithAlpha(theme.primary, 0.85f);
	style.Colors[ImGuiCol_ResizeGripActive] = WithAlpha(theme.primary, 1.0f);
	style.Colors[ImGuiCol_ScrollbarBg] = WithAlpha(theme.surface, 0.85f);
	style.Colors[ImGuiCol_ScrollbarGrab] = WithAlpha(theme.surface_variant, 1.0f);
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = WithAlpha(theme.primary_container, 1.0f);
	style.Colors[ImGuiCol_ScrollbarGrabActive] = WithAlpha(theme.primary, 1.0f);
	style.Colors[ImGuiCol_CheckMark] = theme.primary;
	style.Colors[ImGuiCol_SliderGrab] = theme.secondary;
	style.Colors[ImGuiCol_SliderGrabActive] = theme.secondary_container;
	style.Colors[ImGuiCol_Text] = theme.on_surface;
	style.Colors[ImGuiCol_TextDisabled] = theme.on_surface_variant;
	style.Colors[ImGuiCol_NavHighlight] = theme.secondary;
}

void ImGuiWorkspaceRenderer::RebuildFontsLocked()
{
	ImGuiIO &io = ImGui::GetIO();
	io.Fonts->Clear();

	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	config.PixelSnapH = false;
	bool loaded = false;

	for (const std::string &path : font_paths_) {
		if (path.empty())
			continue;
		if (io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f * density_, &config,
						 io.Fonts->GetGlyphRangesChineseFull()) != nullptr) {
			loaded = true;
			break;
		}
	}

	if (!loaded)
		io.Fonts->AddFontDefault();

	ImGui_ImplOpenGL3_DestroyDeviceObjects();
	ImGui_ImplOpenGL3_CreateDeviceObjects();
	fonts_dirty_ = false;
}

void ImGuiWorkspaceRenderer::UpdateIoLocked()
{
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(egl_.width()),
				static_cast<float>(egl_.height()));
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	const double now = MonotonicSeconds();
	io.DeltaTime = last_frame_time_ > 0.0 ? static_cast<float>(now - last_frame_time_) : (1.0f / 60.0f);
	last_frame_time_ = now;
	io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
	io.AddMousePosEvent(state_.touch_x, state_.touch_y);
	io.AddMouseButtonEvent(0, state_.touch_down);
}

void ImGuiWorkspaceRenderer::BuildUiLocked()
{
	const std::string action = layout_.Render(labels_, state_, density_,
					    ImGui::GetIO().DeltaTime,
					    static_cast<float>(last_frame_time_));
	if (!action.empty())
		pending_actions_.push_back(action);
}

} // namespace lkmdbg::nativeui
