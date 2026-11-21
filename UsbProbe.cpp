#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbioctl.h>
#include <usbiodef.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

constexpr USHORT vendorId = 0x05D8;
constexpr USHORT productId = 0x4002;

class HubHandle {
public:
    explicit HubHandle(const wchar_t* path)
        : handle(CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr)) {}
    ~HubHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
    HubHandle(const HubHandle&) = delete;
    HubHandle& operator=(const HubHandle&) = delete;

    const HANDLE handle;
};

bool hubQuery(HANDLE hub, DWORD request, void* buffer, DWORD capacity, DWORD& received) {
    return DeviceIoControl(hub, request, buffer, capacity, buffer, capacity, &received, nullptr) != FALSE;
}

unsigned readWord(const UCHAR* bytes) {
    return bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
}

bool printConfiguration(HANDLE hub, ULONG port) {
    // A single standard GET_DESCRIPTOR request for configuration index zero.
    // The hub driver handles the transfer. No scanner-specific commands are sent.
    constexpr USHORT maxConfigurationLength = 4096;
    constexpr DWORD headerLength = offsetof(USB_DESCRIPTOR_REQUEST, Data);
    std::array<UCHAR, headerLength + maxConfigurationLength> buffer{};
#pragma warning(suppress : 4815) // Only the fixed header is copied from this SDK structure.
    USB_DESCRIPTOR_REQUEST request;
    ZeroMemory(&request, sizeof(request));

    request.ConnectionIndex = port;
    request.SetupPacket.bmRequest = 0x80;
    request.SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    request.SetupPacket.wValue = USB_CONFIGURATION_DESCRIPTOR_TYPE << 8;
    request.SetupPacket.wLength = maxConfigurationLength;
    std::memcpy(buffer.data(), &request, headerLength);

    DWORD received = 0;
    if (!hubQuery(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buffer.data(),
                  static_cast<DWORD>(buffer.size()), received)) {
        std::fprintf(stderr, "GET_DESCRIPTOR failed (Win32 %lu).\n", GetLastError());
        return false;
    }

    const auto* data = buffer.data() + headerLength;
    const unsigned length = readWord(data + 2);
    if (received < headerLength + 9 || received > buffer.size() ||
        data[0] != 9 || data[1] != USB_CONFIGURATION_DESCRIPTOR_TYPE ||
        length < 9 || length > received - headerLength) {
        std::fputs("Invalid or truncated USB configuration descriptor.\n", stderr);
        return false;
    }

    std::printf("Configuration %u, interfaces %u\n", data[5], data[4]);
    bool bulkInFound = false;
    for (unsigned offset = data[0]; offset < length;) {
        const unsigned descriptorLength = data[offset];
        if (descriptorLength < 2 || descriptorLength > length - offset) {
            std::fputs("Invalid USB descriptor length.\n", stderr);
            return false;
        }

        const auto* descriptor = data + offset;
        if (descriptor[1] == USB_INTERFACE_DESCRIPTOR_TYPE && descriptorLength >= 9) {
            std::printf("  Interface %u, alternate setting %u, class 0x%02X\n",
                        descriptor[2], descriptor[3], descriptor[5]);
        }
        if (descriptor[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && descriptorLength >= 7 &&
            (descriptor[3] & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK) {
            const bool input = (descriptor[2] & USB_ENDPOINT_DIRECTION_MASK) != 0;
            std::printf("    Bulk %s 0x%02X, max packet %u bytes\n", input ? "IN" : "OUT",
                        descriptor[2], readWord(descriptor + 4));
            bulkInFound = bulkInFound || input;
        }
        offset += descriptorLength;
    }
    if (!bulkInFound) {
        std::fputs("Bulk IN endpoint missing.\n", stderr);
    }
    return bulkInFound;
}

} // namespace

int main() {
    auto hubInterface = GUID_DEVINTERFACE_USB_HUB;
    ULONG chars = 0;
    CONFIGRET status;
    std::vector<wchar_t> paths;
    do {
        status = CM_Get_Device_Interface_List_SizeW(&chars, &hubInterface, nullptr,
                                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (status != CR_SUCCESS) {
            break;
        }
        paths.resize(chars);
        status = CM_Get_Device_Interface_ListW(&hubInterface, nullptr, paths.data(), chars,
                                                 CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    } while (status == CR_BUFFER_SMALL);

    if (status != CR_SUCCESS) {
        std::fprintf(stderr, "USB hub enumeration failed (CONFIGRET 0x%lX).\n", status);
        return 1;
    }

    unsigned unreadableHubs = 0;
    unsigned unreadablePorts = 0;
    bool found = false;
    bool allConfigurationsRead = true;
    for (const wchar_t* path = paths.data(); *path; path += std::wcslen(path) + 1) {
        HubHandle hub(path);
        USB_NODE_INFORMATION node{};
        DWORD received = 0;
        if (hub.handle == INVALID_HANDLE_VALUE ||
            !hubQuery(hub.handle, IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), received) ||
            received < sizeof(node)) {
            ++unreadableHubs;
            continue;
        }

        for (ULONG port = 1; port <= node.u.HubInformation.HubDescriptor.bNumberOfPorts; ++port) {
            // Allow room for pipe information after the fixed connection header.
#pragma warning(suppress : 4815) // Only the fixed header is copied into this SDK structure.
            USB_NODE_CONNECTION_INFORMATION_EX connection;
            std::array<UCHAR, sizeof(connection) + 30 * sizeof(USB_PIPE_INFO)> buffer{};
            std::memcpy(buffer.data(), &port, sizeof(port));
            if (!hubQuery(hub.handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                          buffer.data(), static_cast<DWORD>(buffer.size()), received) || received < sizeof(connection)) {
                ++unreadablePorts;
                continue;
            }
            std::memcpy(&connection, buffer.data(), sizeof(connection));

            const auto& device = connection.DeviceDescriptor;
            if (connection.ConnectionStatus != DeviceConnected || device.idVendor != vendorId ||
                device.idProduct != productId) {
                continue;
            }

            found = true;
            std::printf("USB %04X:%04X, revision %04X, hub port %lu, address %u\n",
                        device.idVendor, device.idProduct, device.bcdDevice, port, connection.DeviceAddress);
            const bool configurationRead = printConfiguration(hub.handle, port);
            allConfigurationsRead = allConfigurationsRead && configurationRead;
        }
    }

    if (unreadableHubs || unreadablePorts) {
        std::fprintf(stderr, "USB queries failed for %u hub(s) and %u port(s).\n",
                     unreadableHubs, unreadablePorts);
    }
    if (!found) {
        std::fputs("USB device 05D8:4002 was not found on queried ports.\n", stderr);
        return unreadableHubs || unreadablePorts ? 1 : 2;
    }
    return allConfigurationsRead ? 0 : 1;
}
