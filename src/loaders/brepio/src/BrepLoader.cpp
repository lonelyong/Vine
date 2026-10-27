#include <vine/brepio/BrepLoader.hpp>

#include <array>
#include <span>
#include <string>

#include "BrepFormatTable.hpp"

#if defined(VN_BREPIO_HAS_OCCT)
#include <sstream>

#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <TopoDS_Shape.hxx>
#endif

VN_BREPIO_NS_BEGIN

namespace
{

/**
 * @brief Whether this build links the OpenCASCADE backend.
 *
 * One switch decides the whole module's capability: without a backend there is nothing to parse or write, so
 * supportedExtensions() stays empty and every load answers IoError::Unsupported. formatFromPath() keeps naming the
 * formats, because the format is a fact and the ability to read it is not.
 */
#if defined(VN_BREPIO_HAS_OCCT)
constexpr bool kBackendAvailable = true;
#else
constexpr bool kBackendAvailable = false;
#endif

} // namespace

#if defined(VN_BREPIO_HAS_OCCT)

namespace
{

/**
 * @brief Reads a whole stream into a string, so the bytes can be fingerprinted and parsed.
 *
 * @param in The stream to read.
 * @param out Receives the bytes, appended to what is already there.
 * @return true when the stream reached its end without breaking.
 */
bool drain(std::istream& in, std::string& out)
{
    std::array<char, 64U * 1024U> chunk{};
    for (;;) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            out.append(chunk.data(), static_cast<std::size_t>(got));
        }
        if (got < static_cast<std::streamsize>(chunk.size())) {
            break;
        }
    }
    return !in.bad();
}

/**
 * @brief Reads a solid from a file with the reader the format names.
 *
 * @param file_path The file to read.
 * @param format The format to parse as.
 * @return The shape; null when the reader could not produce one.
 */
TopoDS_Shape readFile(const std::string& file_path, BrepFormat format)
{
    TopoDS_Shape shape;
    switch (format) {
      case BrepFormat::Step: {
          STEPControl_Reader reader;
          if (reader.ReadFile(file_path.c_str()) != IFSelect_RetDone) {
              return {};
          }
          reader.TransferRoots();
          shape = reader.OneShape();
          break;
      }
      case BrepFormat::Iges: {
          IGESControl_Reader reader;
          if (reader.ReadFile(file_path.c_str()) != IFSelect_RetDone) {
              return {};
          }
          reader.TransferRoots();
          shape = reader.OneShape();
          break;
      }
      case BrepFormat::Brep: {
          BRep_Builder builder;
          BRepTools::Read(shape, file_path.c_str(), builder); // void since OCCT 8; a failure leaves the shape null
          if (shape.IsNull()) {
              return {};
          }
          break;
      }
      case BrepFormat::Unknown:
          break;
    }
    return shape;
}

/**
 * @brief Reads a solid from a stream with the reader the format names.
 *
 * @param in The stream holding the file.
 * @param format The format to parse as.
 * @return The shape; null when the reader could not produce one.
 */
TopoDS_Shape readStream(std::istream& in, BrepFormat format)
{
    TopoDS_Shape shape;
    switch (format) {
      case BrepFormat::Step: {
          STEPControl_Reader reader;
          if (reader.ReadStream("model.step", in) != IFSelect_RetDone) {
              return {};
          }
          reader.TransferRoots();
          shape = reader.OneShape();
          break;
      }
      case BrepFormat::Iges: {
          IGESControl_Reader reader;
          if (reader.ReadStream("model.igs", in) != IFSelect_RetDone) {
              return {};
          }
          reader.TransferRoots();
          shape = reader.OneShape();
          break;
      }
      case BrepFormat::Brep: {
          BRep_Builder builder;
          BRepTools::Read(shape, in, builder); // void since OCCT 8; a failed read leaves the shape null
          if (shape.IsNull()) {
              return {};
          }
          break;
      }
      case BrepFormat::Unknown:
          break;
    }
    return shape;
}

