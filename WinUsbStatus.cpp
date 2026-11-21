#include <windows.h>
#include <bcrypt.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <usb.h>
#include <wia.h>

#include "BearPawScanner.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace {

GUID deviceInterface{0x3bda9676, 0xafb4, 0x4502, {0xa0, 0xfe, 0x7d, 0x35, 0x6f, 0x3c, 0x76, 0x09}};
using Packet = std::array<UCHAR, 64>;
constexpr ULONG firmwareLength = 8125;
using Firmware = std::array<UCHAR, firmwareLength + 1>;

bool firmwareHashMatches(Firmware& bytes) {
    constexpr std::array<UCHAR, 32> expectedHash{
        0xf3, 0x11, 0xca, 0xee, 0xa0, 0xd3, 0x11, 0x24, 0xee, 0xb3, 0xd1, 0xb9, 0x0d, 0x70, 0xf4, 0x1a,
        0x43, 0x0a, 0x98, 0x7b, 0x14, 0x08, 0x3f, 0x48, 0x60, 0x48, 0x3c, 0x7d, 0xf6, 0x3b, 0xdc, 0x72
    };
    std::array<UCHAR, expectedHash.size()> hash{};
    const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), firmwareLength,
                                       hash.data(), static_cast<ULONG>(hash.size()));
    if (status < 0 || hash != expectedHash) {
        std::fputs("PS2fw.usb SHA-256 verification failed.\n", stderr);
        return false;
    }
    return true;
}

bool readFirmwareFile(const wchar_t* path, Firmware& bytes) {
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0) {
        std::fputs("Could not open the firmware file.\n", stderr);
        return false;
    }
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool readFailed = std::ferror(file) != 0;
    std::fclose(file);
    if (readFailed || read != firmwareLength) {
        std::fputs("Expected the 8125-byte PS2fw.usb for the BearPaw 2400 CU.\n", stderr);
        return false;
    }
    return firmwareHashMatches(bytes);
}

bool exchange(WINUSB_INTERFACE_HANDLE usb, WINUSB_SETUP_PACKET setup, Packet& bytes) {
    ULONG count = 0;
    if (!WinUsb_ControlTransfer(usb, setup, bytes.data(), setup.Length, &count, nullptr)) {
        std::fprintf(stderr, "GT6801 transfer 0x%04X failed (Win32 %lu).\n", setup.Value, GetLastError());
        return false;
    }
    if (count != setup.Length) {
        std::fprintf(stderr, "GT6801 transfer 0x%04X returned %lu of %u bytes.\n", setup.Value, count, setup.Length);
        return false;
    }
    return true;
}

bool command(WINUSB_INTERFACE_HANDLE usb, Packet request, Packet& reply) {
    return exchange(usb, {0x40, 0x01, 0x2010, 0x3F40, 64}, request) &&
           exchange(usb, {0xC0, 0x01, 0x2011, 0x3F00, 64}, reply);
}

bool queryFirmware(WINUSB_INTERFACE_HANDLE usb, bool& running) {
    Packet reply{};
    if (!command(usb, {0x50, 0x01, 0x80}, reply)) {
        return false;
    }
    std::printf("GT6801 firmware query reply: %02X %02X %02X %02X\n", reply[0], reply[1], reply[2], reply[3]);
    running = reply[0] == 0 && reply[1] == 0x50;
    return true;
}

