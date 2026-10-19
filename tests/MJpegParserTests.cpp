#include "MJpegParser.h"
#include "TestHelpers.h"

#include <vector>

namespace {

// Binary payloads holding the stream's own delimiters: a part's end comes from its Content-length only.
const std::vector<QByteArray> SamplePayloads{
	QByteArray("\xFF\xD8 first \r\n--tag\r\n \xFF\xD9"),
	QByteArray("\xFF\xD8 second \r\n\r\nContent-length: 1\r\n \xFF\xD9"),
	QByteArray("\xFF\xD8 \0 third \xFF\xD9", 13),
};

[[nodiscard]] QByteArray part(const QByteArray& payload, const QByteArray& headers = {})
{
	const QByteArray headerBlock = headers.isEmpty()
		? "Content-type: image/jpeg\r\nContent-length: " + QByteArray::number(payload.size())
		: headers;
	return headerBlock + "\r\n\r\n" + payload + "\r\n";
}

// The layout libavformat's mpjpeg muxer writes: a boundary line before and after every part, no closing boundary.
[[nodiscard]] QByteArray ffmpegStream(const std::vector<QByteArray>& payloads)
{
	QByteArray stream = "--tag\r\n";
	for (const QByteArray& payload : payloads)
		stream += part(payload) + "--tag\r\n";
	return stream;
}

struct ParseResult
{
	MJpegParser::Error error = MJpegParser::Error::None;
	std::vector<QByteArray> payloads;
	bool endsCleanly = false;
};

// Feeds the chunks in order, stopping at the first error.
[[nodiscard]] ParseResult parse(const std::vector<QByteArrayView>& chunks)
{
	MJpegParser parser{ "tag" };
	ParseResult result;
	for (const QByteArrayView chunk : chunks)
	{
		result.error = parser.append(chunk, result.payloads);
		if (result.error != MJpegParser::Error::None)
			return result;
	}
	result.endsCleanly = parser.endsCleanly();
	return result;
}

[[nodiscard]] MJpegParser::Error parseError(const QByteArray& stream)
{
	return parse({ stream }).error;
}

} // namespace

TEST_CASE("MJpegParser: ffmpeg's stream yields every payload, however it is split into chunks", "[mjpeg]")
{
	const QByteArray stream = ffmpegStream(SamplePayloads);

	const ParseResult whole = parse({ stream });
	REQUIRE(whole.error == MJpegParser::Error::None);
	REQUIRE(whole.payloads == SamplePayloads);
	REQUIRE(whole.endsCleanly);

	const QByteArrayView view{ stream };
	for (qsizetype split = 0; split <= stream.size(); ++split)
	{
		CAPTURE(split);
		const ParseResult twoChunks = parse({ view.first(split), view.sliced(split) });
		REQUIRE(twoChunks.error == MJpegParser::Error::None);
		REQUIRE(twoChunks.payloads == SamplePayloads);
		REQUIRE(twoChunks.endsCleanly);
	}

	std::vector<QByteArrayView> bytes;
	for (qsizetype i = 0; i < stream.size(); ++i)
		bytes.push_back(view.sliced(i, 1));
	const ParseResult byteByByte = parse(bytes);
	REQUIRE(byteByByte.error == MJpegParser::Error::None);
	REQUIRE(byteByByte.payloads == SamplePayloads);
	REQUIRE(byteByByte.endsCleanly);
}

TEST_CASE("MJpegParser: where a stream ends cleanly", "[mjpeg]")
{
	REQUIRE_FALSE(parse({ "" }).endsCleanly);
	REQUIRE(parse({ "--tag\r\n" }).endsCleanly); // no parts at all

	const QByteArray stream = ffmpegStream(SamplePayloads);
	// The last part and the boundary line after it; cutting all of it leaves the stream ending after the previous part.
	const qsizetype lastPartSize = part(SamplePayloads.back()).size() + QByteArray("--tag\r\n").size();
	for (qsizetype cut = 1; cut < lastPartSize; ++cut)
	{
		CAPTURE(cut);
		const ParseResult truncated = parse({ stream.chopped(cut) });
		REQUIRE(truncated.error == MJpegParser::Error::None);
		REQUIRE_FALSE(truncated.endsCleanly);
	}
	REQUIRE(parse({ stream.chopped(lastPartSize) }).endsCleanly);

	const QByteArray closed = "--tag\r\n" + part(SamplePayloads.front()) + "--tag--\r\n";
	const ParseResult closedResult = parse({ closed, "\r\n" });
	REQUIRE(closedResult.error == MJpegParser::Error::None);
	REQUIRE(closedResult.payloads == std::vector<QByteArray>{ SamplePayloads.front() });
	REQUIRE(closedResult.endsCleanly);
	REQUIRE(parseError(closed + "--tag\r\n") == MJpegParser::Error::DataAfterEnd);
}

TEST_CASE("MJpegParser: header names and values are matched case-insensitively, other headers ignored", "[mjpeg]")
{
	const ParseResult result = parse({ "--tag\r\n" + part("abc", "content-TYPE: IMAGE/JPEG\r\nX-Extra: 1\r\nCONTENT-LENGTH:  3 ") + "--tag\r\n" });
	REQUIRE(result.error == MJpegParser::Error::None);
	REQUIRE(result.payloads == std::vector<QByteArray>{ "abc" });
}

TEST_CASE("MJpegParser: format errors", "[mjpeg]")
{
	REQUIRE(parseError("--other\r\n") == MJpegParser::Error::InvalidBoundary);
	REQUIRE(parseError("--tag\r\n" + part("abc") + "--other\r\n") == MJpegParser::Error::InvalidBoundary);

	const char* const invalidHeaderBlocks[]{
		"Content-length: 3",                                                  // no type
		"Content-type: image/png\r\nContent-length: 3",                       // not a JPEG
		"Content-type: image/jpeg",                                           // no length
		"Content-type: image/jpeg\r\nContent-length: 3\r\nContent-length: 3", // two lengths
		"Content-type: image/jpeg\r\nContent-length: 0",
		"Content-type: image/jpeg\r\nContent-length: -3",
		"Content-type: image/jpeg\r\nContent-length: 3x",
	};
	for (const char* headers : invalidHeaderBlocks)
	{
		CAPTURE(headers);
		REQUIRE(parseError("--tag\r\n" + part("abc", headers)) == MJpegParser::Error::InvalidHeaders);
	}

	REQUIRE(parseError("--tag\r\nContent-type: image/jpeg\r\nContent-length: 3\r\n\r\nabcd\r\n") == MJpegParser::Error::MalformedPart);
}
