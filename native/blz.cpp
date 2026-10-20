// Native streaming port of BLZ.cs, Copyright (c) 2023 Xzonn, MIT license.
#include "blz.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace blz {
namespace {

struct Page {
    std::array<uint8_t, 4096> bytes{};
    uint32_t start = UINT32_MAX, age = 0;
    bool dirty = false;
};

struct Input {
    Stream stream;
    std::array<uint8_t, 16384> bytes;
    uint32_t start = UINT32_MAX, length = 0;
    bool valid = true;
    explicit Input(Stream value) : stream(value) {}
    uint8_t at(uint32_t offset) {
        if (!valid || offset >= stream.length) { valid = false; return 0; }
        if (offset < start || offset - start >= length) {
            uint32_t page = offset & ~4095u;
            start = page >= 4096 ? page - 4096 : 0;
            length = std::min<uint32_t>(bytes.size(), stream.length - start);
            if (!stream.read(stream.context, start, bytes.data(), length)) { valid = false; return 0; }
        }
        return bytes[offset - start];
    }
    uint32_t word(uint32_t offset) {
        uint32_t result = 0;
        for (unsigned int index = 0; index < 4; index++) result |= uint32_t(at(offset + index)) << (index * 8);
        return result;
    }
};

struct Buffer {
    Stream stream;
    std::array<Page, 3> pages;
    uint32_t age = 0;
    Page* recent = nullptr;
    bool valid = true;
    explicit Buffer(Stream value) : stream(value) {}
    bool flush(Page& page) {
        if (!page.dirty) return true;
        if (!stream.write || !stream.write(stream.context, page.start, page.bytes.data(), std::min<uint32_t>(4096, stream.length - page.start))) return valid = false;
        page.dirty = false;
        return true;
    }
    bool flush() {
        for (auto& page : pages) if (!flush(page)) return false;
        return valid;
    }
    uint8_t* get(uint32_t offset) {
        if (!valid || offset >= stream.length) { valid = false; return nullptr; }
        uint32_t start = offset & ~4095u;
        if (recent && recent->start == start) return &recent->bytes[offset - start];
        Page* found = nullptr;
        for (auto& page : pages) if (page.start == start) { found = &page; break; }
        if (!found) {
            found = &*std::min_element(pages.begin(), pages.end(), [](const Page& a, const Page& b) { return a.age < b.age; });
            if (!flush(*found)) return nullptr;
            uint32_t length = std::min<uint32_t>(4096, stream.length - start);
            if (!stream.read(stream.context, start, found->bytes.data(), length)) { valid = false; return nullptr; }
            found->start = start;
        }
        found->age = ++age;
        recent = found;
        return &found->bytes[offset - start];
    }
    uint8_t at(uint32_t offset) { auto value = get(offset); return value ? *value : 0; }
    void put(uint32_t offset, uint8_t value) {
        auto target = get(offset);
        if (!target) return;
        *target = value;
        recent->dirty = true;
    }
    uint32_t word(uint32_t offset) {
        uint32_t result = 0;
        for (unsigned int index = 0; index < 4; index++) result |= uint32_t(at(offset + index)) << (index * 8);
        return result;
    }
};

bool copy(Stream source, uint32_t from, Stream destination, uint32_t to, uint32_t length) {
    std::array<uint8_t, 4096> bytes;
    while (length) {
        uint32_t take = std::min<uint32_t>(bytes.size(), length);
        if (!source.read(source.context, from, bytes.data(), take) || !destination.write(destination.context, to, bytes.data(), take)) return false;
        from += take;
        to += take;
        length -= take;
    }
    return true;
}

struct Compressor {
    Input input;
    Buffer temporary;
    std::array<int32_t, 4096> heads;
    std::array<int32_t, 0x1003> next;
    Compressor(Stream source, Stream scratch) : input(source), temporary(scratch) { heads.fill(-1); }
    uint32_t key(uint32_t position) {
        return ((uint32_t(input.at(position)) * 251 + input.at(position - 1)) * 251 + input.at(position - 2)) & 4095;
    }
};

}

