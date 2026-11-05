// ============================================================================
//  CHZM SiliconFuse - HardwareSiliconMapper
//  File: tools/HardwareProbe/main.cpp
//
//  Read-only physical topology probe for the SiliconFuse unified memory
//  architecture. Enumerates the display adapter through SetupAPI, resolves the
//  Configuration Manager device node, reads the *allocated* logical
//  configuration (i.e. the physical address windows that the firmware and the
//  OS actually assigned to the device) and validates the size of the
//  prefetchable framebuffer aperture (PCIe Resizable BAR / BAR1).
//
//  The tool performs no writes to hardware, no MMIO access and requires no
//  kernel component. Some Configuration Manager queries need an elevated
//  token; the probe degrades gracefully and reports the exact CONFIGRET.
//
//  Build: see build_probe.bat (MinGW-w64 g++, C++20, static runtime).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Configuration Manager: allocated logical configuration.
//
// MinGW-w64 ships cfgmgr32.h without the CM_Get_Alloc_Log_Conf prototype and
// without the resource-list ABI (it lives in the WDK's wdm.h). The function is
// resolved dynamically from cfgmgr32.dll, which carries it on every supported
// Windows build; the descriptor layout below matches
// CM_PARTIAL_RESOURCE_DESCRIPTOR from wdm.h for x64 (24 bytes).
// ---------------------------------------------------------------------------
typedef CONFIGRET (WINAPI *PfnCmGetAllocLogConf)(PVOID, PULONG, DEVINST, ULONG);

static PfnCmGetAllocLogConf ResolveAllocLogConf()
{
    static PfnCmGetAllocLogConf fn = []() -> PfnCmGetAllocLogConf {
        HMODULE module = GetModuleHandleW(L"cfgmgr32.dll");
        if (!module) module = LoadLibraryW(L"cfgmgr32.dll");
        if (!module) return nullptr;
        FARPROC proc = GetProcAddress(module, "CM_Get_Alloc_Log_Conf");
        return reinterpret_cast<PfnCmGetAllocLogConf>(reinterpret_cast<void*>(proc));
    }();
    return fn;
}

#pragma pack(push, 4)
struct SF_PARTIAL_RESOURCE_DESCRIPTOR {
    UCHAR  Type;
    UCHAR  ShareDisposition;
    USHORT Flags;
    union {
        struct { LARGE_INTEGER Start; ULONG Length; } Memory;
        struct { LARGE_INTEGER Start; ULONG Length; } Port;
        struct { ULONG Level; ULONG Vector; ULONG_PTR Affinity; } Interrupt;
        struct { ULONG DataSize; ULONG Reserved1; ULONG Reserved2; } DevicePrivate;
        struct { LARGE_INTEGER Start; ULONG Length; ULONG Reserved; } BusNumber;
    } u;
};

struct SF_PARTIAL_RESOURCE_LIST {
    USHORT Version;
    USHORT Revision;
    ULONG  Count;
    SF_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptors[1];
};

struct SF_FULL_RESOURCE_DESCRIPTOR {
    ULONG InterfaceType;
    ULONG BusNumber;
    SF_PARTIAL_RESOURCE_LIST PartialResourceList;
};

struct SF_RESOURCE_LIST {
    ULONG Count;
    SF_FULL_RESOURCE_DESCRIPTOR List[1];
};
#pragma pack(pop)

static_assert(sizeof(SF_PARTIAL_RESOURCE_DESCRIPTOR) == 20,
              "CM_PARTIAL_RESOURCE_DESCRIPTOR ABI mismatch (pshpack4, 20 bytes)");

// CmResourceType* values (wdm.h).
enum {
    SF_CmResourceTypeNull = 0,
    SF_CmResourceTypePort = 1,
    SF_CmResourceTypeInterrupt = 2,
    SF_CmResourceTypeMemory = 3,
    SF_CmResourceTypeDma = 4,
    SF_CmResourceTypeDeviceSpecific = 5,
    SF_CmResourceTypeBusNumber = 6,
    SF_CmResourceTypeMemoryLarge = 7,
    SF_CmResourceTypeDevicePrivate = 129
};

