#include "Windows/OscillationCurve.h"

#include "assert/advanced_assert.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

// A nonzero linear share keeps eased curves moving through the turnaround instead of visually dwelling there.
constexpr double CosineLinearShare = 0.6;
constexpr double SmoothstepLinearShare = 0.5;
constexpr double SmootherstepLinearShare = 0.5;

} // namespace

double oscillationCurvePosition(OscillationCurve curve, double phase)
{
	double position = phase;
	switch (curve)
	{
	case OscillationCurve::Linear:
		break;
	case OscillationCurve::Cosine:
		position = CosineLinearShare * phase
			+ (1.0 - CosineLinearShare) * (1.0 - std::cos(std::numbers::pi * phase)) / 2.0;
		break;
	case OscillationCurve::TrueCosine:
		position = (1.0 - std::cos(std::numbers::pi * phase)) / 2.0;
		break;
	case OscillationCurve::Smoothstep:
		position = SmoothstepLinearShare * phase
			+ (1.0 - SmoothstepLinearShare) * phase * phase * (3.0 - 2.0 * phase);
		break;
	case OscillationCurve::Smootherstep:
		position = SmootherstepLinearShare * phase
			+ (1.0 - SmootherstepLinearShare) * phase * phase * phase * (phase * (phase * 6.0 - 15.0) + 10.0);
		break;
	}
	return std::clamp(position, 0.0, 1.0);
}

double oscillationCurvePeakSlope(OscillationCurve curve)
{
	switch (curve)
	{
	case OscillationCurve::Linear:
		return 1.0;
	case OscillationCurve::Cosine:
		return CosineLinearShare + (1.0 - CosineLinearShare) * std::numbers::pi / 2.0;
	case OscillationCurve::TrueCosine:
		return std::numbers::pi / 2.0;
	case OscillationCurve::Smoothstep:
		return SmoothstepLinearShare + (1.0 - SmoothstepLinearShare) * 1.5;
	case OscillationCurve::Smootherstep:
		return SmootherstepLinearShare + (1.0 - SmootherstepLinearShare) * 1.875;
	}
	assert_r(false);
	return 1.0;
}