/**
 * @brief Wraps a shape the reader produced into the geometry handle the API hands out.
 *
 * @param shape The shape; ownership moves into the returned solid.
 * @return The solid, or InvalidData when the reader produced nothing.
 */
vn::io::Result<vn::intrusive_ptr<vn::geometry::BrepShape>> wrap(TopoDS_Shape shape)
{
    if (shape.IsNull()) {
        return vn::io::IoError::InvalidData;
    }
    auto brep = vn::make_intrusive<vn::geometry::BrepShape>();
    brep->setShape(std::make_shared<const TopoDS_Shape>(std::move(shape)));
    return brep;
}

} // namespace

#endif // VN_BREPIO_HAS_OCCT

BrepLoader::BrepLoader() = default;

BrepLoader::~BrepLoader() = default;

const std::set<String>& BrepLoader::supportedExtensions()
{
    static const std::set<String> extensions = [] {
        std::set<String> parsed;
        if (!kBackendAvailable) {
            return parsed; // no reader: offering a file this loader would refuse is worse than offering none
        }
        for (const BrepFormat format : detail::allFormats()) {
            for (const char* const extension : detail::extensionsOf(format)) {
                parsed.insert(String::fromUtf8(std::string(extension)));
            }
        }
        return parsed;
    }();
    return extensions;
}

void BrepLoader::clearCache()
{
    cache_.clear();
}

vn::io::Result<vn::intrusive_ptr<vn::geometry::BrepShape>> BrepLoader::load(
    const std::filesystem::path& file_path)
{
    const BrepFormat format = formatFromPath(file_path);
    if (format == BrepFormat::Unknown) {
        return vn::io::IoError::Unsupported; // names no format this module handles
    }

    std::error_code ec;
    if (file_path.empty() || !std::filesystem::is_regular_file(file_path, ec) || ec) {
        return vn::io::IoError::NotFound;
    }

    vn::crypto::ByteSequenceFingerprint fingerprint;
    try {
        fingerprint = vn::crypto::ByteSequenceFingerprint(file_path);
    } catch (const std::exception&) {
        return vn::io::IoError::IoFailure; // a file this process may not read is not a solid either
    }

    if (auto cached = cache_.get(fingerprint)) {
        return *cached;
    }

#if defined(VN_BREPIO_HAS_OCCT)
    // The readers take a path; the fingerprint pass above already paid for reading the bytes.
    const auto loaded = wrap(readFile(file_path.string(), format));
    if (!loaded) {
        return loaded;
    }
    cache_.set(fingerprint, *loaded);
    return loaded;
#else
    return vn::io::IoError::Unsupported; // no reader is linked into this build
#endif
}

vn::io::Result<vn::intrusive_ptr<vn::geometry::BrepShape>> BrepLoader::load(std::istream& in, BrepFormat format)
{
    if (format == BrepFormat::Unknown) {
        return vn::io::IoError::Unsupported; // a stream has no extension, so the caller must state the format
    }

#if defined(VN_BREPIO_HAS_OCCT)
    std::string bytes;
    if (!drain(in, bytes)) {
        return vn::io::IoError::IoFailure; // a stream that broke is not content to parse
    }
    if (bytes.empty()) {
        return vn::io::IoError::InvalidData;
    }

    // The same content loaded through either entry hashes to the same fingerprint, so it is parsed once.
    const vn::crypto::ByteSequenceFingerprint fingerprint(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (auto cached = cache_.get(fingerprint)) {
        return *cached;
    }

    std::istringstream image(bytes, std::ios::binary);
    auto               loaded = wrap(readStream(image, format));
    if (!loaded) {
        return loaded;
    }
    cache_.set(fingerprint, *loaded);
    return loaded;
#else
    static_cast<void>(in);
    return vn::io::IoError::Unsupported; // no reader is linked into this build
#endif
}

VN_BREPIO_NS_END
