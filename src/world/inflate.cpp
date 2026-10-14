// inflate.cpp - RFC 1950/1951 decompressor. See inflate.hpp.
//
// Structure follows the canonical-Huffman "counts + symbols" decoding used by
// zlib's reference `puff` decoder: build a table of code-length counts and an
// ordered symbol list, then walk bit by bit. This is slower than a table-driven
// decoder but is small enough to audit by eye, which matters more here than
// throughput -- the module inflates ESM records and BSA blocks at load time.

#include "world/inflate.hpp"

#include <array>

namespace skyrim::zip {
namespace {

// --- Bit reader (DEFLATE packs bits LSB-first) -----------------------------

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    // Read `n` bits (0..24). Sets m_failed and returns 0 on overrun.
    uint32_t bits(int n) {
        while (m_bitCount < n) {
            if (m_bytePos >= m_size) { m_failed = true; return 0; }
            m_bitBuf |= static_cast<uint32_t>(m_data[m_bytePos++]) << m_bitCount;
            m_bitCount += 8;
        }
        const uint32_t v = m_bitBuf & ((1u << n) - 1u);
        m_bitBuf >>= n;
        m_bitCount -= n;
        return v;
    }

    void alignToByte() {
        m_bitBuf = 0;
        m_bitCount = 0;
    }

    size_t bytePos() const { return m_bytePos; }
    bool failed() const { return m_failed; }

    // Copy `n` raw bytes straight from the stream (used by stored blocks).
    bool copyBytes(uint8_t* dst, size_t n) {
        if (m_bitCount > 0) {           // discard partial byte first
            m_bitBuf = 0;
            m_bitCount = 0;
        }
        if (m_bytePos + n > m_size) { m_failed = true; return false; }
        for (size_t i = 0; i < n; i++) dst[i] = m_data[m_bytePos++];
        return true;
    }

private:
    const uint8_t* m_data;
    size_t         m_size;
    size_t         m_bytePos = 0;
    uint32_t       m_bitBuf = 0;
    int            m_bitCount = 0;
    bool           m_failed = false;
};

// --- Canonical Huffman table ------------------------------------------------

struct Huffman {
    // counts[len] = number of codes of that bit length; symbols[] = symbols
    // ordered by (length, symbol).
    std::array<uint16_t, 16> counts{};
    std::vector<uint16_t>    symbols;

    bool build(const uint8_t* lengths, int n) {
        counts.fill(0);
        for (int i = 0; i < n; i++) counts[lengths[i]]++;
        if (counts[0] == n) return false;   // no codes at all

        // Reject over-subscribed sets; allow the single-code degenerate case.
        int left = 1;
        for (int len = 1; len <= 15; len++) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) return false;
        }

        std::array<uint16_t, 16> offs{};
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + counts[len];

        symbols.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; i++) {
            if (lengths[i] != 0) symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        }
        return true;
    }

    bool decode(BitReader& br, int& out) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= static_cast<int>(br.bits(1));
            if (br.failed()) return false;
            const int count = counts[len];
            if (code - first < count) {
                out = symbols[static_cast<size_t>(index + (code - first))];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code  <<= 1;
        }
        return false;
    }
};

// --- Length / distance tables (RFC 1951 3.2.5) -----------------------------

constexpr int kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr int kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Guard against decompression bombs / corrupt streams claiming huge distances.
constexpr size_t kMaxOutput = 1u << 30;

bool inflateBlockData(BitReader& br, const Huffman& lencode, const Huffman& distcode,
                      std::vector<uint8_t>& out, size_t limit) {
    for (;;) {
        int symbol = 0;
        if (!lencode.decode(br, symbol)) return false;

        if (symbol < 256) {
            if (out.size() >= kMaxOutput) return false;
            if (limit && out.size() + 1 > limit) return false;
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return true;   // end of block

        symbol -= 257;
        if (symbol >= 29) return false;    // codes 286/287 are invalid
        const int len = kLengthBase[symbol] +
                        static_cast<int>(br.bits(kLengthExtra[symbol]));

        int dsym = 0;
        if (!distcode.decode(br, dsym)) return false;
        if (dsym >= 30) return false;
        const size_t dist = static_cast<size_t>(kDistBase[dsym]) +
                            static_cast<size_t>(br.bits(kDistExtra[dsym]));
        if (br.failed()) return false;
        if (dist > out.size()) return false;   // reference before start of output
        if (limit && out.size() + static_cast<size_t>(len) > limit) return false;
        if (out.size() + static_cast<size_t>(len) > kMaxOutput) return false;

        // Byte-at-a-time: overlapping copies are legal and intended when
        // dist < len (run-length expansion).
        size_t src = out.size() - dist;
        for (int i = 0; i < len; i++) out.push_back(out[src + static_cast<size_t>(i)]);
    }
}

bool fixedTables(Huffman& lencode, Huffman& distcode) {
    uint8_t lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    if (!lencode.build(lengths, 288)) return false;
    uint8_t dlengths[30];
    for (int i = 0; i < 30; i++) dlengths[i] = 5;
    return distcode.build(dlengths, 30);
}

bool dynamicTables(BitReader& br, Huffman& lencode, Huffman& distcode) {
    static constexpr int kMaxBits = 15;
    static constexpr int kMaxLCodes = 286;
    static constexpr int kMaxDCodes = 30;
    static constexpr int kMaxCODES = kMaxLCodes + kMaxDCodes;

    const int nlen  = static_cast<int>(br.bits(5)) + 257;
    const int ndist = static_cast<int>(br.bits(5)) + 1;
    const int ncode = static_cast<int>(br.bits(4)) + 4;
    if (br.failed()) return false;
    if (nlen > kMaxLCodes || ndist > kMaxDCodes) return false;

    uint8_t lengths[kMaxCODES + 16] = {};
    for (int i = 0; i < ncode; i++) lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(br.bits(3));
    if (br.failed()) return false;

    for (int i = ncode; i < 19; i++) lengths[kCodeLengthOrder[i]] = 0;

    Huffman lencode_cl;
    if (!lencode_cl.build(lengths, 19)) return false;

    int index = 0;
    while (index < nlen + ndist) {
        int symbol = 0;
        if (!lencode_cl.decode(br, symbol)) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<uint8_t>(symbol);
        } else {
            int len = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + static_cast<int>(br.bits(2));
            } else if (symbol == 17) {
                repeat = 3 + static_cast<int>(br.bits(3));
            } else {
                repeat = 11 + static_cast<int>(br.bits(7));
            }
            if (index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = static_cast<uint8_t>(len);
        }
        if (br.failed()) return false;
    }
    if (lengths[256] == 0) return false;   // no end-of-block code

    return lencode.build(lengths, nlen) && distcode.build(lengths + nlen, ndist);
}

