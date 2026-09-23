#include <gtest/gtest.h>
#include <vine/MemoryStream.hpp>

#include <array>
#include <cstddef>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

using vine::ChunkedMemoryStream;
using vine::ChunkedMemoryStreamBuf;
using vine::InputChunkedMemoryStream;
using vine::InputMemoryStream;
using vine::InputSpanStream;
using vine::MemoryStream;
using vine::MemoryStreamBuf;
using vine::OutputChunkedMemoryStream;
using vine::OutputMemoryStream;
using vine::OutputSpanStream;
using vine::SpanStream;
using vine::SpanStreamBuf;

namespace
{

/**
 * @brief Wraps ASCII text as a byte span.
 *
 * @param text Text to wrap; the terminator is not included.
 * @return A view over the text bytes.
 */
std::span<const std::byte> asBytes(const std::string& text)
{
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

} // namespace

TEST(MemoryStream, WritesThenReadsBack)
{
    MemoryStream stream;
    stream << "hello" << ' ' << 42;
    ASSERT_EQ(stream.size(), 8u);
    // Every write path is visible through data(), not only through size().
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(stream.data()), stream.size()), "hello 42");

    stream.seekg(0, std::ios::beg);
    std::string out(stream.size(), '\0');
    stream.read(out.data(), static_cast<std::streamsize>(out.size()));
    EXPECT_EQ(out, "hello 42");
}

TEST(MemoryStream, ReadableThroughRdbuf)
{
    // The content is published in the get area, so rdbuf()->sgetn reads it.
    MemoryStream stream(asBytes("hello"));

    std::string out(stream.size(), '\0');
    const auto n = stream.rdbuf()->sgetn(out.data(), static_cast<std::streamsize>(out.size()));
    EXPECT_EQ(static_cast<std::size_t>(n), out.size());
    EXPECT_EQ(out, "hello");
}

TEST(MemoryStream, EmptyIsSafe)
{
    MemoryStream stream;
    EXPECT_EQ(stream.size(), 0u);

    // An empty stream still accepts a seek and yields nothing when read.
    stream.seekg(0, std::ios::beg);
    EXPECT_FALSE(stream.fail());

    char byte = 0;
    EXPECT_EQ(stream.rdbuf()->sgetn(&byte, 1), 0);
}

TEST(MemoryStream, SeekAndOverwrite)
{
    MemoryStream stream;
    stream << "abcdef";

    stream.seekp(2, std::ios::beg);
    stream << "XY";
    EXPECT_EQ(stream.size(), 6u);

    stream.seekg(0, std::ios::beg);
    std::string out(6, '\0');
    stream.read(out.data(), 6);
    EXPECT_EQ(out, "abXYef");
}

TEST(MemoryStream, SeekPastEndZeroFills)
{
    MemoryStream stream;
    stream << "ab";

    stream.seekp(5, std::ios::beg);
    ASSERT_EQ(stream.size(), 5u);

    const std::byte* bytes = stream.data();
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(std::to_integer<int>(bytes[2]), 0);
    EXPECT_EQ(std::to_integer<int>(bytes[4]), 0);

    // Writing at the end extends the stream instead of reopening the hole.
    stream << 'z';
    EXPECT_EQ(stream.size(), 6u);
    EXPECT_EQ(static_cast<char>(bytes[5]), 'z');
}

TEST(MemoryStream, GrowsBeyondInitialCapacity)
{
    // The initial capacity is 256 bytes, so a larger write has to reallocate.
    std::string payload(4096, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('0' + (i % 10));
    }

    MemoryStream stream;
    stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_EQ(stream.size(), payload.size());

    stream.seekg(0, std::ios::beg);
    std::string out(payload.size(), '\0');
    stream.read(out.data(), static_cast<std::streamsize>(out.size()));
    EXPECT_EQ(out, payload);
}

TEST(MemoryStream, GrowsThroughSingleCharacterWrites)
{
    // A single character lands in the put area and only reaches overflow()
    // once that area is full, so growth has to work through that path too.
    MemoryStream stream;
    std::string  expected;
    for (int i = 0; i < 1000; ++i) {
        const char ch = static_cast<char>('a' + (i % 26));
        stream << ch;
        expected.push_back(ch);
    }
    ASSERT_EQ(stream.size(), expected.size());

    stream.seekg(0, std::ios::beg);
    std::string out(expected.size(), '\0');
    stream.read(out.data(), static_cast<std::streamsize>(out.size()));
    EXPECT_EQ(out, expected);
}

