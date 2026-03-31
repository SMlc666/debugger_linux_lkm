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
	std::string session_subtitle = "Connect the agent, open a session fd, and attach a target.";
	std::string processes_subtitle = "Browse and attach without typing a pid.";
	std::string memory_subtitle = "Inspect pages, ranges, images, and search results.";
	std::string threads_subtitle = "Refresh threads and review register snapshots.";
	std::string events_subtitle = "Drain the current session event queue.";
	std::string connected = "Connected";
	std::string session_open = "Session";
	std::string hook = "Hook";
	std::string process_count = "Processes";
	std::string thread_count = "Threads";
	std::string event_count = "Events";
	std::string bool_yes = "yes";
	std::string bool_no = "no";
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
