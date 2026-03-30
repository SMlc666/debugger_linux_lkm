#pragma once

#include <algorithm>

namespace lkmdbg::nativeui {

class AnimatedFloat {
public:
	explicit AnimatedFloat(float value = 0.0f) : value_(value) {}

	void Snap(float value) { value_ = value; }

	void StepToward(float target, float speed, float delta_time)
	{
		const float alpha = std::clamp(speed * delta_time, 0.0f, 1.0f);
		value_ += (target - value_) * alpha;
	}

	float value() const { return value_; }

private:
	float value_;
};

} // namespace lkmdbg::nativeui
