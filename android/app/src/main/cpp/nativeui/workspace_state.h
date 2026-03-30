#pragma once

#include <string>

namespace lkmdbg::nativeui {

struct WorkspaceLabels {
	std::string title = "lkmdbg";
	std::string session = "Session";
	std::string processes = "Processes";
	std::string memory = "Memory";
	std::string threads = "Threads";
	std::string events = "Events";
	std::string connected = "Connected";
	std::string session_open = "Session";
	std::string hook = "Hook";
	std::string process_count = "Processes";
	std::string thread_count = "Threads";
	std::string event_count = "Events";
};

struct WorkspaceState {
	bool expanded = false;
	bool connected = false;
	bool session_open = false;
	int hook_active = 0;
	int process_count = 0;
	int thread_count = 0;
	int event_count = 0;
	float touch_x = 0.0f;
	float touch_y = 0.0f;
	bool touch_down = false;
};

} // namespace lkmdbg::nativeui
