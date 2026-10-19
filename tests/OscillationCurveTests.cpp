#include "Windows/OscillationCurve.h"
#include "TestHelpers.h"

#include <algorithm>

namespace {

constexpr OscillationCurve AllCurves[]{
	OscillationCurve::Linear, OscillationCurve::Cosine, OscillationCurve::TrueCosine, OscillationCurve::Smoothstep, OscillationCurve::Smootherstep
};
constexpr int SampleCount = 10000;

[[nodiscard]] double samplePhase(int i)
{
	return static_cast<double>(i) / SampleCount;
}

} // namespace

TEST_CASE("oscillationCurvePosition: runs from 0 to 1 without moving backwards", "[oscillation]")
{
	for (const OscillationCurve curve : AllCurves)
	{
		CAPTURE(static_cast<int>(curve));
		REQUIRE(oscillationCurvePosition(curve, 0.0) == Approx(0.0).margin(1e-12));
		REQUIRE(oscillationCurvePosition(curve, 1.0) == Approx(1.0));

		int firstDecreaseAt = -1;
		for (int i = 1; i <= SampleCount && firstDecreaseAt < 0; ++i)
		{
			if (oscillationCurvePosition(curve, samplePhase(i)) < oscillationCurvePosition(curve, samplePhase(i - 1)))
				firstDecreaseAt = i;
		}
		REQUIRE(firstDecreaseAt == -1);
	}
}

TEST_CASE("oscillationCurvePeakSlope: matches the steepest sampled slope of the curve", "[oscillation]")
{
	for (const OscillationCurve curve : AllCurves)
	{
		CAPTURE(static_cast<int>(curve));
		double steepestSlope = 0;
		for (int i = 0; i < SampleCount; ++i)
		{
			const double rise = oscillationCurvePosition(curve, samplePhase(i + 1)) - oscillationCurvePosition(curve, samplePhase(i));
			steepestSlope = std::max(steepestSlope, rise * SampleCount);
		}
		REQUIRE(steepestSlope == Approx(oscillationCurvePeakSlope(curve)).epsilon(1e-6));
	}
}