bool inflateCore(BitReader& br, std::vector<uint8_t>& out, size_t limit) {
    int last = 0;
    do {
        last = static_cast<int>(br.bits(1));
        const int type = static_cast<int>(br.bits(2));
        if (br.failed()) return false;

        if (type == 0) {                       // stored
            // Discard the remaining bits of the current byte. The byte was
            // already pulled whole into the bit buffer, so clearing the
            // buffer drops exactly its unused low bits.
            br.alignToByte();
            const uint32_t len  = br.bits(16);   // little-endian on the wire
            const uint32_t nlen = br.bits(16);
            if (br.failed()) return false;
            if ((len ^ 0xFFFFu) != nlen) return false;   // one's complement check
            if (limit && out.size() + len > limit) return false;
            if (out.size() + len > kMaxOutput) return false;
            const size_t before = out.size();
            out.resize(before + len);
            if (len && !br.copyBytes(out.data() + before, len)) return false;
        } else if (type == 1) {                // fixed Huffman
            Huffman lencode, distcode;
            if (!fixedTables(lencode, distcode)) return false;
            if (!inflateBlockData(br, lencode, distcode, out, limit)) return false;
        } else if (type == 2) {                // dynamic Huffman
            Huffman lencode, distcode;
            if (!dynamicTables(br, lencode, distcode)) return false;
            if (!inflateBlockData(br, lencode, distcode, out, limit)) return false;
        } else {
            return false;                       // type 3 is reserved
        }
    } while (!last);
    return true;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

}  // namespace

bool rawInflate(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out,
                size_t limit) {
    out.clear();
    if (!src || srcSize == 0) return false;
    BitReader br(src, srcSize);
    if (!inflateCore(br, out, limit)) { out.clear(); return false; }
    return true;
}

bool zlibInflate(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out,
                 size_t limit) {
    out.clear();
    // RFC 1950: 2-byte header (CMF/FLG), deflate body, 4-byte Adler-32.
    if (!src || srcSize < 6) return false;

    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0F) != 8) return false;                       // must be DEFLATE
    if (((static_cast<unsigned>(cmf) << 8) | flg) % 31u != 0) return false;  // header check
    if (flg & 0x20) return false;                              // preset dictionary unsupported

    BitReader br(src + 2, srcSize - 2);
    if (!inflateCore(br, out, limit)) { out.clear(); return false; }

    // Trailing Adler-32 is big-endian. Verify when the whole stream is present.
    const size_t tail = br.bytePos();
    if (tail + 4 <= srcSize - 2) {
        const uint8_t* t = src + 2 + tail;
        const uint32_t want = (static_cast<uint32_t>(t[0]) << 24) |
                              (static_cast<uint32_t>(t[1]) << 16) |
                              (static_cast<uint32_t>(t[2]) <<  8) |
                               static_cast<uint32_t>(t[3]);
        if (want != adler32(out.data(), out.size())) { out.clear(); return false; }
    }
    return true;
}

bool zlibInflateRecord(const uint8_t* src, size_t srcSize,
                       std::vector<uint8_t>& out) {
    out.clear();
    if (!src || srcSize < 6) return false;

    // Bethesda frames a compressed record payload as:
    //     u32 decompressedSize, then a zlib stream (RFC 1950).
    const uint32_t size = static_cast<uint32_t>(src[0]) |
                          (static_cast<uint32_t>(src[1]) << 8) |
                          (static_cast<uint32_t>(src[2]) << 16) |
                          (static_cast<uint32_t>(src[3]) << 24);
    if (size == 0 || size > kMaxOutput) return false;

    if (!zlibInflate(src + 4, srcSize - 4, out, size)) { out.clear(); return false; }
    if (out.size() != size) { out.clear(); return false; }
    return true;
}

}  // namespace skyrim::zip
