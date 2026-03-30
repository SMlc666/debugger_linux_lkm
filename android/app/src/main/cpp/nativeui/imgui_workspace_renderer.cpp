#include "nativeui/imgui_workspace_renderer.h"

#include <GLES3/gl3.h>
#include <time.h>

#include <algorithm>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

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

void ImGuiWorkspaceRenderer::UpdateState(bool expanded, bool connected,
					  bool session_open, int hook_active,
					  int process_count, int thread_count,
					  int event_count)
{
	std::scoped_lock lock(mutex_);
	state_.expanded = expanded;
	state_.connected = connected;
	state_.session_open = session_open;
	state_.hook_active = hook_active;
	state_.process_count = process_count;
	state_.thread_count = thread_count;
	state_.event_count = event_count;
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
		glClearColor(0.03f, 0.07f, 0.10f, 0.98f);
	else
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	egl_.SwapBuffers();
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
		ImGui::StyleColorsDark();
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
	ImGuiStyle &style = ImGui::GetStyle();
	style.WindowRounding = 22.0f;
	style.ChildRounding = 18.0f;
	style.FrameRounding = 14.0f;
	style.PopupRounding = 12.0f;
	style.ScrollbarRounding = 10.0f;
	style.GrabRounding = 10.0f;
	style.WindowPadding = ImVec2(18.0f, 18.0f);
	style.ItemSpacing = ImVec2(12.0f, 10.0f);
	style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.10f, 0.13f, 0.96f);
	style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.15f, 0.19f, 0.92f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.25f, 0.31f, 1.0f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.73f, 0.78f, 0.85f);
	style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.82f, 0.86f, 1.0f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.11f, 0.23f, 0.28f, 1.0f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.23f, 0.71f, 0.76f, 0.92f);
	style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.80f, 0.84f, 1.0f);
	style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.19f, 0.24f, 1.0f);
	style.Colors[ImGuiCol_Border] = ImVec4(0.21f, 0.42f, 0.48f, 0.7f);
	style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.97f, 0.98f, 1.0f);
	style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.66f, 0.70f, 1.0f);
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
						 io.Fonts->GetGlyphRangesChineseSimplifiedCommon()) != nullptr) {
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
	layout_.Render(labels_, state_, density_, ImGui::GetIO().DeltaTime,
		      static_cast<float>(last_frame_time_));
}

} // namespace lkmdbg::nativeui