bool startFirmware(WINUSB_INTERFACE_HANDLE usb, const Firmware& firmware) {
    constexpr ULONG blockLength = 64;
    unsigned acceptedBlocks = 0;
    for (ULONG address = 0; address < firmwareLength; address += blockLength) {
        Packet block{};
        Packet ack{};
        std::copy_n(firmware.data() + address, (std::min)(blockLength, firmwareLength - address), block.data());
        if (!exchange(usb, {0x40, 0x01, 0x2009, static_cast<USHORT>(address), blockLength}, block) ||
            !exchange(usb, {0xC0, 0x01, 0x200A, 0x3F00, blockLength}, ack)) {
            std::fprintf(stderr, "Firmware transfer stopped at address 0x%04lX.\n", address);
            return false;
        }
        // GT6801 returns an acknowledgement here, not the uploaded bytes.
        if (ack[0] != 0 && ack[1] != 0x40) {
            std::fprintf(stderr, "Block 0x%04lX rejected: %02X %02X.\n", address, ack[0], ack[1]);
            return false;
        }
        ++acceptedBlocks;
    }
    std::printf("Controller acknowledged %u firmware blocks.\n", acceptedBlocks);

    Packet bootReply{};
    if (!command(usb, {0x69, 0x01, 0xC0, 0x1C}, bootReply)) {
        return false;
    }
    std::printf("GT6801 boot reply: %02X %02X %02X %02X\n",
                bootReply[0], bootReply[1], bootReply[2], bootReply[3]);
    Sleep(150);
    bool running = false;
    if (!queryFirmware(usb, running) || !running) {
        std::fputs("Controller did not confirm that the uploaded firmware is running.\n", stderr);
        return false;
    }
    std::puts("PS2fw.usb loaded; controller confirms firmware is running.");
    return true;
}

struct ScanArea {
    unsigned width;
    unsigned height;
    unsigned dpi;
    unsigned firstLine;
    UCHAR motorMode;
    bool lamp;

    constexpr unsigned stride() const { return (width + 63) / 64 * 64; }
};

constexpr ScanArea shortScan{64, 16, 50, 27, 0x01, true};
constexpr ScanArea a4Scan{BearPaw::bedWidth, BearPaw::bedHeight, BearPaw::dpi, 53, 0x01, true};
constexpr ScanArea whiteReference{a4Scan.width, 8, a4Scan.dpi, 22, 0x01, true};
constexpr ScanArea darkReference{a4Scan.width, 8, a4Scan.dpi, 0, 0x07, false};
using ScanPixels = std::vector<UCHAR>;

constexpr void writeWord(Packet& packet, unsigned offset, unsigned value) {
    packet[offset] = static_cast<UCHAR>(value);
    packet[offset + 1] = static_cast<UCHAR>(value >> 8);
}

constexpr Packet scanSetup(const ScanArea& area) {
    const unsigned baseSteps = 1200 / area.dpi;
    Packet packet{0x20, 0x01};
    writeWord(packet, 0x02, area.firstLine * baseSteps);
    writeWord(packet, 0x04, area.height * baseSteps);
    writeWord(packet, 0x08, (area.stride() - 1) * baseSteps);
    packet[0x0A] = 0x82; // 8-bit gray, green channel.
    packet[0x0B] = 0x60; // CIS.
    writeWord(packet, 0x0C, area.dpi);
    packet[0x0E] = 0x12;
    writeWord(packet, 0x10, area.stride());
    writeWord(packet, 0x12, area.height);
    packet[0x14] = area.motorMode;
    packet[0x15] = area.lamp ? 0 : 1;
    writeWord(packet, 0x16, area.dpi);
    return packet;
}

bool expectReply(WINUSB_INTERFACE_HANDLE usb, const Packet& request) {
    Packet reply{};
    if (!command(usb, request, reply)) {
        return false;
    }
    if (reply[0] != 0 || reply[1] != request[0]) {
        std::fprintf(stderr, "GT6801 command 0x%02X returned %02X %02X.\n", request[0], reply[0], reply[1]);
        return false;
    }
    return true;
}

bool waitForPixels(WINUSB_INTERFACE_HANDLE usb) {
    const ULONGLONG deadline = GetTickCount64() + 6000;
    do {
        Packet reply{};
        if (!command(usb, {0x35, 0x01}, reply)) {
            return false;
        }
        if (reply[0] == 0) {
            return true;
        }
        Sleep(50);
    } while (GetTickCount64() < deadline);
    std::fputs("Timed out waiting for scan data.\n", stderr);
    return false;
}