// CM_RESOURCE_MEMORY_* flags (wdm.h, 10.0.16299+).
enum {
    SF_CM_RESOURCE_MEMORY_READ_ONLY     = 0x0001,
    SF_CM_RESOURCE_MEMORY_PREFETCHABLE  = 0x0004,
    SF_CM_RESOURCE_MEMORY_BAR           = 0x0080,
    SF_CM_RESOURCE_MEMORY_LARGE_40      = 0x0200,
    SF_CM_RESOURCE_MEMORY_LARGE_48      = 0x0400,
    SF_CM_RESOURCE_MEMORY_LARGE_64      = 0x0800
};

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

static std::string FormatMB(uint64_t bytes)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(bytes / (1024ull * 1024ull)));
    return buf;
}

static std::string FormatHex64(uint64_t value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08llX_%08llX",
                  (unsigned long long)(value >> 32),
                  (unsigned long long)(value & 0xFFFFFFFFull));
    return buf;
}

static std::string FormatGiB(uint64_t bytes)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// ---------------------------------------------------------------------------
// SetupAPI helpers
// ---------------------------------------------------------------------------
static bool GetDevicePropertyString(HDEVINFO set, PSP_DEVINFO_DATA dev,
                                    DWORD property, std::wstring& out)
{
    DWORD type = 0, size = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type,
                                           nullptr, 0, &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
            return false;
    }
    std::vector<BYTE> buffer(size);
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type,
                                           buffer.data(), size, nullptr))
        return false;
    if (type != REG_SZ && type != REG_MULTI_SZ) return false;
    out.assign(reinterpret_cast<const wchar_t*>(buffer.data()));
    return true;
}

static bool GetDevicePropertyMultiString(HDEVINFO set, PSP_DEVINFO_DATA dev,
                                         DWORD property, std::vector<std::wstring>& out)
{
    DWORD type = 0, size = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type,
                                           nullptr, 0, &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
            return false;
    }
    std::vector<BYTE> buffer(size + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type,
                                           buffer.data(), size, nullptr))
        return false;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(buffer.data());
    const wchar_t* end = p + (size / sizeof(wchar_t));
    while (p < end && *p) {
        out.emplace_back(p);
        p += wcslen(p) + 1;
    }
    return !out.empty();
}

static bool GetDevicePropertyDword(HDEVINFO set, PSP_DEVINFO_DATA dev,
                                   DWORD property, DWORD& out)
{
    DWORD type = 0, size = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type,
                                           nullptr, 0, &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(DWORD))
            return false;
    }
    DWORD value = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, &type,
                                           reinterpret_cast<PBYTE>(&value),
                                           sizeof(value), nullptr))
        return false;
    out = value;
    return true;
}

// ---------------------------------------------------------------------------
// Device model
// ---------------------------------------------------------------------------
struct MemoryWindow {
    uint64_t base = 0;
    uint64_t length = 0;
    bool     prefetchable = false;
    bool     isBar = false;
    bool     large = false;
};

struct GpuInfo {
    bool found = false;
    std::wstring friendlyName;
    std::wstring deviceDesc;
    std::wstring instanceId;
    std::wstring hardwareIdsJoined;
    std::wstring driverKey;
    std::wstring driverVersion;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t subsystemId = 0;
    uint32_t revision = 0;
    uint32_t bus = 0;
    uint32_t device = 0;
    uint32_t function = 0;
    uint64_t framebufferBytes = 0;      // from the display class registry key
    std::vector<MemoryWindow> windows;  // allocated memory resources
    std::wstring resourceSource;        // where the windows came from
    CONFIGRET allocLogConfResult = CR_FAILURE;
};

struct SmbiosMemoryInfo {
    std::string typeName = "Unknown";
    uint32_t speedMt = 0;
    uint32_t populatedModules = 0;
};

// ---------------------------------------------------------------------------
// PCI identifiers
// ---------------------------------------------------------------------------
static bool ParsePciId(const std::wstring& hwid, const wchar_t* tag, uint32_t& out)
{
    const std::wstring needle = std::wstring(tag) + L"_";
    size_t pos = hwid.find(needle);
    if (pos == std::wstring::npos) return false;
    pos += needle.size();
    uint32_t value = 0;
    for (int i = 0; i < 4 && pos + i < hwid.size(); ++i) {
        wchar_t c = hwid[pos + i];
        int digit;
        if (c >= L'0' && c <= L'9')      digit = c - L'0';
        else if (c >= L'A' && c <= L'F') digit = 10 + (c - L'A');
        else if (c >= L'a' && c <= L'f') digit = 10 + (c - L'a');
        else return false;
        value = (value << 4) | (uint32_t)digit;
    }
    out = value;
    return true;
}

