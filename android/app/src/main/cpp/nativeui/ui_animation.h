#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace lkmdbg::nativeui {

enum class AnimationCurve {
	Linear = 0,
	Smooth = 1,
};

struct AnimationConfig {
	float speed = 8.0f;
	AnimationCurve curve = AnimationCurve::Smooth;
};

class AnimatedFloat {
public:
	AnimatedFloat() : value_(0.0f), target_(0.0f) {}
	explicit AnimatedFloat(float value) : value_(value), target_(value) {}

	void Snap(float value) { value_ = value; }

	void SetTarget(float target) { target_ = target; }

	void Step(const AnimationConfig &config, float delta_time)
	{
		float alpha = std::clamp(config.speed * delta_time, 0.0f, 1.0f);
		if (config.curve == AnimationCurve::Smooth)
			alpha = alpha * alpha * (3.0f - 2.0f * alpha);
		value_ += (target_ - value_) * alpha;
	}

	void StepToward(float target, float speed, float delta_time)
	{
		StepToward(target, AnimationConfig{speed, AnimationCurve::Smooth}, delta_time);
	}

	void StepToward(float target, const AnimationConfig &config, float delta_time)
	{
		target_ = target;
		Step(config, delta_time);
	}

	float value() const { return value_; }
	float target() const { return target_; }

private:
	float value_;
	float target_;
};

template <std::size_t N> class AnimatedFloatArray {
public:
	AnimatedFloat &operator[](std::size_t index) { return values_[index]; }
	const AnimatedFloat &operator[](std::size_t index) const { return values_[index]; }

	void SnapAll(float value)
	{
		for (auto &entry : values_)
			entry.Snap(value);
	}

	std::array<float, N> Snapshot() const
	{
		std::array<float, N> out{};
		for (std::size_t i = 0; i < N; ++i)
			out[i] = values_[i].value();
		return out;
	}

private:
	std::array<AnimatedFloat, N> values_{};
};

} // namespace lkmdbg::nativeui