bool readScanPixels(WINUSB_INTERFACE_HANDLE usb, ScanPixels& pixels) {
    if (!waitForPixels(usb)) {
        return false;
    }
    constexpr ULONG chunkSize = 16 * 1024;
    const ULONGLONG deadline = GetTickCount64() + 180000;
    size_t offset = 0;
    while (offset < pixels.size() && GetTickCount64() < deadline) {
        const ULONG requested = static_cast<ULONG>((std::min)(pixels.size() - offset, size_t{chunkSize}));
        ULONG received = 0;
        if (!WinUsb_ReadPipe(usb, 0x81, pixels.data() + offset, requested, &received, nullptr)) {
            std::fprintf(stderr, "Bulk read failed after %zu of %zu bytes (Win32 %lu).\n",
                         offset, pixels.size(), GetLastError());
            return false;
        }
        if (!received || received > requested || received % 64 != 0) {
            std::fprintf(stderr, "Unexpected bulk transfer length: %lu at offset %zu.\n", received, offset);
            return false;
        }
        offset += received;
    }
    return offset == pixels.size();
}

bool saveScan(const wchar_t* path, const ScanArea& area, const ScanPixels& pixels) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    const unsigned rgbStride = (area.width * 3 + 3) & ~3u;
    std::vector<UCHAR> rgb(rgbStride * area.height);
    for (unsigned y = 0; y < area.height; ++y) {
        for (unsigned x = 0; x < area.width; ++x) {
            std::fill_n(rgb.data() + y * rgbStride + x * 3, 3, pixels[y * area.stride() + x]);
        }
    }
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(rgb.size());
    info.biSize = sizeof(info);
    info.biWidth = area.width;
    info.biHeight = -static_cast<LONG>(area.height);
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biXPelsPerMeter = info.biYPelsPerMeter = (area.dpi * 10000 + 127) / 254;
    const HANDLE output = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Creating scan BMP failed (Win32 %lu).\n", GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool complete =
        WriteFile(output, &file, sizeof(file), &written, nullptr) && written == sizeof(file) &&
        WriteFile(output, &info, sizeof(info), &written, nullptr) && written == sizeof(info) &&
        WriteFile(output, rgb.data(), static_cast<DWORD>(rgb.size()), &written, nullptr) && written == rgb.size();
    CloseHandle(output);
    if (!complete) {
        std::fputs("Writing the scan BMP failed.\n", stderr);
        return false;
    }
    std::printf("Saved %u x %u grayscale BMP.\n", area.width, area.height);
    return true;
}

bool capture(WINUSB_INTERFACE_HANDLE usb, const ScanArea& area, ScanPixels& pixels, const char* stage) {
    pixels.resize(area.stride() * area.height);
    if (!expectReply(usb, scanSetup(area))) {
        return false;
    }
    std::puts(stage);
    std::fflush(stdout);
    const bool captured = expectReply(usb, {0x43, 0x01}) && readScanPixels(usb, pixels);
    const bool stopped = expectReply(usb, {0x42, 0x01});
    return captured && stopped;
}

int columnMean(const ScanArea& area, const ScanPixels& pixels, unsigned column) {
    unsigned sum = 0;
    for (unsigned y = 0; y < area.height; ++y) {
        sum += pixels[y * area.stride() + column];
    }
    return static_cast<int>((sum + area.height / 2) / area.height);
}

bool captureCalibrated(WINUSB_INTERFACE_HANDLE usb, const ScanArea& area, ScanPixels& image) {
    ScanPixels white, dark;
    if (!capture(usb, whiteReference, white, "Capturing the white reference strip.") ||
        !capture(usb, darkReference, dark, "Capturing the dark reference with the lamp off.")) {
        return false;
    }

    std::array<int, a4Scan.width> blackLevel{};
    std::array<int, a4Scan.width> range{};
    unsigned usable = 0;
    for (unsigned x = 0; x < a4Scan.width; ++x) {
        blackLevel[x] = columnMean(darkReference, dark, x);
        range[x] = columnMean(whiteReference, white, x) - blackLevel[x];
        usable += range[x] >= 16;
    }
    std::printf("Calibration reference usable in %u of %u columns.\n", usable, a4Scan.width);
    if (usable * 100 < a4Scan.width * 95) {
        std::fputs("White and dark references are too close. A4 movement was not started.\n", stderr);
        return false;
    }
    if (!capture(usb, area, image, "Starting the document pass. Disconnect USB if the carriage hits the stop.")) {
        return false;
    }
    for (unsigned y = 0; y < area.height; ++y) {
        for (unsigned x = 0; x < area.width; ++x) {
            UCHAR& sample = image[y * area.stride() + x];
            if (range[x] >= 16) {
                sample = static_cast<UCHAR>(std::clamp((sample - blackLevel[x]) * 255 / range[x], 0, 255));
            }
        }
    }
    return true;
}