// ---------------------------------------------------------------------------
// Registry: display class key (VRAM size, driver version)
// ---------------------------------------------------------------------------
static std::wstring ReadStringValue(HKEY root, const std::wstring& subkey,
                                    const wchar_t* name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    DWORD type = 0, size = 0;
    std::wstring result;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t)) {
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(buffer.data()), &size) == ERROR_SUCCESS)
            result.assign(buffer.data());
    }
    RegCloseKey(key);
    return result;
}

static uint64_t ReadQwordValue(HKEY root, const std::wstring& subkey, const wchar_t* name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, size = sizeof(uint64_t);
    uint64_t value = 0;
    RegQueryValueExW(key, name, nullptr, &type,
                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return (type == REG_QWORD || type == REG_BINARY) ? value : 0;
}

// The adapter's class key is stored as SPDRP_DRIVER, e.g.
// {4d36e968-e325-11ce-bfc1-08002be10318}\0000
static std::wstring ReadDisplayClassString(const std::wstring& driverKey, const wchar_t* name)
{
    if (driverKey.empty()) return {};
    return ReadStringValue(HKEY_LOCAL_MACHINE,
                           L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + driverKey, name);
}

static uint64_t ReadDisplayClassQword(const std::wstring& driverKey, const wchar_t* name)
{
    if (driverKey.empty()) return 0;
    return ReadQwordValue(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + driverKey, name);
}

// ---------------------------------------------------------------------------
// Resource list parser
//
// CM_RESOURCE_LIST layout (wdm.h, pshpack4):
//   ULONG Count; CM_FULL_RESOURCE_DESCRIPTOR List[];
//   + InterfaceType, BusNumber, CM_PARTIAL_RESOURCE_LIST
//   + Version, Revision, Count, CM_PARTIAL_RESOURCE_DESCRIPTOR[]
// CmResourceTypeMemoryLarge descriptors carry windows >= 4 GB. Their Length
// field stores the real length right-shifted by 8, 16 or 32 bits depending on
// the CM_RESOURCE_MEMORY_LARGE_40/48/64 flag.
// ---------------------------------------------------------------------------
static uint64_t DecodeWindowLength(const SF_PARTIAL_RESOURCE_DESCRIPTOR* desc,
                                   uint8_t type)
{
    uint64_t length = desc->u.Memory.Length;

    if (type == SF_CmResourceTypeMemoryLarge) {
        if (desc->Flags & SF_CM_RESOURCE_MEMORY_LARGE_40)      length <<= 8;
        else if (desc->Flags & SF_CM_RESOURCE_MEMORY_LARGE_48) length <<= 16;
        else if (desc->Flags & SF_CM_RESOURCE_MEMORY_LARGE_64) length <<= 32;
    }
    return length;
}

static bool IsRangeSane(const SF_RESOURCE_LIST* list, const BYTE* begin, const BYTE* end)
{
    if (!list) return false;
    if (list->Count == 0 || list->Count > 64) return false;
    if (reinterpret_cast<const BYTE*>(list) + sizeof(ULONG) > end) return false;
    return true;
}

static bool ParseResourceList(const BYTE* data, size_t size,
                              std::vector<MemoryWindow>& out)
{
    const BYTE* begin = data;
    const BYTE* end = data + size;
    const SF_RESOURCE_LIST* list = reinterpret_cast<const SF_RESOURCE_LIST*>(begin);
    if (!IsRangeSane(list, begin, end)) return false;

    for (ULONG i = 0; i < list->Count; ++i) {
        const BYTE* fullRaw = reinterpret_cast<const BYTE*>(&list->List[i]);
        if (fullRaw + sizeof(SF_FULL_RESOURCE_DESCRIPTOR) > end) break;

        const SF_FULL_RESOURCE_DESCRIPTOR* full =
            reinterpret_cast<const SF_FULL_RESOURCE_DESCRIPTOR*>(fullRaw);
        ULONG count = full->PartialResourceList.Count;

        const BYTE* descRaw = reinterpret_cast<const BYTE*>(
            &full->PartialResourceList.PartialDescriptors[0]);

        for (ULONG d = 0; d < count; ++d) {
            const BYTE* entry = descRaw + (size_t)d * sizeof(SF_PARTIAL_RESOURCE_DESCRIPTOR);
            if (entry + sizeof(SF_PARTIAL_RESOURCE_DESCRIPTOR) > end) break;

            const SF_PARTIAL_RESOURCE_DESCRIPTOR* desc =
                reinterpret_cast<const SF_PARTIAL_RESOURCE_DESCRIPTOR*>(entry);

            const bool isMemory = desc->Type == SF_CmResourceTypeMemory;
            const bool isMemoryLarge = desc->Type == SF_CmResourceTypeMemoryLarge;
            if (!isMemory && !isMemoryLarge) continue;

            MemoryWindow window;
            window.base = (uint64_t)desc->u.Memory.Start.QuadPart;
            window.length = DecodeWindowLength(desc, desc->Type);
            window.prefetchable = (desc->Flags & SF_CM_RESOURCE_MEMORY_PREFETCHABLE) != 0;
            window.isBar = (desc->Flags & SF_CM_RESOURCE_MEMORY_BAR) != 0;
            window.large = isMemoryLarge;

            if (window.base == 0 || window.length == 0) continue;
            out.push_back(window);
        }
    }
    return true;
}

static CONFIGRET ReadAllocatedMemoryWindows(DEVINST devInst, std::vector<MemoryWindow>& out)
{
    PfnCmGetAllocLogConf allocLogConf = ResolveAllocLogConf();
    if (!allocLogConf) return CR_FAILURE;

    ULONG length = 0;
    CONFIGRET cr = allocLogConf(nullptr, &length, devInst, 0);
    if (cr == CR_SUCCESS && length == 0) return CR_SUCCESS;
    if (cr != CR_BUFFER_SMALL && cr != CR_SUCCESS) return cr;
    if (length < sizeof(SF_RESOURCE_LIST)) length = 64 * 1024;

    std::vector<BYTE> buffer(length + 64, 0);
    ULONG actual = (ULONG)buffer.size();
    cr = allocLogConf(buffer.data(), &actual, devInst, 0);
    if (cr != CR_SUCCESS) return cr;

    if (!ParseResourceList(buffer.data(), std::min<size_t>(actual, buffer.size()), out))
        return CR_INVALID_DATA;
    return CR_SUCCESS;
}

// Non-elevated fallback: the firmware-assigned configuration is persisted per
// device under Enum\<instance>\LogConf\BootConfig (REG_RESOURCE_LIST). Reading
// it requires no privileges and matches the boot-time BAR programming.
static bool ReadBootConfigWindows(const std::wstring& instanceId,
                                  std::vector<MemoryWindow>& out)
{
    if (instanceId.empty()) return false;

    const std::wstring subkey =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId + L"\\LogConf";

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &key)
        != ERROR_SUCCESS)
        return false;

    DWORD type = 0, size = 0;
    bool ok = false;
    if (RegQueryValueExW(key, L"BootConfig", nullptr, &type, nullptr, &size)
            == ERROR_SUCCESS &&
        (type == REG_RESOURCE_LIST || type == REG_FULL_RESOURCE_DESCRIPTOR) &&
        size >= sizeof(SF_RESOURCE_LIST)) {
        std::vector<BYTE> buffer(size);
        if (RegQueryValueExW(key, L"BootConfig", nullptr, &type,
                             buffer.data(), &size) == ERROR_SUCCESS) {
            ok = ParseResourceList(buffer.data(), size, out);
        }
    }
    RegCloseKey(key);
    return ok;
}

