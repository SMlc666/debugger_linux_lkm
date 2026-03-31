#include "nativeui/ui_format.h"

#include <fmt/format.h>

namespace lkmdbg::nativeui::ui_format {

std::string JoinTitle(std::string_view left, std::string_view right)
{
	return fmt::format("{} · {}", left, right);
}

std::string KeyValue(std::string_view label, std::string_view value)
{
	return fmt::format("{}: {}", label, value);
}

std::string KeyValue(std::string_view label, int value)
{
	return fmt::format("{}: {}", label, value);
}

} // namespace lkmdbg::nativeui::ui_format
