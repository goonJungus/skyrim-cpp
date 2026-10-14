// inflate.hpp - minimal DEFLATE/zlib decompressor.
//
// Self-contained on purpose: Skyrim's ESM/ESP records and BSA v105 archive
// blocks are zlib-deflated, and pulling in zlib would mean a third-party
// dependency to build on Windows, macOS and Linux. This is a straightforward
// RFC 1950/1951 implementation and is covered by tests/inflate_test.cpp.
//
// Not an optimizer: it trades a little speed for small size and auditability.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace skyrim::zip {

// Decompress a zlib stream (RFC 1950: 2-byte header + DEFLATE + Adler-32).
// On success writes to `out` (resized to the decompressed size) and returns
// true. Returns false on malformed input, bad checksum, or if `limit` is
// exceeded.
//
// `limit` caps the output size (0 = no cap beyond the stream's own end). This
// bounds memory use on hostile input.
bool zlibInflate(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out,
                 size_t limit = 0);

// Decompress a raw DEFLATE stream (RFC 1951, no zlib wrapper). Used by BSA
// blocks, which store bare deflate data.
bool rawInflate(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out,
                size_t limit = 0);

// Decompress a compressed ESM/ESP record payload. Unlike a bare zlib stream,
// a compressed record body is framed by a leading u32 that holds the
// decompressed size, followed by the zlib stream (RFC 1950). Returns false on
// malformed input or when the decoded size disagrees with the declared size.
bool zlibInflateRecord(const uint8_t* src, size_t srcSize,
                       std::vector<uint8_t>& out);

}  // namespace skyrim::zip
