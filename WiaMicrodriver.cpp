#include "BearPawScanner.h"

#include <wiamicro.h>
#include <wia.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace {

HINSTANCE moduleHandle;

struct Transfer {
    std::vector<BYTE> pixels;
    size_t offset = 0;
};

void finish(PSCANINFO scan) {
    delete static_cast<Transfer*>(scan->pMicroDriverContext);
    scan->pMicroDriverContext = nullptr;
}

HRESULT acquire(PSCANINFO scan, Transfer& transfer) {
    const SCANWINDOW& window = scan->Window;
    if (scan->Xresolution != BearPaw::dpi || scan->Yresolution != BearPaw::dpi ||
        scan->DataType != WIA_DATA_GRAYSCALE || window.xPos < 0 || window.yPos < 0 ||
        window.xExtent <= 0 || window.yExtent <= 0) {
        return E_INVALIDARG;
    }
    const BearPaw::Region region{static_cast<unsigned>(window.xPos), static_cast<unsigned>(window.yPos),
                                 static_cast<unsigned>(window.xExtent), static_cast<unsigned>(window.yExtent)};
    const HRSRC resource = FindResourceW(moduleHandle, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (!resource) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const HGLOBAL loaded = LoadResource(moduleHandle, resource);
    if (!loaded) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const auto* data = static_cast<const BYTE*>(LockResource(loaded));
    return BearPaw::scanGray(region, data, SizeofResource(moduleHandle, resource), transfer.pixels);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        moduleHandle = instance;
    }
    return TRUE;
}

WIAMICRO_API HRESULT SetPixelWindow(PSCANINFO scan, LONG x, LONG y, LONG width, LONG height) {
    if (!scan || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        !BearPaw::validRegion({static_cast<unsigned>(x), static_cast<unsigned>(y),
                              static_cast<unsigned>(width), static_cast<unsigned>(height)})) {
        return E_INVALIDARG;
    }
    scan->Window = {x, y, width, height};
    scan->WidthPixels = scan->WidthBytes = width;
    scan->Lines = height;
    return S_OK;
}

WIAMICRO_API HRESULT MicroEntry(LONG command, PVAL value) {
    if (!value || !value->pScanInfo) {
        return E_INVALIDARG;
    }
    auto* scan = value->pScanInfo;
    switch (command) {
    case CMD_INITIALIZE:
        scan->pMicroDriverContext = nullptr;
        scan->ADF = scan->TPA = scan->Endorser = 0;
        scan->OpticalXResolution = scan->OpticalYResolution = BearPaw::dpi;
        scan->Xresolution = scan->Yresolution = BearPaw::dpi;
        scan->BedWidth = BearPaw::bedWidth * 1000 / BearPaw::dpi;
        scan->BedHeight = BearPaw::bedHeight * 1000 / BearPaw::dpi;
        scan->SupportedDataTypes = SUPPORT_GRAYSCALE;
        scan->SupportedCompressionType = 0;
        scan->DataType = WIA_DATA_GRAYSCALE;
        scan->PixelBits = 8;
        scan->RawDataFormat = WIA_PACKED_PIXEL;
        scan->RawPixelOrder = WIA_ORDER_BGR;
        scan->bNeedDataAlignment = TRUE;
        scan->IntensityRange = scan->ContrastRange = {0, 0, 1};
        return SetPixelWindow(scan, 0, 0, BearPaw::bedWidth, BearPaw::bedHeight);
    case CMD_UNINITIALIZE:
    case CMD_RESETSCANNER:
        finish(scan);
        return S_OK;
    case CMD_SETXRESOLUTION:
    case CMD_SETYRESOLUTION:
        return value->lVal == BearPaw::dpi ? S_OK : E_INVALIDARG;
    case CMD_SETDATATYPE:
        return value->lVal == WIA_DATA_GRAYSCALE ? S_OK : E_INVALIDARG;
    case CMD_SETINTENSITY:
    case CMD_SETCONTRAST:
    case CMD_SETNEGATIVE:
    case CMD_SETMIRROR:
        return value->lVal == 0 ? S_OK : E_INVALIDARG;
    case CMD_SETSCANMODE:
        return value->lVal == SCANMODE_FINALSCAN || value->lVal == SCANMODE_PREVIEWSCAN ? S_OK : E_INVALIDARG;
    case CMD_STI_DEVICERESET:
    case CMD_SETSTIDEVICEHKEY:
        return S_OK;
    case CMD_GETCAPABILITIES:
        value->pGuid = nullptr;
        value->ppButtonNames = nullptr;
        value->lVal = 0;
        return S_OK;
    case CMD_STI_DIAGNOSTIC:
    case CMD_STI_GETSTATUS:
        try {
            const HRESULT status = BearPaw::scannerStatus();
            value->lVal = SUCCEEDED(status) || status == WIA_ERROR_BUSY ? MCRO_STATUS_OK : MCRO_ERROR_OFFLINE;
            value->pGuid = nullptr;
            return command == CMD_STI_DIAGNOSTIC ? status : S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    default:
        return E_NOTIMPL;
    }
}

WIAMICRO_API HRESULT Scan(PSCANINFO scan, LONG phase, PBYTE buffer, LONG capacity, LONG* received) {
    if (!scan || !received || capacity < 0 || (capacity > 0 && !buffer)) {
        return E_INVALIDARG;
    }
    *received = 0;
    if (phase == SCAN_FINISHED) {
        finish(scan);
        return S_OK;
    }
    if (phase == SCAN_FIRST) {
        finish(scan);
        auto* transfer = new (std::nothrow) Transfer;
        if (!transfer) {
            return E_OUTOFMEMORY;
        }
        scan->pMicroDriverContext = transfer;
        HRESULT result;
        try {
            result = acquire(scan, *transfer);
        } catch (const std::bad_alloc&) {
            result = E_OUTOFMEMORY;
        }
        if (FAILED(result)) {
            finish(scan);
            return result;
        }
    } else if (phase != SCAN_NEXT) {
        return E_INVALIDARG;
    }
    auto* transfer = static_cast<Transfer*>(scan->pMicroDriverContext);
    if (!transfer) {
        return E_UNEXPECTED;
    }
    const size_t available = transfer->pixels.size() - transfer->offset;
    const size_t count = (std::min)(available, static_cast<size_t>(capacity));
    if (count) {
        std::memcpy(buffer, transfer->pixels.data() + transfer->offset, count);
        transfer->offset += count;
        *received = static_cast<LONG>(count);
    }
    return S_OK;
}
