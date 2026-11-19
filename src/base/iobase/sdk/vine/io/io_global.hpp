#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_IOBASE_LIB
#    define VN_IOBASE_API VN_EXPORT
#else
#    define VN_IOBASE_API VN_IMPORT
#endif

/**
 * @brief vn::io is the storage layer: byte sources and sinks, a virtual file tree, and ZIP.
 *
 * Scope: the DataSource/DataSink byte vocabulary, the Vfs backends (a real directory, a ZIP archive, several trees
 * mounted under one root) with their IoError/Result contract, path normalization, stream adapters, and zlib/ZIP
 * (de)compression. Everything above it - file dialogs, documents, application policy - belongs to the modules that
 * own those concepts.
 */
#define VN_IO_NS_BEGIN                                                                                                                                         \
    namespace VN_ROOT_NS                                                                                                                                       \
    {                                                                                                                                                          \
    namespace io                                                                                                                                               \
    {

#define VN_IO_NS_END                                                                                                                                           \
    }                                                                                                                                                          \
    }