bool requestHome(WINUSB_INTERFACE_HANDLE usb) {
    std::puts("Requesting the GT6801 return to home.");
    std::fflush(stdout);
    const bool homeAccepted = expectReply(usb, {0x12, 0x01}) && expectReply(usb, {0x24, 0x01});
    if (homeAccepted) {
        std::puts("Controller accepted the return command. Verify the physical position at the scanner.");
    }
    return homeAccepted;
}

bool prepareScan(WINUSB_INTERFACE_HANDLE usb, bool fullPage) {
    ULONG timeoutMs = fullPage ? 4000 : 1500;
    constexpr UCHAR bulkIn = 0x81;
    if (!WinUsb_SetPipePolicy(usb, bulkIn, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs)) {
        std::fprintf(stderr, "Setting the bulk transfer timeout failed (Win32 %lu).\n", GetLastError());
        return false;
    }

    // BearPaw 2400 CU factory AFE and exposure defaults from the SANE model profile.
    Packet afe{0x22, 0x01, 0x0F, 0x01, 0x15, 0x06, 0x13, 0x04};
    Packet exposure{0x76, 0x01, 0x04, 0, 0x57, 0x01, 0x04, 0, 0x57, 0x01, 0x04, 0, 0x57, 0x01};
    return expectReply(usb, {0x42, 0x01}) && expectReply(usb, afe) && expectReply(usb, exposure);
}

bool scanDocument(WINUSB_INTERFACE_HANDLE usb, const wchar_t* output, bool fullPage) {
    if (!prepareScan(usb, fullPage)) {
        return false;
    }
    ScanPixels pixels;
    if (fullPage) {
        if (!captureCalibrated(usb, a4Scan, pixels)) {
            return false;
        }
        const bool saved = saveScan(output, a4Scan, pixels);
        const bool returned = requestHome(usb);
        return saved && returned;
    }
    return capture(usb, shortScan, pixels, "Starting one short scan. Disconnect USB if the carriage hits the stop.") &&
           saveScan(output, shortScan, pixels);
}

bool prepareControl(WINUSB_INTERFACE_HANDLE usb) {
    USB_DEVICE_DESCRIPTOR descriptor{};
    ULONG count = 0;
    if (!WinUsb_GetDescriptor(usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                              reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &count)) {
        std::fprintf(stderr, "Reading device descriptor failed (Win32 %lu).\n", GetLastError());
        return false;
    }
    if (count != sizeof(descriptor) || descriptor.idVendor != 0x05D8 || descriptor.idProduct != 0x4002) {
        std::fputs("Unexpected USB device descriptor. No GT6801 request sent.\n", stderr);
        return false;
    }
    std::printf("WinUSB connected to %04X:%04X, revision %04X.\n",
                descriptor.idVendor, descriptor.idProduct, descriptor.bcdDevice);

    ULONG timeoutMs = 1500;
    if (!WinUsb_SetPipePolicy(usb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs)) {
        std::fprintf(stderr, "Setting the control transfer timeout failed (Win32 %lu).\n", GetLastError());
        return false;
    }
    return true;
}

