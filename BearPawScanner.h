#pragma once

#include <windows.h>
#include <vector>

namespace BearPaw {

constexpr unsigned dpi = 100;
constexpr unsigned bedWidth = 827;
constexpr unsigned bedHeight = 1169;

struct Region {
    unsigned x, y, width, height;
};

bool validRegion(const Region& region);
HRESULT scannerStatus();
HRESULT scanGray(const Region& region, const BYTE* firmware, DWORD firmwareSize, std::vector<BYTE>& pixels);

} // namespace BearPaw
