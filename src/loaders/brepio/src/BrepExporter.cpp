#include <vine/brepio/BrepExporter.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include <string>

#if defined(VN_BREPIO_HAS_OCCT)
#    include <BRepTools.hxx>
#    include <IFSelect_ReturnStatus.hxx>
#    include <IGESControl_Writer.hxx>
#    include <STEPControl_Writer.hxx>
#    include <TopoDS_Shape.hxx>
#endif

VN_BREPIO_NS_BEGIN

namespace
{

/**
 * @brief Whether this build links the OpenCASCADE backend.
 *
 * Without it nothing can be written, so every export answers IoError::Unsupported instead of pretending the shape was
 * the problem.
 */
#if defined(VN_BREPIO_HAS_OCCT)
constexpr bool kBackendLinked = true;
#else
constexpr bool kBackendLinked = false;
#endif

/**
 * @brief A source that produces nothing and states why.
 *
 * createSource() answers a source for every format, so a build without a backend reports the refusal through error()
 * instead of returning null: a container asks that question before it takes the source.
 */
class RefusedSource final : public vn::io::DataSource {
  public:
    /**
     * @brief Creates a source that reports @p error and yields no bytes.
     *
     * @param error The failure to report.
     */
    explicit RefusedSource(vn::io::IoError error) noexcept
      : error_(error)
    {}

    [[nodiscard]] std::uint64_t size() const override
    {
        return 0;
    }

    void rewind() override
    {}

    [[nodiscard]] std::size_t read(std::span<std::byte>) override
    {
        return 0;
    }

    [[nodiscard]] vn::io::IoError error() const override
    {
        return error_;
    }

  private:
    vn::io::IoError error_;
};

/**
 * @brief A source whose bytes are produced by the exporter on the first pull.
 *
 * Nothing is written at construction. The first size() or read() runs the writer into a memory image, after which the
 * bytes are held here and rewind() replays them - a package that saves twice exports once.
 */
class ExportedBlobSource final : public vn::io::DataSource {
  public:
    /// Writes the whole content into the stream it is handed, and reports why it could not.
    using Producer = std::function<vn::io::IoError(std::ostream&)>;

    /**
     * @brief Creates a source that runs the producer on the first pull.
     *
     * @param producer The writer; it is called at most once.
     */
    explicit ExportedBlobSource(Producer producer)
      : producer_(std::move(producer))
    {}

    [[nodiscard]] std::uint64_t size() const override
    {
        produce();
        return static_cast<std::uint64_t>(bytes_.size());
    }

    void rewind() override
    {
        cursor_ = 0;
    }

    [[nodiscard]] std::size_t read(std::span<std::byte> out) override
    {
        produce();
        if (cursor_ >= bytes_.size()) {
            return 0;
        }
        const std::size_t count = std::min(out.size(), bytes_.size() - cursor_);
        std::memcpy(out.data(), bytes_.data() + cursor_, count);
        cursor_ += count;
        return count;
    }

    [[nodiscard]] vn::io::IoError error() const override
    {
        return error_;
    }

  private:
    /// Runs the producer once; its failure is remembered and reported through error().
    void produce() const
    {
        if (produced_) {
            return;
        }
        produced_ = true;

        std::ostringstream image(std::ios::binary);
        try {
            error_ = producer_(image);
        }
        catch (const std::exception&) {
            error_ = vn::io::IoError::IoFailure;
            return;
        }
        if (error_ == vn::io::IoError::Ok) {
            bytes_ = image.str();
        }
    }

    Producer                producer_;
    mutable std::string     bytes_;
    mutable bool            produced_{ false };
    mutable vn::io::IoError error_{ vn::io::IoError::Ok };
    std::size_t             cursor_{ 0 };
};

#if defined(VN_BREPIO_HAS_OCCT)

/**
 * @brief Writes one shape with the writer the format names.
 *
 * @param shape The shape to write.
 * @param format The target format.
 * @param out The stream to write.
 * @return IoError::Ok on success; IoFailure when the writer fails.
 */
vn::io::IoError writeStream(const TopoDS_Shape& shape, BrepFormat format, std::ostream& out)
{
    switch (format) {
    case BrepFormat::Step:
    {
        STEPControl_Writer writer;
        if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) {
            return vn::io::IoError::InvalidData; // the solid is not something STEP can state
        }
        return writer.WriteStream(out) == IFSelect_RetDone ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
    }
    case BrepFormat::Iges:
    {
        IGESControl_Writer writer;
        if (!writer.AddShape(shape)) {
            return vn::io::IoError::InvalidData;
        }
        writer.ComputeModel();
        return writer.Write(out) ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
    }
    case BrepFormat::Brep:
        BRepTools::Write(shape, out); // void since OCCT 8; a failure surfaces as an exception or a bad stream
        return out ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
    case BrepFormat::Unknown: break;
    }
    return vn::io::IoError::Unsupported;
}

#endif // VN_BREPIO_HAS_OCCT

} // namespace

BrepExporter::BrepExporter() = default;

vn::io::IoError BrepExporter::exportAs(const vn::geometry::BrepShape& shape, BrepFormat format, const std::filesystem::path& file_path) const
{
    if (format == BrepFormat::Unknown) {
        return vn::io::IoError::Unsupported;
    }
    if (!shape.isValid()) {
        return vn::io::IoError::InvalidData; // a solid without a shape has nothing to write
    }
    if (!kBackendLinked) {
        return vn::io::IoError::Unsupported;
    }

#if defined(VN_BREPIO_HAS_OCCT)
    try {
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path());
        }
    }
    catch (const std::exception&) {
        return vn::io::IoError::IoFailure;
    }

    try {
        std::ostringstream image(std::ios::binary);
        const auto         written = writeStream(*shape.shape(), format, image);
        if (written != vn::io::IoError::Ok) {
            return written;
        }
        std::ofstream file(file_path, std::ios::binary);
        if (!file) {
            return vn::io::IoError::IoFailure;
        }
        const std::string bytes = image.str();
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return file ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
    }
    catch (const std::exception&) {
        return vn::io::IoError::IoFailure;
    }
#else
    static_cast<void>(file_path);
    return vn::io::IoError::Unsupported; // no writer is linked into this build
#endif
}

vn::io::IoError BrepExporter::exportAs(const vn::geometry::BrepShape& shape, BrepFormat format, std::ostream& out) const
{
    if (format == BrepFormat::Unknown) {
        return vn::io::IoError::Unsupported;
    }
    if (!shape.isValid()) {
        return vn::io::IoError::InvalidData;
    }
    if (!kBackendLinked) {
        return vn::io::IoError::Unsupported;
    }

#if defined(VN_BREPIO_HAS_OCCT)
    try {
        return writeStream(*shape.shape(), format, out);
    }
    catch (const std::exception&) {
        return vn::io::IoError::IoFailure;
    }
#else
    static_cast<void>(out);
    return vn::io::IoError::Unsupported; // no writer is linked into this build
#endif
}

std::shared_ptr<vn::io::DataSource> BrepExporter::createSource(vn::intrusive_ptr<const vn::geometry::BrepShape> shape, BrepFormat format) const
{
    if (!shape) {
        return std::make_shared<RefusedSource>(vn::io::IoError::InvalidData); // nothing to write, in any build
    }
    if (format == BrepFormat::Unknown || !kBackendLinked) {
        return std::make_shared<RefusedSource>(vn::io::IoError::Unsupported);
    }

    return std::make_shared<ExportedBlobSource>([shape = std::move(shape), format](std::ostream& out) { return BrepExporter().exportAs(*shape, format, out); });
}

VN_BREPIO_NS_END