// ---------------------------------------------------------------------------
// GPU discovery
// ---------------------------------------------------------------------------
static bool HardwareIdMatchesTarget(const std::wstring& hwid,
                                    uint32_t& vendor, uint32_t& device)
{
    if (hwid.find(L"PCI\\") != 0 && hwid.find(L"PCI&") == std::wstring::npos &&
        hwid.find(L"VEN_") == std::wstring::npos)
        return false;

    uint32_t vid = 0, did = 0;
    if (!ParsePciId(hwid, L"VEN", vid) || !ParsePciId(hwid, L"DEV", did))
        return false;
    if (vid != 0x10DE) return false;

    vendor = vid;
    device = did;
    return true;
}

static void FillPciLocation(const std::wstring& instanceId, GpuInfo& gpu,
                            HDEVINFO set, PSP_DEVINFO_DATA dev)
{
    DWORD busNumber = 0, address = 0;
    GetDevicePropertyDword(set, dev, SPDRP_BUSNUMBER, busNumber);
    GetDevicePropertyDword(set, dev, SPDRP_ADDRESS, address);

    if (address != 0) {
        gpu.bus = (address >> 16) & 0xFF;
        gpu.device = (address >> 8) & 0xFF;
        gpu.function = address & 0xFF;
    }
    if (busNumber != 0 || gpu.bus == 0) {
        if (busNumber != 0) gpu.bus = busNumber;
    }

    // Fallback: parse "PCI bus 1, device 0, function 0" from the instance path.
    if (gpu.bus == 0 && gpu.device == 0 && gpu.function == 0) {
        std::wstring location;
        if (GetDevicePropertyString(set, dev, SPDRP_LOCATION_INFORMATION, location)) {
            unsigned b = 0, d = 0, f = 0;
            if (swscanf_s(location.c_str(), L"PCI bus %u, device %u, function %u",
                          &b, &d, &f) == 3) {
                gpu.bus = b; gpu.device = d; gpu.function = f;
            }
        }
    }

    (void)instanceId;
}

