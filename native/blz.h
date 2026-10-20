#pragma once
#include <cstdint>

namespace blz {

struct Stream {
    void* context;
    uint32_t length;
    bool (*read)(void*, uint32_t, void*, uint32_t);
    bool (*write)(void*, uint32_t, const void*, uint32_t);
};

enum class Error { None, Io, Size, Footer, Reference };
struct Result { Error error; uint32_t length; };

Result compress(Stream input, Stream temporary, Stream output);
Result decompress(Stream input, Stream output);

}
