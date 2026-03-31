#pragma once

#include "nativeui/workspace_view_model.h"

namespace lkmdbg::nativeui {

struct WorkspaceUiResult {
	int selected_section = -1;
};

WorkspaceUiResult RenderWorkspaceUi(const WorkspaceViewModel &model, float density);

} // namespace lkmdbg::nativeui