int readStatus(WINUSB_INTERFACE_HANDLE usb, const Firmware* firmware, const wchar_t* scanOutput, bool fullPage) {
    if (!prepareControl(usb)) {
        return 1;
    }
    bool running = false;
    if (!queryFirmware(usb, running)) {
        return 1;
    }
    std::puts(running
        ? "Controller acknowledges a running firmware; its identity has not been verified."
        : "Controller did not acknowledge running firmware.");
    if (scanOutput) {
        if (!running) {
            std::fputs("Load the BearPaw 2400 CU firmware before attempting a test scan.\n", stderr);
            return 1;
        }
        return scanDocument(usb, scanOutput, fullPage) ? 0 : 1;
    }
    return !firmware || running || startFirmware(usb, *firmware) ? 0 : 1;
}

int runCommand(int argc, wchar_t** argv);

struct ScannerConnection {
    HANDLE device = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    ~ScannerConnection() {
        if (usb) {
            WinUsb_Free(usb);
        }
        if (device != INVALID_HANDLE_VALUE) {
            CloseHandle(device);
        }
    }
};

HRESULT openScanner(ScannerConnection& connection) {
    constexpr ULONG present = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
    std::vector<wchar_t> paths;
    CONFIGRET result;
    do {
        ULONG length = 0;
        result = CM_Get_Device_Interface_List_SizeW(&length, &deviceInterface, nullptr, present);
        if (result != CR_SUCCESS) {
            break;
        }
        paths.resize(length);
        result = CM_Get_Device_Interface_ListW(&deviceInterface, nullptr, paths.data(), length, present);
    } while (result == CR_BUFFER_SMALL);
    if (result != CR_SUCCESS) {
        std::fprintf(stderr, "WinUSB interface enumeration failed (CONFIGRET 0x%lX).\n", result);
        return E_FAIL;
    }
    if (paths.empty() || paths.front() == L'\0') {
        std::fputs("BearPaw WinUSB interface is not registered or the scanner is disconnected.\n", stderr);
        return WIA_ERROR_OFFLINE;
    }
    if (paths[std::wcslen(paths.data()) + 1] != L'\0') {
        std::fputs("More than one BearPaw WinUSB interface is present.\n", stderr);
        return E_FAIL;
    }
    connection.device = CreateFileW(paths.data(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
    if (connection.device == INVALID_HANDLE_VALUE || !WinUsb_Initialize(connection.device, &connection.usb)) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "Opening the WinUSB device failed (Win32 %lu).\n", error);
        return error == ERROR_SHARING_VIOLATION ? WIA_ERROR_BUSY : HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}

bool parseCoordinate(const wchar_t* text, unsigned& value) {
    if (!*text) {
        return false;
    }
    value = 0;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9' || value > BearPaw::bedHeight) {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(*text - L'0');
    }
    return true;
}

