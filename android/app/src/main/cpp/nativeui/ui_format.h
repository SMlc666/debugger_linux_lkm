#pragma once

#include <string>
#include <string_view>

namespace lkmdbg::nativeui::ui_format {

std::string JoinTitle(std::string_view left, std::string_view right);
std::string KeyValue(std::string_view label, std::string_view value);
std::string KeyValue(std::string_view label, int value);

} // namespace lkmdbg::nativeui::ui_format
