#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
// Inline H.264 helpers shared by the encoder and UDP paths plus the wire-protocol tests.
namespace h264 {
// Builds the phone-bound buffer: 4-byte big-endian length header plus payload copy; caller frees the result. Test helper BuildLengthPrefixedPacket mirrors OnEncodedPacket framing.
static inline uint8_t* BuildLengthPrefixedPacket(const uint8_t* data, int size, int* outFramedSize)
{
    int framedSize = size + 4;
    uint8_t* framed = (uint8_t*)malloc(framedSize);
    if (framed) {
        framed[0] = (uint8_t)((size >> 24) & 0xFF);
        framed[1] = (uint8_t)((size >> 16) & 0xFF);
        framed[2] = (uint8_t)((size >> 8) & 0xFF);
        framed[3] = (uint8_t)(size & 0xFF);
        memcpy(framed + 4, data, size);
    }
    *outFramedSize = framedSize;
    return framed;
}
// Scans a keyframe for the PPS end so OnEncodedPacket can insert a missing IDR marker; returns the offset or -1 when no insert applies.
static inline int FindIdrInsertionPoint(const uint8_t* data, int size, bool keyframe)
{
    if (!keyframe || size <= 10) return -1;
    int ppsDataEnd = -1;
    for (int i = 0; i < size - 5; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 &&
            data[i+3] == 0x01 && (data[i+4] & 0x1F) == 8) {
            for (int j = i + 5; j < size - 3; j++) {
                if (data[j] == 0x00 && data[j+1] == 0x00 &&
                    ((data[j+2] == 0x01) || (data[j+2] == 0x00 && data[j+3] == 0x01))) {
                    ppsDataEnd = j;
                    break;
                }
            }
            if (ppsDataEnd < 0) ppsDataEnd = i + 5 + 4;
            bool idrScPresent = false;
            if (ppsDataEnd + 3 < size &&
                data[ppsDataEnd] == 0x00 && data[ppsDataEnd+1] == 0x00 &&
                data[ppsDataEnd+2] == 0x01 &&
                (data[ppsDataEnd+3] & 0x1F) == 5) {
                idrScPresent = true;
            } else if (ppsDataEnd + 4 < size &&
                data[ppsDataEnd] == 0x00 && data[ppsDataEnd+1] == 0x00 &&
                data[ppsDataEnd+2] == 0x00 && data[ppsDataEnd+3] == 0x01 &&
                (data[ppsDataEnd+4] & 0x1F) == 5) {
                idrScPresent = true;
            }
            if (idrScPresent) return -1;
            return ppsDataEnd;
        }
    }
    return -1;
}
// Converts codec extradata (avcC or Annex-B) into Annex-B SPS/PPS bytes prepended to keyframes lacking them.
static inline std::vector<uint8_t> ParseExtradataSpsPps(const uint8_t* e, int esz)
{
    std::vector<uint8_t> result;
    if (!e || esz <= 0) return result;
    bool isAnnexB = (esz >= 4 && e[0] == 0 && e[1] == 0 &&
                     ((e[2] == 1) || (e[2] == 0 && e[3] == 1)));
    const uint8_t sc[] = {0, 0, 0, 1};
    if (isAnnexB) {
        for (int i = 0; i < esz - 4; i++) {
            bool sc4 = (e[i]==0 && e[i+1]==0 && e[i+2]==0 && e[i+3]==1);
            bool sc3 = (e[i]==0 && e[i+1]==0 && e[i+2]==1);
            if (!sc4 && !sc3) continue;
            int scLen = sc4 ? 4 : 3;
            int nalHdr = e[i + scLen];
            int nalType = nalHdr & 0x1F;
            if (nalType == 7 || nalType == 8) {
                int nalStart = i;
                int nalEnd = esz;
                for (int j = i + scLen + 1; j < esz - 3; j++) {
                    if (e[j]==0 && e[j+1]==0 &&
                        (e[j+2]==1 || (e[j+2]==0 && j+3<esz && e[j+3]==1))) {
                        nalEnd = j;
                        break;
                    }
                }
                result.insert(result.end(), sc, sc+4);
                result.insert(result.end(), e + nalStart + scLen, e + nalEnd);
                i = nalEnd - 1;
            }
        }
    } else if (esz > 5) {
        int offset = 5;
        int numSPS = e[offset] & 0x1F;
        offset++;
        for (int i = 0; i < numSPS && offset + 2 <= esz; i++) {
            int spsLen = (e[offset] << 8) | e[offset + 1];
            offset += 2;
            if (offset + spsLen > esz) break;
            result.insert(result.end(), sc, sc + 4);
            result.insert(result.end(), e + offset, e + offset + spsLen);
            offset += spsLen;
        }
        if (offset < esz) {
            int numPPS = e[offset];
            offset++;
            for (int i = 0; i < numPPS && offset + 2 <= esz; i++) {
                int ppsLen = (e[offset] << 8) | e[offset + 1];
                offset += 2;
                if (offset + ppsLen > esz) break;
                result.insert(result.end(), sc, sc + 4);
                result.insert(result.end(), e + offset, e + offset + ppsLen);
                offset += ppsLen;
            }
        }
    }
    return result;
}
// Inspects the first NAL of an access unit; reports whether SPS data is already present so callers skip prepending.
static inline bool AccessUnitHasSps(const uint8_t* data, int size)
{
    for (int i = 0; i < size - 4; i++) {
        int nalType = -1;
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            if (i + 4 < size) nalType = data[i+4] & 0x1F;
        } else if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            if (i + 3 < size) nalType = data[i+3] & 0x1F;
        }
        if (nalType == 7 || nalType == 14 || nalType == 15) return true;
        if (nalType >= 0) return false;
    }
    return false;
}
}