static bool EnumerateGpu(GpuInfo& gpu)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                        DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        if (!SetupDiEnumDeviceInfo(set, index, &dev)) break;

        std::vector<std::wstring> hwids;
        if (!GetDevicePropertyMultiString(set, &dev, SPDRP_HARDWAREID, hwids))
            continue;

        uint32_t vendor = 0, device = 0;
        bool match = false;
        for (const auto& hwid : hwids) {
            uint32_t v = 0, d = 0;
            if (HardwareIdMatchesTarget(hwid, v, d)) {
                match = true;
                vendor = v;
                device = d;
                if (d == 0x2503 || d == 0x2504) break; // RTX 3060 LHR / GA106
            }
        }
        if (!match) continue;

        gpu.found = true;
        gpu.vendorId = vendor;
        gpu.deviceId = device;

        for (const auto& hwid : hwids) {
            uint32_t sub = 0;
            if (ParsePciId(hwid, L"SUBSYS", sub)) gpu.subsystemId = sub;
            uint32_t rev = 0;
            if (ParsePciId(hwid, L"REV", rev)) gpu.revision = rev;
        }
        for (size_t i = 0; i < hwids.size(); ++i) {
            if (i) gpu.hardwareIdsJoined += L"; ";
            gpu.hardwareIdsJoined += hwids[i];
        }

        std::wstring value;
        if (GetDevicePropertyString(set, &dev, SPDRP_FRIENDLYNAME, value))
            gpu.friendlyName = value;
        if (GetDevicePropertyString(set, &dev, SPDRP_DEVICEDESC, value))
            gpu.deviceDesc = value;
        if (GetDevicePropertyString(set, &dev, SPDRP_DRIVER, value))
            gpu.driverKey = value;

        DWORD len = 0;
        SetupDiGetDeviceInstanceIdW(set, &dev, nullptr, 0, &len);
        if (len > 1) {
            std::vector<wchar_t> id(len + 1, L'\0');
            if (SetupDiGetDeviceInstanceIdW(set, &dev, id.data(), len, nullptr))
                gpu.instanceId.assign(id.data());
        }

        FillPciLocation(gpu.instanceId, gpu, set, &dev);

        gpu.driverVersion = ReadDisplayClassString(gpu.driverKey, L"DriverVersion");
        gpu.framebufferBytes = ReadDisplayClassQword(gpu.driverKey,
                                                     L"HardwareInformation.qwMemorySize");

        DEVINST devInst = 0;
        if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(gpu.instanceId.c_str()),
                               CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            gpu.allocLogConfResult = ReadAllocatedMemoryWindows(devInst, gpu.windows);
        } else {
            gpu.allocLogConfResult = CR_NO_SUCH_DEVNODE;
        }

        if (!gpu.windows.empty()) {
            gpu.resourceSource = L"CM_Get_Alloc_Log_Conf (live allocation)";
        } else {
            std::vector<MemoryWindow> bootWindows;
            if (ReadBootConfigWindows(gpu.instanceId, bootWindows)) {
                gpu.windows.swap(bootWindows);
                gpu.resourceSource = L"Enum\\...\\LogConf\\BootConfig (firmware)";
            } else {
                gpu.resourceSource = L"unavailable";
            }
        }

        found = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// ---------------------------------------------------------------------------