int runCommand(int argc, wchar_t** argv) {
    const bool planA4 = argc == 2 && std::wcscmp(argv[1], L"--plan-a4") == 0;
    if (planA4 || (argc == 2 && std::wcscmp(argv[1], L"--plan-test-scan") == 0)) {
        const ScanArea& area = planA4 ? a4Scan : shortScan;
        std::printf("Motion test plan: %u x %u pixels at %u dpi, grayscale, no USB access.\n",
                    area.width, area.height, area.dpi);
        const Packet packet = scanSetup(area);
        for (unsigned i = 0; i < 25; ++i) {
            std::printf("%02X%c", packet[i], i == 24 ? '\n' : ' ');
        }
        return 0;
    }
    const bool load = argc == 3 && std::wcscmp(argv[1], L"--load-firmware") == 0;
    const bool fullPage = argc == 3 && std::wcscmp(argv[1], L"--scan-a4") == 0;
    const bool scan = fullPage || (argc == 3 && std::wcscmp(argv[1], L"--test-scan") == 0);
    const bool regionScan = argc == 8 && std::wcscmp(argv[1], L"--scan-region") == 0;
    if (argc != 1 && !load && !scan && !regionScan) {
        std::fputs("Usage: BearPaw2400CUStatus.exe [--load-firmware <PS2fw.usb> | --plan-test-scan | --plan-a4 | --test-scan <new BMP> | --scan-a4 <new BMP> | --scan-region <x> <y> <width> <height> <PS2fw.usb> <new BMP>]\n", stderr);
        return 1;
    }
    Firmware firmware{};
    if (load && !readFirmwareFile(argv[2], firmware)) {
        return 1;
    }
    if (scan && GetFileAttributesW(argv[2]) != INVALID_FILE_ATTRIBUTES) {
        std::fputs("The test scan needs a new output file path.\n", stderr);
        return 1;
    }
    if (regionScan) {
        BearPaw::Region region{};
        if (!parseCoordinate(argv[2], region.x) || !parseCoordinate(argv[3], region.y) ||
            !parseCoordinate(argv[4], region.width) || !parseCoordinate(argv[5], region.height) ||
            !BearPaw::validRegion(region)) {
            std::fputs("The scan rectangle must fit inside 827 x 1169 pixels at 100 dpi.\n", stderr);
            return 1;
        }
        if (GetFileAttributesW(argv[7]) != INVALID_FILE_ATTRIBUTES || !readFirmwareFile(argv[6], firmware)) {
            return 1;
        }
        ScanPixels pixels;
        const HRESULT result = BearPaw::scanGray(region, firmware.data(), firmwareLength, pixels);
        if (FAILED(result)) {
            std::fprintf(stderr, "Region scan failed (0x%08lX).\n", static_cast<unsigned long>(result));
            return result == WIA_ERROR_OFFLINE ? 2 : 1;
        }
        const ScanArea output{region.width, region.height, BearPaw::dpi, 0, 0, false};
        ScanPixels padded(output.stride() * output.height);
        for (unsigned y = 0; y < output.height; ++y) {
            std::copy_n(pixels.data() + y * output.width, output.width, padded.data() + y * output.stride());
        }
        return saveScan(argv[7], output, padded) ? 0 : 1;
    }
    ScannerConnection connection;
    const HRESULT opened = openScanner(connection);
    if (FAILED(opened)) {
        return opened == WIA_ERROR_OFFLINE ? 2 : 1;
    }
    return readStatus(connection.usb, load ? &firmware : nullptr, scan ? argv[2] : nullptr, fullPage);
}

} // namespace

namespace BearPaw {

bool validRegion(const Region& region) {
    return region.x < bedWidth && region.y < bedHeight &&
           region.width > 0 && region.width <= bedWidth - region.x &&
           region.height > 0 && region.height <= bedHeight - region.y;
}

HRESULT scannerStatus() {
    ScannerConnection connection;
    const HRESULT opened = openScanner(connection);
    if (FAILED(opened)) {
        return opened;
    }
    return prepareControl(connection.usb) ? S_OK : WIA_ERROR_DEVICE_COMMUNICATION;
}

HRESULT scanGray(const Region& region, const BYTE* data, DWORD dataSize, std::vector<BYTE>& pixels) {
    if (!validRegion(region) || !data || dataSize != firmwareLength) {
        return E_INVALIDARG;
    }
    Firmware firmware{};
    std::copy_n(data, dataSize, firmware.data());
    if (!firmwareHashMatches(firmware)) {
        return E_INVALIDARG;
    }
    ScannerConnection connection;
    const HRESULT opened = openScanner(connection);
    if (FAILED(opened)) {
        return opened;
    }
    bool running = false;
    if (!prepareControl(connection.usb) || !queryFirmware(connection.usb, running) ||
        (!running && !startFirmware(connection.usb, firmware)) || !prepareScan(connection.usb, true)) {
        return WIA_ERROR_DEVICE_COMMUNICATION;
    }
    ScanArea area = a4Scan;
    area.firstLine += region.y;
    area.height = region.height;
    ScanPixels raw;
    if (!captureCalibrated(connection.usb, area, raw) || !requestHome(connection.usb)) {
        return WIA_ERROR_DEVICE_COMMUNICATION;
    }
    pixels.resize(region.width * region.height);
    for (unsigned y = 0; y < region.height; ++y) {
        std::copy_n(raw.data() + y * area.stride() + region.x, region.width, pixels.data() + y * region.width);
    }
    return S_OK;
}

} // namespace BearPaw

#ifndef BEARPAW_WIA_BUILD
int wmain(int argc, wchar_t** argv) {
    return runCommand(argc, argv);
}
#endif
