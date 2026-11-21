#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <newdev.h>

#include <cstdio>
#include <cwchar>

namespace {
constexpr wchar_t instancePrefix[] = L"USB\\VID_05D8&PID_4002\\";
constexpr wchar_t interfaceId[] = L"{3BDA9676-AFB4-4502-A0FE-7D356F3C7609}";

int error(const char* operation, DWORD code = GetLastError()) {
    std::fprintf(stderr, "%s failed (0x%08lX).\n", operation, code);
    return 1;
}

bool selectInboxWinUsb(HDEVINFO set) {
    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof(params);
    if (!SetupDiGetDeviceInstallParamsW(set, nullptr, &params)) {
        return false;
    }
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    const UINT length = GetWindowsDirectoryW(params.DriverPath, MAX_PATH);
    if (!length || length >= MAX_PATH || wcscat_s(params.DriverPath, L"\\INF\\winusb.inf") != 0) {
        SetLastError(ERROR_BAD_PATHNAME);
        return false;
    }
    if (!SetupDiSetDeviceInstallParamsW(set, nullptr, &params) ||
        !SetupDiBuildDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER)) {
        return false;
    }
    for (DWORD index = 0;; ++index) {
        SP_DRVINFO_DATA_W driver{};
        driver.cbSize = sizeof(driver);
        if (!SetupDiEnumDriverInfoW(set, nullptr, SPDIT_CLASSDRIVER, index, &driver)) {
            return false;
        }
        SP_DRVINFO_DETAIL_DATA_W detail{};
        detail.cbSize = sizeof(detail);
        if (SetupDiGetDriverInfoDetailW(set, nullptr, &driver, &detail, sizeof(detail), nullptr) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            if (_wcsicmp(detail.SectionName, L"WINUSB") == 0) {
                return SetupDiSetSelectedDriverW(set, nullptr, &driver) != FALSE;
            }
        }
    }
}

int bind(HDEVINFO set, wchar_t* instance, bool replaceHamrick) {
    DEVINST devnode = 0;
    CONFIGRET result = CM_Locate_DevNodeW(&devnode, instance, CM_LOCATE_DEVNODE_NORMAL);
    if (result != CR_SUCCESS) {
        return error("Finding the connected scanner", result);
    }

    wchar_t service[256]{};
    ULONG length = sizeof(service);
    result = CM_Get_DevNode_Registry_PropertyW(devnode, CM_DRP_SERVICE, nullptr, service, &length, 0);
    const bool alreadyWinUsb = result == CR_SUCCESS && _wcsicmp(service, L"WinUSB") == 0;
    const bool unbound = result == CR_NO_SUCH_VALUE;
    wchar_t manufacturer[256]{};
    length = sizeof(manufacturer);
    const bool hamrick = replaceHamrick && result == CR_SUCCESS && _wcsicmp(service, L"usbscan") == 0 &&
        CM_Get_DevNode_Registry_PropertyW(devnode, CM_DRP_MFG, nullptr, manufacturer, &length, 0) == CR_SUCCESS &&
        _wcsicmp(manufacturer, L"Hamrick Software") == 0;
    if (!alreadyWinUsb && !unbound && !hamrick) {
        std::fputs("Cannot confirm this device is unbound. Existing driver will not be replaced.\n", stderr);
        return 1;
    }
    if (!alreadyWinUsb && !selectInboxWinUsb(set)) {
        return error("Selecting Microsoft's in-box WinUSB driver");
    }

    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance, nullptr, alreadyWinUsb ? 0 : DIOD_INHERIT_CLASSDRVS, &device)) {
        return error("Opening the scanner device instance");
    }
    BOOL needsReboot = FALSE;
    if (!alreadyWinUsb) {
        SP_DRVINFO_DATA_W driver{};
        driver.cbSize = sizeof(driver);
        if (!SetupDiGetSelectedDriverW(set, &device, &driver) ||
            !DiInstallDevice(nullptr, set, &device, &driver, 0, &needsReboot)) {
            return error("Installing Microsoft's in-box WinUSB driver");
        }
        std::puts("Microsoft WinUSB bound to this instance.");
    }

    HKEY parameters = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
                                          KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (parameters == INVALID_HANDLE_VALUE) {
        return error("Opening the scanner's Device Parameters key");
    }
    wchar_t currentId[128]{};
    DWORD currentBytes = sizeof(currentId);
    const LSTATUS existing = RegGetValueW(parameters, nullptr, L"DeviceInterfaceGUID",
                                          RRF_RT_REG_SZ, nullptr, currentId, &currentBytes);
    if (existing == ERROR_SUCCESS && _wcsicmp(currentId, interfaceId) == 0) {
        RegCloseKey(parameters);
        std::puts("The application interface is already configured.");
        return 0;
    }
    if (existing != ERROR_FILE_NOT_FOUND ||
        RegQueryValueExW(parameters, L"DeviceInterfaceGUIDs", nullptr, nullptr, nullptr, nullptr) != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(parameters);
        std::fputs("Cannot confirm the existing interface can be preserved. Settings were not replaced.\n", stderr);
        return 1;
    }
    const LSTATUS written = RegSetValueExW(parameters, L"DeviceInterfaceGUID", 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(interfaceId), sizeof(interfaceId));
    RegCloseKey(parameters);
    if (written != ERROR_SUCCESS) {
        return error("Registering the application interface", written);
    }
    std::puts(needsReboot ? "Windows requested a reboot." : "Reconnect to the same port or restart the device instance.");
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool replaceHamrick = argc == 3 && std::wcscmp(argv[2], L"--replace-hamrick") == 0;
    if ((argc != 2 && !replaceHamrick) || _wcsnicmp(argv[1], instancePrefix, ARRAYSIZE(instancePrefix) - 1) != 0 ||
        argv[1][ARRAYSIZE(instancePrefix) - 1] == L'\0') {
        std::fputs("Usage: BindWinUsb.exe <full USB\\VID_05D8&PID_4002\\... instance ID> [--replace-hamrick]\n", stderr);
        return 2;
    }
    HDEVINFO devices = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devices == INVALID_HANDLE_VALUE) {
        return error("Creating a device information set");
    }
    const int status = bind(devices, argv[1], replaceHamrick);
    SetupDiDestroyDeviceInfoList(devices);
    return status;
}
