#pragma once

enum class OscillationCurve
{
	Linear,
	Cosine,
	TrueCosine,
	Smoothstep,
	Smootherstep,
};

// Position along one leg of the oscillation, 0 to 1, at a phase of 0 to 1.
[[nodiscard]] double oscillationCurvePosition(OscillationCurve curve, double phase);
// Steepest d(position)/d(phase), where the oscillation moves at its maximum speed.
[[nodiscard]] double oscillationCurvePeakSlope(OscillationCurve curve);
