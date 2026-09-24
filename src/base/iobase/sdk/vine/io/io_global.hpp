#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_IOBASE_LIB
#    define VN_IOBASE_API VN_EXPORT
#else
#    define VN_IOBASE_API VN_IMPORT
#endif

/**
 * @brief vn::io provides byte/stream/encoding primitives.
 *
 * Scope: byte buffers, input/output streams, gzip/zlib (de)compression and
 * encodings (base64/hex). File-system path operations are out of scope; they
 * belong to the system module.
 */
#define VN_IO_NS_BEGIN                                                                                                                                   \
    namespace VN_ROOT_NS                                                                                                                                 \
    {                                                                                                                                                   \
    namespace io                                                                                                                                        \
    {

#define VN_IO_NS_END                                                                                                                                     \
    }                                                                                                                                                   \
    }