// SMBIOS: populated DRAM type and rated speed
// ---------------------------------------------------------------------------
static const char* SmbiosMemoryTypeName(uint8_t type)
{
    switch (type) {
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default:   return "Unknown";
    }
}

static SmbiosMemoryInfo ReadSmbiosMemory()
{
    SmbiosMemoryInfo info;
    const DWORD providerRsmb = ('R' << 24) | ('S' << 16) | ('M' << 8) | 'B';
    DWORD size = GetSystemFirmwareTable(providerRsmb, 0, nullptr, 0);
    if (size == 0) return info;

    std::vector<BYTE> buffer(size);
    if (GetSystemFirmwareTable(providerRsmb, 0, buffer.data(), size) != size)
        return info;

    // RawSMBIOSData: 4 method bytes, 1 major, 1 minor, 1 revision, 4 length.
    if (buffer.size() < 8) return info;
    const BYTE* table = buffer.data() + 8;
    const BYTE* end = buffer.data() + buffer.size();

    while (table + 4 <= end) {
        uint8_t type = table[0];
        uint8_t length = table[1];
        if (length < 4 || table + length > end) break;

        if (type == 127) break; // end-of-table

        if (type == 17 && length >= 0x16) {
            uint16_t moduleSize = *reinterpret_cast<const uint16_t*>(table + 0x0C);
            uint8_t memType = table[0x12];
            uint16_t speed = *reinterpret_cast<const uint16_t*>(table + 0x15);

            bool populated = moduleSize != 0 && moduleSize != 0xFFFF;
            if (populated) {
                info.populatedModules++;
                if (info.typeName == "Unknown") info.typeName = SmbiosMemoryTypeName(memType);
                if (speed > info.speedMt && speed != 0xFFFF) info.speedMt = speed;
            }
        }

        // Skip the formatted area and the double-NUL terminated string set.
        const BYTE* next = table + length;
        while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) ++next;
        next += 2;
        if (next <= table) break;
        table = next;
    }

    return info;
}

// ---------------------------------------------------------------------------
// System RAM
// ---------------------------------------------------------------------------
struct SystemMemoryInfo {
    uint64_t installedBytes = 0;
    uint64_t usableBytes = 0;
    DWORD    loadPercent = 0;
};

static SystemMemoryInfo ReadSystemMemory()
{
    SystemMemoryInfo info;
    ULONGLONG installedKb = 0;
    if (GetPhysicallyInstalledSystemMemory(&installedKb))
        info.installedBytes = installedKb * 1024ull;

    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info.usableBytes = status.ullTotalPhys;
        info.loadPercent = status.dwMemoryLoad;
    }
    return info;
}

