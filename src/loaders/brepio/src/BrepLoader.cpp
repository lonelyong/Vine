#include <vine/brepio/BrepLoader.hpp>

#include <algorithm>
#include <cctype>
#include <string>

VN_BREPIO_NS_BEGIN

namespace
{

/**
 * @brief Returns whether the extension denotes a STEP file.
 *
 * @param extension The lowercase file extension.
 * @return true for ".stp" or ".step".
 */
bool isStpExtension(const std::string& extension)
{
    return extension == ".stp" || extension == ".step";
}

/**
 * @brief Returns whether the extension denotes an IGES file.
 *
 * @param extension The lowercase file extension.
 * @return true for ".iges" or ".igs".
 */
bool isIgesExtension(const std::string& extension)
{
    return extension == ".iges" || extension == ".igs";
}

} // namespace

BrepLoader::BrepLoader() = default;

BrepLoader::~BrepLoader() = default;

BrepLoader& BrepLoader::defaultInstance()
{
    static BrepLoader instance;
    return instance;
}

bool BrepLoader::isSupportedFormat(const std::filesystem::path& file_path)
{
    if (!file_path.has_extension()) {
        return false;
    }

    std::string extension = file_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return isStpExtension(extension) || isIgesExtension(extension);
}

BrepLoader::Options& BrepLoader::options() noexcept
{
    return options_;
}

const BrepLoader::Options& BrepLoader::options() const noexcept
{
    return options_;
}

void BrepLoader::setOptions(const Options& options)
{
    options_ = options;
}

vn::intrusive_ptr<vn::geometry::BrepShape> BrepLoader::load(const std::filesystem::path& file_path)
{
    // TODO: STEP/IGES parsing requires OpenCASCADE (STEPControl_Reader /
    // IGESControl_Reader), which is not linked into the project yet. The
    // file-fingerprint cache (cache_) is defined and ready to be used once
    // the backend is wired in.
    return {};
}

VN_BREPIO_NS_END
