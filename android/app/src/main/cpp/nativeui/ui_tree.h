#pragma once

#include <string>

#include "nativeui/workspace_view_model.h"

namespace lkmdbg::nativeui {

struct WorkspaceUiResult {
	std::string action_key;
};

WorkspaceUiResult RenderWorkspaceUi(const WorkspaceViewModel &model, float density);

} // namespace lkmdbg::nativeui