static std::string ReadProcessorName()
{
    std::wstring name = ReadStringValue(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    return ToUtf8(name);
}

static std::string ConfigRetText(CONFIGRET cr)
{
    char buf[96];
    const char* name = "unknown";
    switch (cr) {
    case CR_SUCCESS:         name = "CR_SUCCESS"; break;
    case CR_ACCESS_DENIED:   name = "CR_ACCESS_DENIED (elevation required)"; break;
    case CR_BUFFER_SMALL:    name = "CR_BUFFER_SMALL"; break;
    case CR_NO_SUCH_DEVNODE: name = "CR_NO_SUCH_DEVNODE"; break;
    case CR_INVALID_DATA:    name = "CR_INVALID_DATA"; break;
    case CR_FAILURE:         name = "CR_FAILURE - CM query unavailable, firmware config used"; break;
    default: break;
    }
    std::snprintf(buf, sizeof(buf), "%lu (%s)", (unsigned long)cr, name);
    return buf;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
static const char* BarStateText(uint64_t apertureBytes, uint64_t framebufferBytes)
{
    if (apertureBytes == 0) return "UNDETECTED";
    if (framebufferBytes > 0 && apertureBytes >= framebufferBytes)
        return "ACTIVE (Full Framebuffer Exposed)";
    if (apertureBytes >= (1ull << 30))
        return "PARTIAL (Aperture Smaller Than Framebuffer)";
    return "DISABLED (Legacy Aperture)";
}

static void PrintRule(char c = '=')
{
    for (int i = 0; i < 70; ++i) std::putchar(c);
    std::putchar('\n');
}

static void PrintDetail(const char* label, const std::string& value)
{
    std::printf("%-30s: %s\n", label, value.c_str());
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"CHZM-SiliconFuse Hardware Silicon Mapper");

    const SystemMemoryInfo mem = ReadSystemMemory();
    const SmbiosMemoryInfo smbios = ReadSmbiosMemory();
    const std::string cpuName = ReadProcessorName();

    GpuInfo gpu;
    const bool gpuFound = EnumerateGpu(gpu);

    // Primary BAR = largest prefetchable memory window owned by the adapter.
    const MemoryWindow* bar1 = nullptr;
    for (const auto& window : gpu.windows) {
        if (!window.prefetchable) continue;
        if (!bar1 || window.length > bar1->length) bar1 = &window;
    }
    // Fallback: some firmware marks the framebuffer aperture non-prefetchable.
    if (!bar1) {
        for (const auto& window : gpu.windows) {
            if (!bar1 || window.length > bar1->length) bar1 = &window;
        }
    }

    const uint64_t barBytes = bar1 ? bar1->length : 0;
    const uint64_t unifiedBytes = mem.installedBytes + gpu.framebufferBytes;

    PrintRule();
    std::printf("CHZM-SiliconFuse Hardware Silicon Mapper v0.1\n");
    PrintRule();

    std::string ramType = smbios.typeName;
    if (smbios.speedMt) ramType += "-" + std::to_string(smbios.speedMt);
    std::printf("[CPU] System RAM Installed   : %s MB (%s)\n",
                FormatMB(mem.installedBytes).c_str(), ramType.c_str());

    if (gpuFound) {
        std::string name = ToUtf8(gpu.friendlyName.empty() ? gpu.deviceDesc : gpu.friendlyName);
        char location[64];
        std::snprintf(location, sizeof(location), "PCIe %02X:%02X.%X",
                      gpu.bus, gpu.device, gpu.function);
        std::printf("[GPU] Device Identified       : %s (%s)\n", name.c_str(), location);
        std::printf("[BAR] Resizable BAR Status   : %s\n",
                    BarStateText(barBytes, gpu.framebufferBytes));
        std::printf("[BAR] Physical Base Address  : %s\n",
                    bar1 ? FormatHex64(bar1->base).c_str() : "N/A");
        std::printf("[BAR] Memory Window Size     : %s MB (%.2f GB)\n",
                    FormatMB(barBytes).c_str(),
                    (double)barBytes / (1024.0 * 1024.0 * 1024.0));
        std::printf("[BAR] Framebuffer Mapped     : %s MB (%.2f GB)\n",
                    FormatMB(gpu.framebufferBytes).c_str(),
                    (double)gpu.framebufferBytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        std::printf("[GPU] Device Identified       : NOT FOUND (VEN_10DE)\n");
        std::printf("[BAR] Resizable BAR Status   : UNDETECTED\n");
        std::printf("[BAR] Physical Base Address  : N/A\n");
        std::printf("[BAR] Memory Window Size     : 0 MB (0.00 GB)\n");
    }

    std::printf("[MAP] Unified Memory Target  : %s MB (%.2f GB Hardware Fabric)\n",
                FormatMB(unifiedBytes).c_str(),
                (double)unifiedBytes / (1024.0 * 1024.0 * 1024.0));
    PrintRule();
    std::printf("NOTE: 28 GB is a unified physical address window, not a single DRAM\n");
    std::printf("      device. Residency and placement are enforced by the SiliconFuse\n");
    std::printf("      policy engine; the probe performs read-only topology discovery.\n");
    PrintRule('-');

    std::printf("\n[SYSTEM]\n");
    PrintDetail("Processor", cpuName.empty() ? "Unknown" : cpuName);
    PrintDetail("Installed RAM (firmware)", FormatMB(mem.installedBytes) + " MB");
    PrintDetail("Usable RAM (OS)", FormatMB(mem.usableBytes) + " MB");
    PrintDetail("Memory Load", std::to_string(mem.loadPercent) + " %");
    PrintDetail("SMBIOS Memory Type", smbios.typeName);
    PrintDetail("SMBIOS Rated Speed", smbios.speedMt ? std::to_string(smbios.speedMt) + " MT/s" : "Unknown");
    PrintDetail("Populated Modules", std::to_string(smbios.populatedModules));

    if (gpuFound) {
        std::printf("\n[ADAPTER]\n");
        PrintDetail("Friendly Name", ToUtf8(gpu.friendlyName.empty()
                                            ? gpu.deviceDesc : gpu.friendlyName));
        PrintDetail("Device Description", ToUtf8(gpu.deviceDesc));
        PrintDetail("Instance Path", ToUtf8(gpu.instanceId));
        char ids[128];
        std::snprintf(ids, sizeof(ids), "VEN_%04X DEV_%04X SUBSYS_%08X REV_%02X",
                      gpu.vendorId, gpu.deviceId, gpu.subsystemId, gpu.revision);
        PrintDetail("PCI Identifiers", ids);
        PrintDetail("Driver Version", ToUtf8(gpu.driverVersion));
        PrintDetail("Framebuffer (registry)", FormatMB(gpu.framebufferBytes) + " MB");
        PrintDetail("Resource Source", ToUtf8(gpu.resourceSource));
        PrintDetail("CM_Get_Alloc_Log_Conf", ConfigRetText(gpu.allocLogConfResult));

        std::printf("\n[MEMORY WINDOWS] (allocated logical configuration)\n");
        if (gpu.windows.empty()) {
            std::printf("  none reported (CONFIGRET %ld)\n", (long)gpu.allocLogConfResult);
        } else {
            std::printf("  %-19s %-12s %-8s %s\n", "BASE", "SIZE (MB)", "FLAGS", "ROLE");
            std::vector<MemoryWindow> sorted = gpu.windows;
            std::sort(sorted.begin(), sorted.end(),
                      [](const MemoryWindow& a, const MemoryWindow& b) {
                          return a.base < b.base;
                      });
            for (const auto& window : sorted) {
                std::string flags;
                flags += window.prefetchable ? "P" : "-";
                flags += window.isBar ? "B" : "-";
                flags += window.large ? "L" : "-";
                const char* role = "auxiliary BAR / MMIO";
                if (bar1 && window.base == bar1->base)
                    role = "BAR1 framebuffer aperture";
                else if (window.length == 0x1000000ull)
                    role = "BAR0 register block";
                std::printf("  %-19s %-12s %-8s %s\n",
                            FormatHex64(window.base).c_str(),
                            FormatMB(window.length).c_str(),
                            flags.c_str(),
                            role);
            }
            std::printf("  flags: P=prefetchable B=PCI BAR L=large-memory descriptor\n");
        }

        std::printf("\n[RESIZABLE BAR VALIDATION]\n");
        if (gpu.framebufferBytes && barBytes) {
            const bool covers = barBytes >= gpu.framebufferBytes;
            PrintDetail("Framebuffer Size", FormatMB(gpu.framebufferBytes) + " MB");
            PrintDetail("Aperture Size", FormatMB(barBytes) + " MB");
            PrintDetail("Aperture / Framebuffer", FormatGiB(barBytes) + " / "
                        + FormatGiB(gpu.framebufferBytes) + " GB");
            PrintDetail("Power-of-two aperture", barBytes == 16384ull * 1024 * 1024
                        ? "yes (12 GB rounds up to 16 GB)" : "n/a");
            PrintDetail("Verdict", covers ? "PASS - full framebuffer is CPU-visible"
                                          : "FAIL - aperture does not cover framebuffer");
        } else {
            PrintDetail("Verdict", "INSUFFICIENT DATA");
        }

        std::printf("\n[TIER PROJECTION]\n");
        PrintDetail("Tier 0 (DDR4)", FormatMB(mem.installedBytes) + " MB");
        PrintDetail("Tier 1 (GDDR6 via ReBAR)", FormatMB(gpu.framebufferBytes) + " MB");
        PrintDetail("Tier 2 (NVMe spillover)", "policy-defined");
        PrintDetail("Unified Target", FormatMB(unifiedBytes) + " MB");
    }

    PrintRule('-');
    std::printf("Probe complete. No hardware state was modified.\n");

    return (gpuFound && barBytes != 0) ? 0 : 1;
}