Result compress(Stream input, Stream temporary, Stream output) {
    if (input.length > 0xffffff || temporary.length < input.length || output.length < input.length) return {Error::Size, 0};
    temporary.length = input.length;
    auto state = std::make_unique<Compressor>(input, temporary);
    auto& source = state->input;
    auto& target = state->temporary;
    uint32_t src = input.length, dst = input.length, indexed = input.length;
    while (src) {
        if (!dst) return {Error::None, 0};
        uint32_t flagPosition = --dst;
        uint8_t flags = 0;
        for (unsigned int bit = 0; bit < 8; bit++) {
            flags <<= 1;
            if (!src) continue;
            while (indexed > src) {
                uint32_t position = --indexed;
                if (position < 2) continue;
                uint32_t value = state->key(position);
                state->next[position % state->next.size()] = state->heads[value];
                state->heads[value] = position;
            }
            uint32_t count = std::min<uint32_t>(src, 18), best = 0, distance = 0;
            for (int32_t position = src >= 3 ? state->heads[state->key(src - 1)] : -1; position >= 0; position = state->next[position % state->next.size()]) {
                uint32_t offset = uint32_t(position) - src + 1;
                if (offset > 0x1002) break;
                uint32_t matched = 0, limit = std::min(offset, count);
                while (matched < limit && source.at(src - 1 - matched) == source.at(src + offset - 1 - matched)) matched++;
                if (matched > best) {
                    best = matched;
                    distance = offset;
                    if (best == count) break;
                }
            }
            if (best < 3) {
                if (!dst) return {Error::None, 0};
                target.put(--dst, source.at(--src));
            } else {
                if (dst < 2) return {Error::None, 0};
                src -= best;
                dst -= 2;
                target.put(dst, uint8_t(distance - 3));
                target.put(dst + 1, uint8_t(((best - 3) << 4) | ((distance - 3) >> 8)));
                flags |= 1;
            }
            if (!source.valid || !target.valid) return {Error::Io, 0};
        }
        target.put(flagPosition, flags);
    }
    uint32_t packed = input.length - dst, sourcePosition = input.length, compressedPosition = packed;
    uint32_t prefix = 0, skip = 0;
    bool overwritten = false;
    while (sourcePosition && !overwritten) {
        uint8_t flags = target.at(dst + --compressedPosition);
        for (unsigned int bit = 128; bit && sourcePosition; bit >>= 1) {
            if (!(flags & bit)) {
                compressedPosition--;
                sourcePosition--;
            } else {
                sourcePosition -= (target.at(dst + compressedPosition - 1) >> 4) + 3;
                compressedPosition -= 2;
                if (sourcePosition < compressedPosition) {
                    prefix = sourcePosition;
                    skip = compressedPosition;
                    overwritten = true;
                    break;
                }
            }
        }
    }
    if (!target.flush()) return {Error::Io, 0};
    uint32_t length = prefix + packed - skip, aligned = (length + 3) & ~3u, size = aligned + 8;
    if (size >= input.length) return {Error::None, 0};
    if (!copy(input, 0, output, 0, prefix) || !copy(temporary, dst + skip, output, prefix, packed - skip)) return {Error::Io, 0};
    uint8_t footer[11];
    std::memset(footer, 255, sizeof(footer));
    uint32_t padding = aligned - length, header = size - length;
    uint32_t words[] = {(header << 24) | (length - prefix + header), input.length - size};
    for (unsigned int word = 0; word < 2; word++) for (unsigned int byte = 0; byte < 4; byte++)
        footer[padding + word * 4 + byte] = uint8_t(words[word] >> (byte * 8));
    if (!output.write(output.context, length, footer, padding + 8)) return {Error::Io, 0};
    return {Error::None, size};
}

Result decompress(Stream input, Stream output) {
    if (input.length < 8) return {Error::Footer, 0};
    auto source = std::make_unique<Input>(input);
    uint32_t difference = source->word(input.length - 4), packed = source->word(input.length - 8);
    uint32_t header = packed >> 24, length = packed & 0xffffff;
    uint64_t size = uint64_t(input.length) + difference;
    if (header < 8 || header > length || length > input.length || size > output.length) return {Error::Footer, 0};
    output.length = size;
    uint32_t prefix = input.length - length, src = input.length - header, dst = size;
    auto target = std::make_unique<Buffer>(output);
    while (src > prefix) {
        uint8_t flags = source->at(--src);
        for (unsigned int bit = 128; bit && src > prefix; bit >>= 1) {
            if (!(flags & bit)) {
                if (dst <= prefix) return {Error::Reference, 0};
                target->put(--dst, source->at(--src));
            } else {
                if (src - prefix < 2) return {Error::Reference, 0};
                uint8_t high = source->at(--src), low = source->at(--src);
                uint32_t count = (high >> 4) + 3, offset = (((high & 15) << 8) | low) + 3;
                if (dst < prefix + count || uint64_t(dst) + offset > size) return {Error::Reference, 0};
                for (uint32_t index = 0; index < count; index++) {
                    dst--;
                    uint8_t value = target->at(dst + offset);
                    target->put(dst, value);
                }
            }
            if (!source->valid || !target->valid) return {Error::Io, 0};
        }
    }
    if (dst != prefix) return {Error::Reference, 0};
    if (!target->flush() || !copy(input, 0, output, 0, prefix)) return {Error::Io, 0};
    return {Error::None, uint32_t(size)};
}

}