TEST(MemoryStream, ReadPositionSurvivesGrowth)
{
    MemoryStream stream;
    stream << "start";
    stream.seekg(2, std::ios::beg);

    // Reallocation moves the storage; the reader must stay at its offset.
    const std::string tail(1000, 'x');
    stream.write(tail.data(), static_cast<std::streamsize>(tail.size()));

    char buf[5] = { 0, 0, 0, 0, 0 };
    stream.read(buf, 5);
    EXPECT_EQ(std::string(buf, 5), "artxx");
}

TEST(MemoryStream, TakesOverAVectorAndReleasesItWithoutCopying)
{
    std::vector<std::byte> bytes = { std::byte{ 'a' }, std::byte{ 'b' }, std::byte{ 'c' } };
    const std::byte*       original = bytes.data();

    MemoryStream stream(std::move(bytes));

    // The stream adopted the caller's storage instead of copying it.
    EXPECT_EQ(stream.data(), original);
    ASSERT_EQ(stream.size(), 3u);

    const std::vector<std::byte> released = stream.release();
    EXPECT_EQ(released.data(), original);
    EXPECT_EQ(released.size(), 3u);
    EXPECT_EQ(stream.size(), 0u);
}

TEST(MemoryStream, MoveLeavesSourceEmpty)
{
    MemoryStream source;
    source << "data";

    MemoryStream moved(std::move(source));
    EXPECT_EQ(moved.size(), 4u);
    EXPECT_EQ(source.size(), 0u);

    std::string out(4, '\0');
    moved.read(out.data(), 4);
    EXPECT_EQ(out, "data");
}

TEST(MemoryStream, ReleaseReturnsBytesAndEmpties)
{
    MemoryStream stream;
    stream << "abc";

    const auto bytes = stream.release();
    ASSERT_EQ(bytes.size(), 3u);
    EXPECT_EQ(static_cast<char>(bytes[0]), 'a');
    EXPECT_EQ(stream.size(), 0u);
}

TEST(MemoryStreamBuf, CopiesTheGivenBytes)
{
    const std::byte source[] = { std::byte{ 0x01 }, std::byte{ 0x02 }, std::byte{ 0x03 } };
    MemoryStreamBuf buf(std::span<const std::byte>(source, 3));

    ASSERT_EQ(buf.size(), 3u);
    ASSERT_NE(buf.data(), nullptr);
    EXPECT_EQ(std::to_integer<int>(buf.data()[2]), 0x03);

    buf.clearData();
    EXPECT_EQ(buf.size(), 0u);
}

TEST(ChunkedMemoryStream, ReadsAcrossChunkBoundaries)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdefghij";
    ASSERT_EQ(stream.size(), 10u);

    stream.seekg(0, std::ios::beg);
    std::string out(10, '\0');
    stream.read(out.data(), 10);
    EXPECT_EQ(out, "abcdefghij");
}

TEST(ChunkedMemoryStream, GrowsByAppendingChunks)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdefghij";
    ASSERT_EQ(stream.chunks().size(), 3u);

    const std::string tail(10, 'k');
    stream.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    ASSERT_EQ(stream.size(), 20u);

    // Growth added two chunks and left every earlier byte alone.
    const auto grown = stream.chunks();
    ASSERT_EQ(grown.size(), 5u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(grown[0].data), grown[0].size), "abcd");

    stream.seekg(0, std::ios::beg);
    std::string out(20, '\0');
    stream.read(out.data(), 20);
    EXPECT_EQ(out, "abcdefghij" + tail);
}

TEST(ChunkedMemoryStream, SeeksToArbitraryOffsets)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdefghij";

    stream.seekg(7, std::ios::beg);
    char buf[3] = { 0, 0, 0 };
    stream.read(buf, 3);
    EXPECT_EQ(std::string(buf, 3), "hij");
}

TEST(ChunkedMemoryStream, SeekToEndThenReadIsEndOfFile)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdefghij";

    stream.seekg(0, std::ios::end);
    char byte = 0;
    stream.read(&byte, 1);
    EXPECT_EQ(stream.gcount(), 0);
    EXPECT_TRUE(stream.eof());

    // The position stays at the end, so rewinding works as usual and
    // clear() remains the std::ios state reset it is everywhere else.
    stream.clear();
    stream.seekg(0, std::ios::beg);
    stream.read(&byte, 1);
    EXPECT_EQ(byte, 'a');
}

