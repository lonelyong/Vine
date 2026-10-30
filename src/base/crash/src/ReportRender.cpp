#include <vine/crash/CrashHandler.hpp>

#include <charconv>
#include <cstring>

VN_CRASH_NS_BEGIN
namespace {

/// @brief One text line being assembled.
///
/// @details
/// - A fixed buffer rather than a string: this code runs inside a crashing process, where the heap and the CRT's locks are
///   exactly what may be broken.
class Line
{
  public:
    void add(std::string_view text) noexcept
    {
        const std::size_t room = sizeof data_ - size_;
        const std::size_t n    = text.size() < room ? text.size() : room;
        std::memcpy(data_ + size_, text.data(), n);
        size_ += n;
        data_[size_] = '\0';
    }

    void add(char value) noexcept
    {
        add(std::string_view{ &value, 1 });
    }

    void addHex(std::uint64_t value, std::size_t min_digits) noexcept
    {
        char buffer[24]{ '0', 'x' };
        char* const first = buffer + 2;
        char* const end   = std::to_chars(first, buffer + sizeof buffer, value, 16).ptr;
        auto        digits = static_cast<std::size_t>(end - first);
        if (digits < min_digits) {
            const std::size_t padding = min_digits - digits;
            std::memmove(first + padding, first, digits);
            std::memset(first, '0', padding);
            digits += padding;
        }
        add(std::string_view{ buffer, digits + 2 });
    }

    void addDecimal(std::uint64_t value) noexcept
    {
        char        buffer[24]{};
        char* const end = std::to_chars(buffer, buffer + sizeof buffer, value).ptr;
        add(std::string_view{ buffer, static_cast<std::size_t>(end - buffer) });
    }

    void addSigned(long long value) noexcept
    {
        char        buffer[24]{};
        char* const end = std::to_chars(buffer, buffer + sizeof buffer, value).ptr;
        add(std::string_view{ buffer, static_cast<std::size_t>(end - buffer) });
    }

    std::string_view view() const noexcept
    {
        return { data_, size_ };
    }

  private:
    char        data_[400]{};
    std::size_t size_{ 0 };
};

/// @brief Line-accumulating writer that never emits a partial line.
///
/// @details
/// - Writing stops at the first line that does not fit, so the result is a *prefix* of the full report. Dropping only the
///   line that overflows and continuing would let a short frame line in after the image table it refers to was cut - a
///   report that reads fine and lies about which module a frame belongs to.
class Writer
{
  public:
    explicit Writer(std::span<char> out) noexcept
        : out_(out)
    {
    }

    void line(std::string_view text) noexcept
    {
        if (stopped_ || used_ + text.size() + 2 > out_.size()) {
            stopped_ = true;
            return;
        }
        std::memcpy(out_.data() + used_, text.data(), text.size());
        used_ += text.size();
        out_[used_++] = '\r';
        out_[used_++] = '\n';
    }

    std::size_t used() const noexcept
    {
        return used_;
    }

  private:
    std::span<char> out_;
    std::size_t     used_{ 0 };
    bool            stopped_{ false };
};

/// @brief Writes `"<key> <value>"` as one line, skipping the line entirely when the value is empty.
void keyed(Writer& writer, std::string_view key, const char* value) noexcept
{
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    Line line;
    line.add(key);
    line.add(' ');
    line.add(value);
    writer.line(line.view());
}

} // namespace

std::size_t renderReport(const Report& report, std::span<char> out) noexcept
{
    Writer writer(out);

    {
        Line line;
        line.add(Report::kMagic);
        line.add(' ');
        line.addDecimal(static_cast<std::uint64_t>(Report::kVersion));
        writer.line(line.view());
    }

    keyed(writer, "app", report.app);
    keyed(writer, "time", report.time);
    keyed(writer, "dump", report.dump);

    for (std::size_t i = 0; i < report.context_count && i < Report::kMaxContext; ++i) {
        keyed(writer, "context", report.context[i]);
    }

    {
        Line line;
        line.add("exception ");
        line.addHex(report.code, 8);
        line.add(" at ");
        line.addHex(report.address, 16);
        line.add(" thread ");
        line.addDecimal(report.thread_id);
        writer.line(line.view());
    }

    // The image table comes before the frames so a reader (and the parser) knows every name a frame can refer to by index.
    for (std::size_t i = 0; i < report.image_count && i < Report::kMaxImages; ++i) {
        const ReportImage& image = report.images[i];
        Line               line;
        line.add("image ");
        line.addHex(image.base, 16);
        line.add(' ');
        line.add(image.name[0] != '\0' ? image.name : "?");
        line.add(' ');
        line.add(image.path);
        writer.line(line.view());
    }

    for (std::size_t i = 0; i < report.frame_count && i < Report::kMaxFrames; ++i) {
        const ReportFrame& frame = report.frames[i];
        Line               line;
        line.add("frame ");
        line.addHex(frame.address, 16);
        line.add(' ');
        line.addSigned(frame.image);
        line.add(' ');
        line.addHex(frame.offset, 0);
        writer.line(line.view());
    }

    return writer.used();
}

std::string renderReport(const Report& report)
{
    // Measure-then-render would need the same walk twice; growing a buffer is cheaper and still ends in one exact size.
    std::string text(2048, '\0');
    for (;;) {
        const std::size_t used = renderReport(report, std::span<char>(text.data(), text.size()));
        if (used < text.size() || text.size() >= std::size_t{ 1 } << 20) {
            text.resize(used);
            return text;
        }
        text.resize(text.size() * 2);
    }
}

VN_CRASH_NS_END
