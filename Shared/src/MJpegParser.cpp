#include "MJpegParser.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QList>
RESTORE_COMPILER_WARNINGS

#include <limits>
#include <optional>

namespace {

// The payload size a part's header block declares; empty unless the part is a JPEG with exactly one valid length.
[[nodiscard]] std::optional<qsizetype> jpegPayloadSize(const QByteArray& headerBlock)
{
	bool jpegContentType = false;
	int contentLengthCount = 0;
	qint64 contentLength = -1;
	for (QByteArray line : headerBlock.split('\n'))
	{
		line = line.trimmed();
		const qsizetype colon = line.indexOf(':');
		if (colon <= 0)
			continue;
		const QByteArray name = line.left(colon).trimmed().toLower();
		const QByteArray value = line.mid(colon + 1).trimmed();
		if (name == "content-type")
			jpegContentType = value.compare("image/jpeg", Qt::CaseInsensitive) == 0;
		else if (name == "content-length")
		{
			bool ok = false;
			contentLength = value.toLongLong(&ok, 10);
			if (!ok)
				contentLength = -1;
			++contentLengthCount;
		}
	}

	// The payload's trailing CRLF must fit in a qsizetype too.
	const qint64 maximumPayload = std::numeric_limits<qsizetype>::max() - 2;
	if (!jpegContentType || contentLengthCount != 1 || contentLength <= 0 || contentLength > maximumPayload)
		return std::nullopt;
	return static_cast<qsizetype>(contentLength);
}

} // namespace

MJpegParser::MJpegParser(QByteArrayView boundaryTag)
	: _boundaryLine("--" + boundaryTag.toByteArray())
	, _closingBoundaryLine(_boundaryLine + "--")
{
}

MJpegParser::Error MJpegParser::append(QByteArrayView data, std::vector<QByteArray>& payloads)
{
	_buffer.append(data);
	for (;;)
	{
		switch (_state)
		{
		case State::Boundary:
		{
			const qsizetype lineEnd = _buffer.indexOf("\r\n");
			if (lineEnd < 0)
				return Error::None;
			const QByteArrayView line = QByteArrayView(_buffer).first(lineEnd);
			if (line == _boundaryLine)
				_state = State::Headers;
			else if (line == _closingBoundaryLine)
				_state = State::Complete;
			else
				return Error::InvalidBoundary;
			_buffer.remove(0, lineEnd + 2);
			break;
		}
		case State::Headers:
		{
			const qsizetype headerEnd = _buffer.indexOf("\r\n\r\n");
			if (headerEnd < 0)
				return Error::None;
			const std::optional<qsizetype> payloadSize = jpegPayloadSize(_buffer.first(headerEnd));
			if (!payloadSize)
				return Error::InvalidHeaders;
			_buffer.remove(0, headerEnd + 4);
			_payloadSize = *payloadSize;
			_state = State::Payload;
			break;
		}
		case State::Payload:
			if (_buffer.size() < _payloadSize + 2)
				return Error::None;
			if (QByteArrayView(_buffer).sliced(_payloadSize, 2) != "\r\n")
				return Error::MalformedPart;
			payloads.push_back(_buffer.first(_payloadSize));
			_buffer.remove(0, _payloadSize + 2);
			_state = State::Boundary;
			break;
		case State::Complete:
			return _buffer.trimmed().isEmpty() ? Error::None : Error::DataAfterEnd;
		}
	}
}

bool MJpegParser::endsCleanly() const
{
	// After a successful append, State::Complete holds only whitespace.
	return (_state == State::Headers && _buffer.isEmpty()) || _state == State::Complete;
}