TEST(ChunkedMemoryStream, AppendAfterReadKeepsReadPosition)
{
    // The reader sits inside the last chunk, which the append extends.
    ChunkedMemoryStream stream(8);
    stream << "abcd";
    stream.seekg(2, std::ios::beg);
    stream << "efgh";
    ASSERT_EQ(stream.size(), 8u);

    char buf[2] = { 0, 0 };
    stream.read(buf, 2);
    EXPECT_EQ(std::string(buf, 2), "cd");
}

TEST(ChunkedMemoryStream, ChunksExposeTheContentWithoutCopying)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdefghij";

    const auto parts = stream.chunks();
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].size, 4u);
    EXPECT_EQ(parts[1].size, 4u);
    EXPECT_EQ(parts[2].size, 2u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(parts[2].data), parts[2].size), "ij");
}

TEST(ChunkedMemoryStream, CoalescesTheChunksIntoOneBlock)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdefghij";

    const std::byte* bytes = stream.data();
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes), stream.size()), "abcdefghij");

    // The coalesced block is cached, so it stays valid until the next write.
    EXPECT_EQ(stream.data(), bytes);
}

TEST(ChunkedMemoryStream, ReleaseReturnsBytesAndEmpties)
{
    ChunkedMemoryStream stream(4);
    stream << "abcdef";

    const auto bytes = stream.release();
    ASSERT_EQ(bytes.size(), 6u);
    EXPECT_EQ(static_cast<char>(bytes[5]), 'f');
    EXPECT_EQ(stream.size(), 0u);
}

TEST(ChunkedMemoryStreamBuf, MoveLeavesSourceEmpty)
{
    ChunkedMemoryStreamBuf source(4);
    source.sputn("abcd", 4);

    ChunkedMemoryStreamBuf moved(std::move(source));
    EXPECT_EQ(moved.size(), 4u);
    EXPECT_EQ(source.size(), 0u);
}

TEST(SpanStreamBuf, WritesIntoTheGivenWindow)
{
    std::array<std::byte, 8> window{};
    SpanStreamBuf            buf(window);

    EXPECT_EQ(buf.capacity(), window.size());
    EXPECT_EQ(buf.data(), window.data());
    EXPECT_EQ(buf.size(), 0u);

    EXPECT_EQ(buf.sputn("abcd", 4), 4);
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(window.data()), 4), "abcd");
}

TEST(SpanStreamBuf, RefusesToWritePastTheWindow)
{
    std::array<std::byte, 4> window{};
    SpanStreamBuf            buf(window);

    // A bulk write is short when the window runs out...
    EXPECT_EQ(buf.sputn("abcdef", 6), 4);
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(window.data()), 4), "abcd");

    // ...a single byte reports the overflow as eof...
    EXPECT_EQ(buf.sputc('z'), std::streambuf::traits_type::eof());

    // ...and the stream interface turns it into badbit without writing anything.
    std::ostream out(&buf);
    out.write("xy", 2);
    EXPECT_TRUE(out.bad());
    EXPECT_EQ(buf.size(), 4u);
}

TEST(SpanStreamBuf, ReadsBackTheWrittenPrefix)
{
    std::array<std::byte, 16> window{};
    SpanStreamBuf             buf(window);
    buf.sputn("hello", 5);

    // Only the written prefix is readable; the untouched bytes stay out of reach.
    buf.pubseekoff(0, std::ios::beg, std::ios::in);
    char out[8] = {};
    EXPECT_EQ(buf.sgetn(out, 8), 5);
    EXPECT_EQ(std::string(out, 5), "hello");
}

TEST(SpanStreamBuf, ClearDataAllowsFillingTheWindowAgain)
{
    std::array<std::byte, 8> window{};
    SpanStreamBuf            buf(window);

    buf.sputn("aaaaaaaa", 8);
    ASSERT_EQ(buf.size(), 8u);

    buf.clearData();
    EXPECT_EQ(buf.size(), 0u);

    // Back-patching: seekp() moves the write position without writing, so only
    // the byte at offset 0 is replaced and size() stays what was written.
    buf.sputn("hdr", 3);
    buf.pubseekoff(0, std::ios::beg, std::ios::out);
    buf.sputn("H", 1);
    ASSERT_EQ(buf.size(), 3u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(window.data()), 3), "Hdr");
}

