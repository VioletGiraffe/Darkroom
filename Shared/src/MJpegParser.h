#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QByteArrayView>
RESTORE_COMPILER_WARNINGS

#include <vector>

// Incremental parser of ffmpeg's mpjpeg output: a "--<tag>" line, then per part a header block with
// Content-type: image/jpeg and Content-length, the JPEG payload and a CRLF, then the next "--<tag>" line.
// The stream may end after a "--<tag>" line (ffmpeg's always does) or with a closing "--<tag>--" line.
class MJpegParser
{
public:
	enum class Error
	{
		None,
		InvalidBoundary,
		InvalidHeaders,
		MalformedPart,
		DataAfterEnd,
	};

	explicit MJpegParser(QByteArrayView boundaryTag);

	// Consumes data and appends every payload it completes to payloads. After an error, the parser must not be fed again.
	[[nodiscard]] Error append(QByteArrayView data, std::vector<QByteArray>& payloads);
	// The data so far ends between parts: after a boundary line, or after the closing one.
	[[nodiscard]] bool endsCleanly() const;

private:
	enum class State
	{
		Boundary,
		Headers,
		Payload,
		Complete,
	};

	QByteArray _boundaryLine;
	QByteArray _closingBoundaryLine;
	QByteArray _buffer;
	State _state = State::Boundary;
	qsizetype _payloadSize = 0; // of the part being read in State::Payload
};