TEST(SpanStreamBuf, EmptyWindowAndMoveAreSafe)
{
    SpanStreamBuf empty{ std::span<std::byte>{} };
    EXPECT_EQ(empty.capacity(), 0u);
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_EQ(empty.sputc('x'), std::streambuf::traits_type::eof());

    std::array<std::byte, 4> window{};
    SpanStreamBuf            buf(window);
    buf.sputn("ab", 2);

    SpanStreamBuf moved(std::move(buf));
    EXPECT_EQ(moved.size(), 2u);
    EXPECT_EQ(moved.capacity(), 4u);
    EXPECT_EQ(moved.data(), window.data());
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.capacity(), 0u);

    // The write position travelled with the window, so writing continues at offset 2.
    EXPECT_EQ(moved.sputn("cd", 2), 2);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(window.data()), 4), "abcd");
}

TEST(SpanStreamBuf, ReadOnlyViewIsZeroCopy)
{
    const std::string text = "hello";
    SpanStreamBuf     buf = SpanStreamBuf::readOnly(asBytes(text));

    // The view points at the caller's bytes: nothing was copied.
    EXPECT_EQ(buf.data(), asBytes(text).data());
    EXPECT_EQ(buf.capacity(), text.size());
    EXPECT_EQ(buf.size(), text.size());

    // With no writable room every write fails...
    EXPECT_EQ(buf.sputc('x'), std::streambuf::traits_type::eof());
    EXPECT_EQ(buf.sputn("xy", 2), 0);

    // ...while the whole window is readable, also after rewinding.
    char out[5] = {};
    EXPECT_EQ(buf.sgetn(out, 5), 5);
    EXPECT_EQ(std::string(out, 5), "hello");

    buf.clearData();
    EXPECT_EQ(buf.size(), text.size());
    char again[5] = {};
    EXPECT_EQ(buf.sgetn(again, 5), 5);
    EXPECT_EQ(std::string(again, 5), "hello");
}

TEST(InputSpanStream, ReadsAZeroCopyView)
{
    const std::string text = "12 34";
    InputSpanStream   in(asBytes(text));

    int first = 0;
    int second = 0;
    in >> first >> second;

    EXPECT_EQ(first, 12);
    EXPECT_EQ(second, 34);
    EXPECT_EQ(in.size(), text.size());
    EXPECT_EQ(in.data(), asBytes(text).data());
}

TEST(OutputSpanStream, WritesUntilTheWindowIsFull)
{
    std::array<std::byte, 4> window{};
    OutputSpanStream         out(window);

    out << "abc";
    EXPECT_EQ(out.size(), 3u);
    EXPECT_EQ(out.capacity(), window.size());

    // One byte fits and the second does not, so the stream reports it rather than growing.
    out << "de";
    EXPECT_TRUE(out.bad());
    EXPECT_EQ(out.size(), 4u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(window.data()), 4), "abcd");
}

TEST(SpanStream, WritesIntoTheWindowAndReadsItBack)
{
    std::array<std::byte, 8> window{};
    SpanStream               stream(window);

    stream << "hi";
    ASSERT_EQ(stream.size(), 2u);

    stream.seekg(0, std::ios::beg);
    std::string out(stream.size(), '\0');
    stream.read(out.data(), static_cast<std::streamsize>(out.size()));
    EXPECT_EQ(out, "hi");

    // The bytes live in the caller's window, not in a copy.
    EXPECT_EQ(stream.data(), window.data());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(window.data()), 2), "hi");
}

TEST(MemoryStreamWrappers, InputAndOutputStreamsReadAndWrite)
{
    OutputMemoryStream writer;
    writer << "hello";
    ASSERT_EQ(writer.size(), 5u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(writer.data()), writer.size()), "hello");

    InputMemoryStream reader(asBytes("hello"));
    std::string       text(5, '\0');
    reader.read(text.data(), 5);
    EXPECT_EQ(text, "hello");
}

TEST(ChunkedStreamWrappers, InputAndOutputStreamsReadAndWrite)
{
    OutputChunkedMemoryStream writer(4);
    writer << "abcdefghij";
    ASSERT_EQ(writer.size(), 10u);
    ASSERT_EQ(writer.chunks().size(), 3u);

    // The chunked input stream copies what it is handed, so releasing the bytes
    // first and wrapping them afterwards is safe.
    const std::vector<std::byte> bytes = writer.release();
    InputChunkedMemoryStream     reader(bytes, 4);

    std::string text(10, '\0');
    reader.read(text.data(), 10);
    EXPECT_EQ(text, "abcdefghij");
}
