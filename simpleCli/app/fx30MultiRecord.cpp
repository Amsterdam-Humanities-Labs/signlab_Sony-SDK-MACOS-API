// FX30 Multi-Camera Web Controller
// Auto-discovers Sony FX30 cameras over USB and provides
// a web-based REST API + embedded HTML dashboard for
// simultaneous start/stop recording, status monitoring, and file download.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
  #include <unistd.h>
#endif

#if defined(__APPLE__)
  #include <IOKit/IOKitLib.h>
  #include <IOKit/usb/IOUSBLib.h>
  #include <IOKit/IOCFPlugIn.h>
  #include <CoreFoundation/CoreFoundation.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
  using CrString = std::wstring;
  #define CRSTR(s) L ## s
  #define CrCout std::wcout
#else
  using CrString = std::string;
  #define CRSTR(s) s
  #define CrCout std::cout
#endif

#include "CrDeviceProperty.h"
#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "CrDebugString.h"
#include "httplib.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// USB Reset (macOS only)
// ---------------------------------------------------------------------------

#if defined(__APPLE__)
static bool resetUSBDevice(uint16_t vendorId, uint16_t productId)
{
    CFMutableDictionaryRef matchDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matchDict) return false;

    CFNumberRef vidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &vendorId);
    CFNumberRef pidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &productId);
    CFDictionarySetValue(matchDict, CFSTR(kUSBVendorID), vidRef);
    CFDictionarySetValue(matchDict, CFSTR(kUSBProductID), pidRef);
    CFRelease(vidRef);
    CFRelease(pidRef);

    io_iterator_t iterator = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchDict, &iterator);
    if (kr != KERN_SUCCESS) return false;

    bool resetOk = false;
    io_service_t usbDevice;
    while ((usbDevice = IOIteratorNext(iterator)) != 0) {
        IOCFPlugInInterface** plugIn = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(usbDevice, kIOUSBDeviceUserClientTypeID,
                                                kIOCFPlugInInterfaceID, &plugIn, &score);
        IOObjectRelease(usbDevice);
        if (kr != KERN_SUCCESS || !plugIn) continue;

        IOUSBDeviceInterface** dev = nullptr;
        (*plugIn)->QueryInterface(plugIn,
            CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
            (LPVOID*)&dev);
        (*plugIn)->Release(plugIn);
        if (!dev) continue;

        kr = (*dev)->USBDeviceOpen(dev);
        if (kr == KERN_SUCCESS) {
            kr = (*dev)->USBDeviceReEnumerate(dev, 0);
            (*dev)->USBDeviceClose(dev);
        }
        (*dev)->Release(dev);

        if (kr == KERN_SUCCESS) {
            std::cout << "  USB device reset successful.\n";
            resetOk = true;
        } else {
            std::cout << "  USB device reset returned: " << kr << "\n";
        }
    }
    IOObjectRelease(iterator);
    return resetOk;
}

static const uint16_t kSonyVendorId = 0x054c;
static const uint16_t kFX30ProductId = 0x0e10;
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isFX30Camera(const SCRSDK::ICrCameraObjectInfo* objInfo)
{
    CrString model = objInfo->GetModel();
    return model.find(CRSTR("FX30")) != CrString::npos;
}

static CrString getModelId(const SCRSDK::ICrCameraObjectInfo* objInfo)
{
    CrString id;
    if (CrString(objInfo->GetConnectionTypeName()) == CRSTR("IP")) {
        id = CrString(objInfo->GetMACAddressChar());
    } else {
        id = CrString((CrChar*)objInfo->GetId());
    }
    return CrString(objInfo->GetModel()).append(CRSTR(" (")).append(id).append(CRSTR(")"));
}

// JSON string escaping
static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Escape control characters as \u00XX
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Property formatting helpers
// ---------------------------------------------------------------------------

static std::string formatFNumber(uint16_t val)
{
    if (val == 0) return "---";
    int whole = val / 100;
    int frac = val % 100;
    if (frac == 0)
        return "F" + std::to_string(whole);
    else if (frac % 10 == 0)
        return "F" + std::to_string(whole) + "." + std::to_string(frac / 10);
    else
        return "F" + std::to_string(whole) + "." + std::to_string(frac);
}

static std::string formatShutterSpeed(uint32_t val)
{
    if (val == 0) return "---";
    // Special: bulb
    if (val == 0x00000000) return "Bulb";
    uint16_t hi = (uint16_t)(val >> 16);
    uint16_t lo = (uint16_t)(val & 0xFFFF);
    if (hi == 0 && lo == 0) return "Bulb";
    if (hi == 1)
        return "1/" + std::to_string(lo);
    if (lo == 10)
        return std::to_string(hi) + "." + std::to_string(lo % 10) + "\"";
    if (hi > 1 && lo == 1)
        return std::to_string(hi) + "\"";
    if (hi == 0)
        return "1/" + std::to_string(lo);
    return std::to_string(hi) + "/" + std::to_string(lo);
}

static std::string formatISO(uint32_t val)
{
    if (val == 0 || val == 0xFFFFFF) return "---";
    uint32_t isoVal = val & 0x00FFFFFF;
    uint8_t mode = (uint8_t)((val >> 24) & 0x0F);
    if (mode == 0 && isoVal == 0) return "ISO AUTO";
    std::string prefix = (mode != 0) ? "ISO AUTO " : "ISO ";
    return prefix + std::to_string(isoVal);
}

static std::string formatWhiteBalance(uint16_t val)
{
    switch (val) {
        case SCRSDK::CrWhiteBalance_AWB:                    return "AWB";
        case SCRSDK::CrWhiteBalance_Underwater_Auto:        return "Underwater";
        case SCRSDK::CrWhiteBalance_Daylight:               return "Daylight";
        case SCRSDK::CrWhiteBalance_Shadow:                 return "Shadow";
        case SCRSDK::CrWhiteBalance_Cloudy:                 return "Cloudy";
        case SCRSDK::CrWhiteBalance_Tungsten:               return "Tungsten";
        case SCRSDK::CrWhiteBalance_Fluorescent:            return "Fluorescent";
        case SCRSDK::CrWhiteBalance_Fluorescent_WarmWhite:  return "Fluor WarmWhite";
        case SCRSDK::CrWhiteBalance_Fluorescent_CoolWhite:  return "Fluor CoolWhite";
        case SCRSDK::CrWhiteBalance_Fluorescent_DayWhite:   return "Fluor DayWhite";
        case SCRSDK::CrWhiteBalance_Fluorescent_Daylight:   return "Fluor Daylight";
        case SCRSDK::CrWhiteBalance_Flush:                  return "Flash";
        case SCRSDK::CrWhiteBalance_ColorTemp:              return "ColorTemp";
        case SCRSDK::CrWhiteBalance_Custom_1:               return "Custom 1";
        case SCRSDK::CrWhiteBalance_Custom_2:               return "Custom 2";
        case SCRSDK::CrWhiteBalance_Custom_3:               return "Custom 3";
        case SCRSDK::CrWhiteBalance_Custom:                 return "Custom";
        default: return "WB " + std::to_string(val);
    }
}

static std::string formatMovieFormat(uint16_t val)
{
    switch (val) {
        case SCRSDK::CrFileFormatMovie_AVCHD:          return "AVCHD";
        case SCRSDK::CrFileFormatMovie_MP4:            return "MP4";
        case SCRSDK::CrFileFormatMovie_XAVC_S_4K:     return "XAVC S 4K";
        case SCRSDK::CrFileFormatMovie_XAVC_S_HD:     return "XAVC S HD";
        case SCRSDK::CrFileFormatMovie_XAVC_HS_8K:    return "XAVC HS 8K";
        case SCRSDK::CrFileFormatMovie_XAVC_HS_4K:    return "XAVC HS 4K";
        case SCRSDK::CrFileFormatMovie_XAVC_S_L_4K:   return "XAVC S-L 4K";
        case SCRSDK::CrFileFormatMovie_XAVC_S_L_HD:   return "XAVC S-L HD";
        case SCRSDK::CrFileFormatMovie_XAVC_S_I_4K:   return "XAVC S-I 4K";
        case SCRSDK::CrFileFormatMovie_XAVC_S_I_HD:   return "XAVC S-I HD";
        case SCRSDK::CrFileFormatMovie_XAVC_I:        return "XAVC I";
        case SCRSDK::CrFileFormatMovie_XAVC_L:        return "XAVC L";
        case SCRSDK::CrFileFormatMovie_XAVC_HS_HD:    return "XAVC HS HD";
        case SCRSDK::CrFileFormatMovie_XAVC_S_I_DCI_4K: return "XAVC S-I DCI 4K";
        default: return "Format " + std::to_string(val);
    }
}

static std::string formatRecSetting(uint16_t val)
{
    switch (val) {
        case SCRSDK::CrRecordingSettingMovie_60p_50M:    return "60p 50M";
        case SCRSDK::CrRecordingSettingMovie_30p_50M:    return "30p 50M";
        case SCRSDK::CrRecordingSettingMovie_24p_50M:    return "24p 50M";
        case SCRSDK::CrRecordingSettingMovie_50p_50M:    return "50p 50M";
        case SCRSDK::CrRecordingSettingMovie_25p_50M:    return "25p 50M";
        case SCRSDK::CrRecordingSettingMovie_600M_422_10bit:  return "600M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_500M_422_10bit:  return "500M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_400M_420_10bit:  return "400M 420 10bit";
        case SCRSDK::CrRecordingSettingMovie_300M_422_10bit:  return "300M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_280M_422_10bit:  return "280M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_250M_422_10bit:  return "250M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_200M_422_10bit:  return "200M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_200M_420_10bit:  return "200M 420 10bit";
        case SCRSDK::CrRecordingSettingMovie_200M_420_8bit:   return "200M 420 8bit";
        case SCRSDK::CrRecordingSettingMovie_150M_420_10bit:  return "150M 420 10bit";
        case SCRSDK::CrRecordingSettingMovie_150M_420_8bit:   return "150M 420 8bit";
        case SCRSDK::CrRecordingSettingMovie_100M_422_10bit:  return "100M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_100M_420_10bit:  return "100M 420 10bit";
        case SCRSDK::CrRecordingSettingMovie_100M_420_8bit:   return "100M 420 8bit";
        case SCRSDK::CrRecordingSettingMovie_50M_422_10bit:   return "50M 422 10bit";
        case SCRSDK::CrRecordingSettingMovie_50M_420_10bit:   return "50M 420 10bit";
        case SCRSDK::CrRecordingSettingMovie_50M_420_8bit:    return "50M 420 8bit";
        default: return "RecSet " + std::to_string(val);
    }
}

static std::string formatFrameRate(uint16_t val)
{
    switch (val) {
        case SCRSDK::CrRecordingFrameRateSettingMovie_120p:     return "120p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_100p:     return "100p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_60p:      return "60p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_50p:      return "50p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_30p:      return "30p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_25p:      return "25p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_24p:      return "24p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_23_98p:   return "23.98p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_29_97p:   return "29.97p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_59_94p:   return "59.94p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_24_00p:   return "24.00p";
        case SCRSDK::CrRecordingFrameRateSettingMovie_119_88p:  return "119.88p";
        default: return "FR " + std::to_string(val);
    }
}

// ---------------------------------------------------------------------------
// CameraDevice
// ---------------------------------------------------------------------------

// Convert CrInt16u* (UTF-16) string to std::string (ASCII/UTF-8 approximation)
static std::string crStr16ToString(const CrInt16u* str)
{
    if (!str) return "";
    std::string out;
    for (int i = 0; str[i] != 0; i++) {
        if (str[i] >= 0x20 && str[i] < 128) out += (char)str[i];
        // skip control chars and non-ASCII
    }
    return out;
}

// Persistent per-day event log (connect/disconnect/scan); defined later.
static void logEvent(const std::string& msg);

struct CameraProperties {
    int battery = -1;
    std::string iso = "---";
    std::string shutterSpeed = "---";
    std::string fNumber = "---";
    std::string whiteBalance = "---";
    int colorTemp = 0;
    int mediaSlot1Min = -1;
    int mediaSlot2Min = -1;
    std::string movieFormat = "---";
    std::string recSetting = "---";
    std::string frameRate = "---";
    bool recording = false;
    std::string clipName;
    int heatState = 0; // 0=ok, 1=pre-overheat, 2=overheat
};

class CameraDevice : public SCRSDK::IDeviceCallback
{
public:
    int64_t  m_device_handle = 0;
    bool     m_connected = false;
    bool     m_reconnecting = false;
    CrString m_modelId;

    std::mutex m_eventPromiseMutex;
    std::promise<void>* m_eventPromise = nullptr;

    // For contents transfer download completion
    std::mutex m_downloadMutex;
    std::promise<void>* m_downloadPromise = nullptr;

    void setEventPromise(std::promise<void>* p)
    {
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        m_eventPromise = p;
    }

    void setDownloadPromise(std::promise<void>* p)
    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_downloadPromise = p;
    }

    CameraDevice() {}
    ~CameraDevice() {}

    // IDeviceCallback overrides

    void OnConnected(SCRSDK::DeviceConnectionVersioin version)
    {
        m_connected = true;
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if (m_eventPromise) {
            m_eventPromise->set_value();
            m_eventPromise = nullptr;
        }
    }

    void OnDisconnected(CrInt32u error)
    {
        m_connected = false;
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if (m_eventPromise) {
            m_eventPromise->set_value();
            m_eventPromise = nullptr;
        }
    }

    void OnError(CrInt32u error)
    {
        std::cout << "  Error on " << std::string(m_modelId.begin(), m_modelId.end())
                  << ": " << CrErrorString(error) << "\n";
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if (m_eventPromise) {
            m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("connection error")));
            m_eventPromise = nullptr;
        }
    }

    void OnWarning(CrInt32u warning)
    {
        if (warning == SCRSDK::CrWarning_Connect_Reconnecting) {
            CrCout << "  Reconnecting to " << m_modelId << "\n";
            m_reconnecting = true;
            return;
        }
        // ContentsTransfer warnings → fail the download promise
        if (warning == SCRSDK::CrWarning_ContentsTransferMode_DeviceBusy ||
            warning == SCRSDK::CrWarning_ContentsTransferMode_StatusError ||
            warning == SCRSDK::CrWarning_ContentsTransferMode_CanceledFromCamera) {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            if (m_downloadPromise) {
                m_downloadPromise->set_exception(std::make_exception_ptr(std::runtime_error("transfer warning")));
                m_downloadPromise = nullptr;
            }
        }
    }

    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3) {}

    void OnCompleteDownload(CrChar* filename, CrInt32u type)
    {
        CrCout << "  Download complete: " << filename << "\n";
    }

    void OnNotifyContentsTransfer(CrInt32u notify, SCRSDK::CrContentHandle contentHandle, CrChar* filename)
    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        switch (notify) {
        case SCRSDK::CrNotify_ContentsTransfer_Start:
            break;
        case SCRSDK::CrNotify_ContentsTransfer_Complete:
            if (m_downloadPromise) {
                m_downloadPromise->set_value();
                m_downloadPromise = nullptr;
            }
            break;
        default:
            if (m_downloadPromise) {
                m_downloadPromise->set_exception(std::make_exception_ptr(std::runtime_error("transfer error")));
                m_downloadPromise = nullptr;
            }
            break;
        }
    }

    void OnLvPropertyChanged() {}
    void OnLvPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
    void OnPropertyChanged() {}
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}

    // Connect to a camera in Remote mode with retry logic
    bool connect(const SCRSDK::ICrCameraObjectInfo* objInfo, int maxRetries = 3)
    {
        m_modelId = getModelId(objInfo);

        for (int attempt = 1; attempt <= maxRetries; attempt++) {
            std::promise<void> eventPromise;
            std::future<void> eventFuture = eventPromise.get_future();
            setEventPromise(&eventPromise);

            SCRSDK::CrError err = SCRSDK::Connect(
                (SCRSDK::ICrCameraObjectInfo*)objInfo, this, &m_device_handle,
                SCRSDK::CrSdkControlMode_Remote,
                SCRSDK::CrReconnecting_ON);
            if (err) {
                CrCout << "  Attempt " << attempt << "/" << maxRetries
                       << " failed for " << m_modelId << ": " << CrErrorString(err).c_str() << "\n";
                setEventPromise(nullptr);
                if (m_device_handle) {
                    SCRSDK::ReleaseDevice(m_device_handle);
                    m_device_handle = 0;
                }
                if (attempt < maxRetries) {
#if defined(__APPLE__)
                    std::cout << "  Resetting USB connection...\n";
                    resetUSBDevice(kSonyVendorId, kFX30ProductId);
                    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
#else
                    std::cout << "  Retrying in 3 seconds...\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
#endif
                }
                continue;
            }

            try {
                eventFuture.get();
            } catch (const std::exception&) {
                CrCout << "  Attempt " << attempt << "/" << maxRetries
                       << " connection error for " << m_modelId << "\n";
                if (m_device_handle) {
                    SCRSDK::ReleaseDevice(m_device_handle);
                    m_device_handle = 0;
                }
                if (attempt < maxRetries) {
#if defined(__APPLE__)
                    std::cout << "  Resetting USB connection...\n";
                    resetUSBDevice(kSonyVendorId, kFX30ProductId);
                    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
#else
                    std::cout << "  Retrying in 3 seconds...\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
#endif
                }
                continue;
            }

            CrCout << "  Connected: " << m_modelId << "\n";
            logEvent("CONNECTED " + std::string(m_modelId.begin(), m_modelId.end()));
            return true;
        }

        CrCout << "  Failed to connect " << m_modelId << " after " << maxRetries << " attempts.\n";
        logEvent("CONNECT_FAILED " + std::string(m_modelId.begin(), m_modelId.end())
                 + " (after " + std::to_string(maxRetries) + " attempts)");
        return false;
    }

    // Connect in ContentsTransfer mode (for file download)
    bool connectContentsTransfer(const SCRSDK::ICrCameraObjectInfo* objInfo)
    {
        m_modelId = getModelId(objInfo);

        std::promise<void> eventPromise;
        std::future<void> eventFuture = eventPromise.get_future();
        setEventPromise(&eventPromise);

        SCRSDK::CrError err = SCRSDK::Connect(
            (SCRSDK::ICrCameraObjectInfo*)objInfo, this, &m_device_handle,
            SCRSDK::CrSdkControlMode_ContentsTransfer,
            SCRSDK::CrReconnecting_ON);
        if (err) {
            CrCout << "  ContentsTransfer connect failed for " << m_modelId
                   << ": " << CrErrorString(err).c_str() << "\n";
            setEventPromise(nullptr);
            if (m_device_handle) {
                SCRSDK::ReleaseDevice(m_device_handle);
                m_device_handle = 0;
            }
            return false;
        }

        try {
            eventFuture.get();
        } catch (const std::exception&) {
            CrCout << "  ContentsTransfer connection error for " << m_modelId << "\n";
            if (m_device_handle) {
                SCRSDK::ReleaseDevice(m_device_handle);
                m_device_handle = 0;
            }
            return false;
        }

        CrCout << "  ContentsTransfer connected: " << m_modelId << "\n";
        return true;
    }

    void disconnect()
    {
        if (m_connected) {
            std::promise<void> eventPromise;
            std::future<void> eventFuture = eventPromise.get_future();
            setEventPromise(&eventPromise);
            SCRSDK::Disconnect(m_device_handle);
            eventFuture.wait_for(std::chrono::milliseconds(3000));
            m_connected = false;
        }
        if (m_device_handle) {
            SCRSDK::ReleaseDevice(m_device_handle);
            m_device_handle = 0;
        }
    }

    bool startRecording()
    {
        if (!m_connected) return false;
        SCRSDK::CrError err = SCRSDK::SendCommand(
            m_device_handle,
            SCRSDK::CrCommandId_MovieRecord,
            SCRSDK::CrCommandParam_Down);
        return err == SCRSDK::CrError_None;
    }

    bool stopRecording()
    {
        if (!m_connected) return false;
        SCRSDK::CrError err = SCRSDK::SendCommand(
            m_device_handle,
            SCRSDK::CrCommandId_MovieRecord,
            SCRSDK::CrCommandParam_Up);
        return err == SCRSDK::CrError_None;
    }

    // Format media slot 1 (quick format)
    bool formatSlot1()
    {
        if (!m_connected) return false;
        SCRSDK::CrError err = SCRSDK::ExecuteControlCodeValue(
            m_device_handle,
            SCRSDK::CrControlCode_SelectedMediaFormat,
            SCRSDK::CrMediaFormat_QuickFormatSlot1);
        return err == SCRSDK::CrError_None;
    }

    // Format media slot 2 (quick format)
    bool formatSlot2()
    {
        if (!m_connected) return false;
        SCRSDK::CrError err = SCRSDK::ExecuteControlCodeValue(
            m_device_handle,
            SCRSDK::CrControlCode_SelectedMediaFormat,
            SCRSDK::CrMediaFormat_QuickFormatSlot2);
        return err == SCRSDK::CrError_None;
    }

    // Batch-read all relevant properties
    CameraProperties getProperties()
    {
        CameraProperties props;
        if (!m_connected) return props;

        uint32_t codes[] = {
            SCRSDK::CrDeviceProperty_BatteryRemain,
            SCRSDK::CrDeviceProperty_IsoSensitivity,
            SCRSDK::CrDeviceProperty_ShutterSpeed,
            SCRSDK::CrDeviceProperty_FNumber,
            SCRSDK::CrDeviceProperty_WhiteBalance,
            SCRSDK::CrDeviceProperty_Colortemp,
            SCRSDK::CrDeviceProperty_RecordingState,
            SCRSDK::CrDeviceProperty_MediaSLOT1_RemainingTime,
            SCRSDK::CrDeviceProperty_MediaSLOT2_RemainingTime,
            SCRSDK::CrDeviceProperty_Movie_File_Format,
            SCRSDK::CrDeviceProperty_Movie_Recording_Setting,
            SCRSDK::CrDeviceProperty_Movie_Recording_FrameRateSetting,
            SCRSDK::CrDeviceProperty_RecorderClipName,
            SCRSDK::CrDeviceProperty_DeviceOverheatingState,
        };
        uint32_t numCodes = sizeof(codes) / sizeof(codes[0]);

        SCRSDK::CrDeviceProperty* prop_list = nullptr;
        std::int32_t nprop = 0;

        SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(
            m_device_handle, numCodes, codes, &prop_list, &nprop);
        if (err || !prop_list || nprop < 1) return props;

        for (int32_t i = 0; i < nprop; i++) {
            uint32_t code = prop_list[i].GetCode();
            uint64_t val = prop_list[i].GetCurrentValue();

            switch (code) {
            case SCRSDK::CrDeviceProperty_BatteryRemain:
                props.battery = (int)val;
                break;
            case SCRSDK::CrDeviceProperty_IsoSensitivity:
                props.iso = formatISO((uint32_t)val);
                break;
            case SCRSDK::CrDeviceProperty_ShutterSpeed:
                props.shutterSpeed = formatShutterSpeed((uint32_t)val);
                break;
            case SCRSDK::CrDeviceProperty_FNumber:
                props.fNumber = formatFNumber((uint16_t)val);
                break;
            case SCRSDK::CrDeviceProperty_WhiteBalance:
                props.whiteBalance = formatWhiteBalance((uint16_t)val);
                break;
            case SCRSDK::CrDeviceProperty_Colortemp:
                props.colorTemp = (int)val;
                break;
            case SCRSDK::CrDeviceProperty_RecordingState:
                props.recording = (val == SCRSDK::CrMovie_Recording_State_Recording);
                break;
            case SCRSDK::CrDeviceProperty_MediaSLOT1_RemainingTime:
                props.mediaSlot1Min = (int)val;
                break;
            case SCRSDK::CrDeviceProperty_MediaSLOT2_RemainingTime:
                props.mediaSlot2Min = (int)val;
                break;
            case SCRSDK::CrDeviceProperty_Movie_File_Format:
                props.movieFormat = formatMovieFormat((uint16_t)val);
                break;
            case SCRSDK::CrDeviceProperty_Movie_Recording_Setting:
                props.recSetting = formatRecSetting((uint16_t)val);
                break;
            case SCRSDK::CrDeviceProperty_Movie_Recording_FrameRateSetting:
                props.frameRate = formatFrameRate((uint16_t)val);
                break;
            case SCRSDK::CrDeviceProperty_RecorderClipName:
                {
                    CrInt16u* str = prop_list[i].GetCurrentStr();
                    if (str) props.clipName = crStr16ToString(str);
                }
                break;
            case SCRSDK::CrDeviceProperty_DeviceOverheatingState:
                props.heatState = (int)val;
                break;
            }
        }

        SCRSDK::ReleaseDeviceProperties(m_device_handle, prop_list);
        return props;
    }
};

// ---------------------------------------------------------------------------
// Global State
// ---------------------------------------------------------------------------

static std::mutex g_mutex;
static std::vector<std::unique_ptr<CameraDevice>> g_cameras;
static std::string g_downloadPath = "/tmp/fx30_downloads";
static std::atomic<bool> g_downloading{false};
static std::string g_downloadStatus;
static std::atomic<bool> g_scanning{false};
static std::string g_scanStatus;
static std::atomic<bool> g_running{true};
static std::string g_presetPath = "fx30_preset.json";
static std::atomic<bool> g_listing{false};
static std::string g_listStatus;
static std::string g_filesJson = "null";

// ---------------------------------------------------------------------------
// Settings Preset (save/restore camera properties)
// ---------------------------------------------------------------------------

// Property codes that can be saved and restored
static const uint32_t kPresetCodes[] = {
    SCRSDK::CrDeviceProperty_IsoSensitivity,
    SCRSDK::CrDeviceProperty_ShutterSpeed,
    SCRSDK::CrDeviceProperty_FNumber,
    SCRSDK::CrDeviceProperty_WhiteBalance,
    SCRSDK::CrDeviceProperty_Colortemp,
    SCRSDK::CrDeviceProperty_Movie_File_Format,
    SCRSDK::CrDeviceProperty_Movie_Recording_Setting,
    SCRSDK::CrDeviceProperty_Movie_Recording_FrameRateSetting,
};
static const size_t kPresetCodeCount = sizeof(kPresetCodes) / sizeof(kPresetCodes[0]);

static std::string presetCodeName(uint32_t code)
{
    switch (code) {
    case SCRSDK::CrDeviceProperty_IsoSensitivity:                return "iso";
    case SCRSDK::CrDeviceProperty_ShutterSpeed:                  return "shutterSpeed";
    case SCRSDK::CrDeviceProperty_FNumber:                       return "fNumber";
    case SCRSDK::CrDeviceProperty_WhiteBalance:                  return "whiteBalance";
    case SCRSDK::CrDeviceProperty_Colortemp:                     return "colorTemp";
    case SCRSDK::CrDeviceProperty_Movie_File_Format:             return "movieFormat";
    case SCRSDK::CrDeviceProperty_Movie_Recording_Setting:       return "recSetting";
    case SCRSDK::CrDeviceProperty_Movie_Recording_FrameRateSetting: return "frameRate";
    default: return "unknown_" + std::to_string(code);
    }
}

struct PresetEntry {
    uint32_t code;
    uint64_t value;
};

// Save current camera properties to JSON file
static bool savePreset(CameraDevice& cam, const std::string& path)
{
    if (!cam.m_connected) return false;

    SCRSDK::CrDeviceProperty* prop_list = nullptr;
    std::int32_t nprop = 0;
    uint32_t codes[kPresetCodeCount];
    for (size_t i = 0; i < kPresetCodeCount; i++) codes[i] = kPresetCodes[i];

    SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(
        cam.m_device_handle, (uint32_t)kPresetCodeCount, codes, &prop_list, &nprop);
    if (err || !prop_list || nprop < 1) return false;

    std::ostringstream js;
    js << "{\n";
    bool first = true;
    for (int32_t i = 0; i < nprop; i++) {
        uint32_t code = prop_list[i].GetCode();
        uint64_t val = prop_list[i].GetCurrentValue();
        if (!first) js << ",\n";
        first = false;
        js << "  \"" << presetCodeName(code) << "\": " << val;
    }
    js << "\n}\n";

    SCRSDK::ReleaseDeviceProperties(cam.m_device_handle, prop_list);

    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << js.str();
    f.close();
    std::cout << "Preset saved to " << path << "\n";
    return true;
}

// Load preset from JSON file, returns entries
static std::vector<PresetEntry> loadPreset(const std::string& path)
{
    std::vector<PresetEntry> entries;
    std::ifstream f(path);
    if (!f.is_open()) return entries;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Simple parser: find "key": value pairs
    for (size_t i = 0; i < kPresetCodeCount; i++) {
        std::string key = "\"" + presetCodeName(kPresetCodes[i]) + "\"";
        auto pos = content.find(key);
        if (pos == std::string::npos) continue;
        pos = content.find(':', pos + key.size());
        if (pos == std::string::npos) continue;
        pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
        uint64_t val = std::strtoull(content.c_str() + pos, nullptr, 10);
        entries.push_back({kPresetCodes[i], val});
    }

    return entries;
}

// Apply preset to a camera: read current values, set any that differ
static int applyPreset(CameraDevice& cam, const std::vector<PresetEntry>& entries)
{
    if (!cam.m_connected || entries.empty()) return 0;

    int applied = 0;
    for (const auto& entry : entries) {
        // Read current value
        uint32_t code = entry.code;
        SCRSDK::CrDeviceProperty* prop_list = nullptr;
        std::int32_t nprop = 0;
        SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(
            cam.m_device_handle, 1, &code, &prop_list, &nprop);
        if (err || !prop_list || nprop < 1) continue;

        SCRSDK::CrDeviceProperty devProp = prop_list[0];
        uint64_t current = devProp.GetCurrentValue();
        SCRSDK::ReleaseDeviceProperties(cam.m_device_handle, prop_list);

        if (current == entry.value) continue;

        // Value differs — set it
        std::string name = presetCodeName(code);
        std::cout << "  Setting " << name << ": " << current << " -> " << entry.value << "\n";

        devProp.SetCurrentValue(entry.value);
        err = SCRSDK::SetDeviceProperty(cam.m_device_handle, &devProp);
        if (err) {
            std::cout << "  Failed to set " << name << ": " << CrErrorString(err) << "\n";
        } else {
            applied++;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    return applied;
}

// ---------------------------------------------------------------------------
// Scan and Connect
// ---------------------------------------------------------------------------

// Must be called with g_mutex held. Sets g_scanning/g_scanStatus for UI feedback.
static void scanAndConnect(bool usbReset = false)
{
    g_scanning = true;

#if defined(__APPLE__)
    if (usbReset) {
        g_scanStatus = "Resetting USB devices...";
        std::cout << "Resetting USB...\n";
        resetUSBDevice(kSonyVendorId, kFX30ProductId);
        g_scanStatus = "Waiting for USB re-enumeration...";
        std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    }
#endif

    SCRSDK::ICrEnumCameraObjectInfo* enumInfo = nullptr;
    g_scanStatus = "Enumerating cameras...";
    std::cout << "Scanning for cameras (3 seconds)...\n";

    SCRSDK::CrError err = SCRSDK::EnumCameraObjects(&enumInfo, 3);
    if (err || !enumInfo) {
        std::cout << "No cameras found.\n";
        g_scanStatus = "No cameras found.";
        logEvent("SCAN_ENUMERATED 0 FX30 on USB: (none)");
        g_scanning = false;
        return;
    }

    uint32_t count = enumInfo->GetCount();
    uint32_t fx30Found = 0;
    uint32_t fx30Total = 0;

    // First pass: count FX30 cameras
    for (uint32_t i = 0; i < count; i++) {
        if (isFX30Camera(enumInfo->GetCameraObjectInfo(i))) fx30Total++;
    }

    // Record every FX30 physically present on the USB bus this scan — this is
    // what makes "was camera X powered on that day?" answerable after the fact.
    {
        std::string seen;
        for (uint32_t i = 0; i < count; i++) {
            auto* oi = enumInfo->GetCameraObjectInfo(i);
            if (!isFX30Camera(oi)) continue;
            CrString id = getModelId(oi);
            if (!seen.empty()) seen += ", ";
            seen += std::string(id.begin(), id.end());
        }
        logEvent("SCAN_ENUMERATED " + std::to_string(fx30Total) + " FX30 on USB: "
                 + (seen.empty() ? "(none)" : seen));
    }

    uint32_t fx30Attempted = 0;
    for (uint32_t i = 0; i < count; i++) {
        auto* objInfo = enumInfo->GetCameraObjectInfo(i);
        CrString model = objInfo->GetModel();

        if (!isFX30Camera(objInfo)) {
            CrCout << "  Skipping non-FX30: " << model << "\n";
            continue;
        }

        fx30Attempted++;
        std::string modelStr(model.begin(), model.end());

        // Check if already connected
        CrString id = getModelId(objInfo);
        bool alreadyConnected = false;
        for (auto& cam : g_cameras) {
            if (cam->m_modelId == id && cam->m_connected) {
                alreadyConnected = true;
                break;
            }
        }
        if (alreadyConnected) {
            CrCout << "  Already connected: " << id << "\n";
            g_scanStatus = "Already connected: " + modelStr +
                " (" + std::to_string(fx30Attempted) + "/" + std::to_string(fx30Total) + ")";
            continue;
        }

        g_scanStatus = "Connecting to " + modelStr +
            " (" + std::to_string(fx30Attempted) + "/" + std::to_string(fx30Total) + ")...";

        auto cam = std::make_unique<CameraDevice>();
        if (cam->connect(objInfo)) {
            g_cameras.push_back(std::move(cam));
            fx30Found++;
        }
    }

    enumInfo->Release();

    if (fx30Found == 0) {
        std::cout << "No new FX30 cameras found.\n";
        g_scanStatus = "Scan complete. No new cameras found.";
        logEvent("SCAN_RESULT newly_connected=0 total_connected="
                 + std::to_string(g_cameras.size()));
    } else {
        std::cout << fx30Found << " FX30 camera(s) connected. Total: " << g_cameras.size() << "\n";
        g_scanStatus = "Scan complete. " + std::to_string(fx30Found) + " new camera(s), " +
            std::to_string(g_cameras.size()) + " total.";
        logEvent("SCAN_RESULT newly_connected=" + std::to_string(fx30Found)
                 + " total_connected=" + std::to_string(g_cameras.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // Apply preset if file exists
        auto preset = loadPreset(g_presetPath);
        if (!preset.empty()) {
            g_scanStatus = "Applying preset...";
            std::cout << "Applying preset from " << g_presetPath << "...\n";
            for (auto& cam : g_cameras) {
                if (!cam->m_connected) continue;
                std::string name(cam->m_modelId.begin(), cam->m_modelId.end());
                g_scanStatus = "Applying preset to " + name + "...";
                int n = applyPreset(*cam, preset);
                std::cout << "  " << name << ": " << n << " setting(s) applied.\n";
            }
            g_scanStatus = "Preset applied. " + std::to_string(g_cameras.size()) + " camera(s) ready.";
        }
    }

    g_scanning = false;
}

// ---------------------------------------------------------------------------
// Camera management thread
// ---------------------------------------------------------------------------

static void cameraManagementThread()
{
    // USB reset on startup to clear stale connections from previous process
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        scanAndConnect(true);
    }

    // Let the initial connection stabilize before monitoring
    std::this_thread::sleep_for(std::chrono::seconds(15));

    int disconnectedSecs = 0;
    const int kResetAfterSecs = 20;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!g_running || g_downloading || g_scanning || g_listing) { disconnectedSecs = 0; continue; }

        std::lock_guard<std::mutex> lock(g_mutex);

        // Clear reconnecting flags for cameras that recovered
        for (auto& cam : g_cameras) {
            if (cam->m_connected && cam->m_reconnecting) {
                std::string name(cam->m_modelId.begin(), cam->m_modelId.end());
                std::cout << "  Reconnected: " << name << "\n";
                logEvent("RECONNECTED " + name);
                cam->m_reconnecting = false;
            }
        }

        // Check if all cameras are OK
        bool allOk = !g_cameras.empty();
        for (auto& cam : g_cameras) {
            if (!cam->m_connected) {
                allOk = false;
                break;
            }
        }

        if (allOk) {
            disconnectedSecs = 0;
            continue;
        }

        disconnectedSecs += 5;
        std::cout << "Camera(s) disconnected for " << disconnectedSecs << "s...\n";

        // Only do a full tear-down after sustained disconnection
        if (disconnectedSecs >= kResetAfterSecs) {
            std::cout << "Resetting after " << disconnectedSecs << "s disconnection...\n";
            std::string lost;
            for (auto& cam : g_cameras) {
                if (!cam->m_connected) {
                    if (!lost.empty()) lost += ", ";
                    lost += std::string(cam->m_modelId.begin(), cam->m_modelId.end());
                }
            }
            logEvent("DISCONNECTED after " + std::to_string(disconnectedSecs)
                     + "s: " + (lost.empty() ? "(unknown)" : lost) + " — resetting");
            for (auto& cam : g_cameras) cam->disconnect();
            g_cameras.clear();
            scanAndConnect(true);
            disconnectedSecs = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// File Download
// ---------------------------------------------------------------------------

static void downloadFilesThread()
{
    g_downloading = true;
    logEvent("DOWNLOAD started");
    std::string dlPath;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = "Starting download...";
        dlPath = g_downloadPath;
    }

    // Create download directory
    try {
        fs::create_directories(dlPath);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = std::string("Error creating directory: ") + e.what();
        g_downloading = false;
        return;
    }

    // Step 1: Disconnect all cameras from Remote mode
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = "Disconnecting cameras from Remote mode...";
        for (auto& cam : g_cameras) cam->disconnect();
        g_cameras.clear();
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Step 2: Enumerate and connect in ContentsTransfer mode
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = "Scanning for cameras in ContentsTransfer mode...";
    }

    SCRSDK::ICrEnumCameraObjectInfo* enumInfo = nullptr;
    SCRSDK::CrError err = SCRSDK::EnumCameraObjects(&enumInfo, 3);
    if (err || !enumInfo) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = "Error: No cameras found for download.";
        // Try to reconnect in Remote mode
        scanAndConnect(false);
        g_downloading = false;
        return;
    }

    uint32_t camCount = enumInfo->GetCount();
    std::vector<std::unique_ptr<CameraDevice>> dlCameras;

    for (uint32_t i = 0; i < camCount; i++) {
        auto* objInfo = enumInfo->GetCameraObjectInfo(i);
        if (!isFX30Camera(objInfo)) continue;

        auto cam = std::make_unique<CameraDevice>();
        if (cam->connectContentsTransfer(objInfo)) {
            dlCameras.push_back(std::move(cam));
        }
    }
    enumInfo->Release();

    if (dlCameras.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = "Error: Could not connect to any camera in ContentsTransfer mode.";
        scanAndConnect(false);
        g_downloading = false;
        return;
    }

    // Step 3: Wait for ContentsTransfer to become ready
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Step 4: Download files from each camera
    int totalFiles = 0;
    int skippedFiles = 0;
    int downloadedFiles = 0;
    int errorFiles = 0;

    for (size_t ci = 0; ci < dlCameras.size(); ci++) {
        auto& cam = dlCameras[ci];
        std::string camName(cam->m_modelId.begin(), cam->m_modelId.end());

        // Set save info
        err = SCRSDK::SetSaveInfo(cam->m_device_handle,
            const_cast<CrChar*>(dlPath.c_str()),
            const_cast<CrChar*>(CRSTR("")), -1);
        if (err) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_downloadStatus = "Error: SetSaveInfo failed for " + camName;
            continue;
        }

        // Get folder list
        SCRSDK::CrMtpFolderInfo* folderList = nullptr;
        CrInt32u folderCount = 0;
        err = SCRSDK::GetDateFolderList(cam->m_device_handle, &folderList, &folderCount);
        if (err || !folderList || folderCount == 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_downloadStatus = "No folders found on " + camName;
            continue;
        }

        for (CrInt32u fi = 0; fi < folderCount; fi++) {
            SCRSDK::CrContentHandle* contentHandles = nullptr;
            CrInt32u contentCount = 0;

            err = SCRSDK::GetContentsHandleList(cam->m_device_handle,
                folderList[fi].handle, &contentHandles, &contentCount);
            if (err || !contentHandles || contentCount == 0) continue;

            for (CrInt32u ci2 = 0; ci2 < contentCount; ci2++) {
                SCRSDK::CrMtpContentsInfo info;
                err = SCRSDK::GetContentsDetailInfo(cam->m_device_handle,
                    contentHandles[ci2], &info);
                if (err) { errorFiles++; continue; }

                totalFiles++;
                std::string fileName(info.fileName);

                // Skip if file already exists — either still in the download
                // dir, or already sorted into the per-recording-date folder
                // next to it (../YYYY-MM-DD/raw/<file>, the sync client moves
                // clips there after each download).
                std::string fullPath = dlPath + "/" + fileName;
                bool alreadyHave = fs::exists(fullPath);
                if (!alreadyHave && fileName.size() >= 14) {
                    std::string d = fileName.substr(1, 8);
                    if (d.find_first_not_of("0123456789") == std::string::npos) {
                        std::string dateDir = d.substr(0, 4) + "-" + d.substr(4, 2)
                                              + "-" + d.substr(6, 2);
                        fs::path sorted = fs::path(dlPath).parent_path()
                                          / dateDir / "raw" / fileName;
                        alreadyHave = fs::exists(sorted);
                    }
                }
                if (alreadyHave) {
                    skippedFiles++;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_downloadStatus = "Skipped (exists): " + fileName +
                            " [" + std::to_string(downloadedFiles + skippedFiles) + "/" + std::to_string(totalFiles) + "]";
                    }
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_downloadStatus = "Downloading: " + fileName +
                        " from " + camName +
                        " [" + std::to_string(downloadedFiles + skippedFiles + 1) + "/" + std::to_string(totalFiles) + "]";
                }

                // Pull file
                std::promise<void> dlPromise;
                std::future<void> dlFuture = dlPromise.get_future();
                cam->setDownloadPromise(&dlPromise);

                err = SCRSDK::PullContentsFile(cam->m_device_handle,
                    contentHandles[ci2], SCRSDK::CrPropertyStillImageTransSize_Original);
                if (err) {
                    cam->setDownloadPromise(nullptr);
                    errorFiles++;
                    continue;
                }

                // Wait for download completion (timeout 5 minutes per file)
                try {
                    auto status = dlFuture.wait_for(std::chrono::minutes(5));
                    if (status == std::future_status::timeout) {
                        errorFiles++;
                        cam->setDownloadPromise(nullptr);
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_downloadStatus = "Timeout downloading: " + fileName;
                        continue;
                    }
                    dlFuture.get();
                    downloadedFiles++;
                } catch (const std::exception&) {
                    errorFiles++;
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_downloadStatus = "Error downloading: " + fileName;
                }

                // Brief pause between files (workaround per SDK sample)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            SCRSDK::ReleaseContentsHandleList(cam->m_device_handle, contentHandles);
        }

        SCRSDK::ReleaseDateFolderList(cam->m_device_handle, folderList);
    }

    // Step 5: Disconnect from ContentsTransfer
    for (auto& cam : dlCameras) cam->disconnect();
    dlCameras.clear();

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Step 6: Reconnect in Remote mode
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadStatus = "Download complete. Downloaded: " + std::to_string(downloadedFiles) +
            ", Skipped: " + std::to_string(skippedFiles) +
            ", Errors: " + std::to_string(errorFiles) +
            ". Reconnecting...";
        scanAndConnect(false);
        g_downloadStatus = "Download complete. Downloaded: " + std::to_string(downloadedFiles) +
            ", Skipped: " + std::to_string(skippedFiles) +
            ", Errors: " + std::to_string(errorFiles);
    }

    logEvent("DOWNLOAD complete: downloaded=" + std::to_string(downloadedFiles)
             + " skipped=" + std::to_string(skippedFiles)
             + " errors=" + std::to_string(errorFiles));
    g_downloading = false;
}

// ---------------------------------------------------------------------------
// List files on cameras (ContentsTransfer enumeration only, no download)
// ---------------------------------------------------------------------------
static void listFilesThread()
{
    g_listing = true;

    // Step 1: Disconnect all cameras from Remote mode
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_listStatus = "Disconnecting cameras from Remote mode...";
        for (auto& cam : g_cameras) cam->disconnect();
        g_cameras.clear();
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Step 2: Enumerate and connect in ContentsTransfer mode
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_listStatus = "Scanning for cameras in ContentsTransfer mode...";
    }

    SCRSDK::ICrEnumCameraObjectInfo* enumInfo = nullptr;
    SCRSDK::CrError err = SCRSDK::EnumCameraObjects(&enumInfo, 3);
    if (err || !enumInfo) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_listStatus = "Error: No cameras found for listing.";
        scanAndConnect(false);
        g_listing = false;
        return;
    }

    uint32_t camCount = enumInfo->GetCount();
    std::vector<std::unique_ptr<CameraDevice>> listCameras;
    for (uint32_t i = 0; i < camCount; i++) {
        auto* objInfo = enumInfo->GetCameraObjectInfo(i);
        if (!isFX30Camera(objInfo)) continue;
        auto cam = std::make_unique<CameraDevice>();
        if (cam->connectContentsTransfer(objInfo)) {
            listCameras.push_back(std::move(cam));
        }
    }
    enumInfo->Release();

    if (listCameras.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_listStatus = "Error: Could not connect to any camera in ContentsTransfer mode.";
        scanAndConnect(false);
        g_listing = false;
        return;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Step 3: Enumerate file names per camera
    std::ostringstream js;
    js << "{\"cameras\":{";
    int total = 0;
    int errorFiles = 0;
    for (size_t ci = 0; ci < listCameras.size(); ci++) {
        auto& cam = listCameras[ci];
        std::string camName(cam->m_modelId.begin(), cam->m_modelId.end());
        if (ci > 0) js << ",";
        js << "\"" << jsonEscape(camName) << "\":[";

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_listStatus = "Listing files on " + camName + "...";
        }

        bool first = true;
        SCRSDK::CrMtpFolderInfo* folderList = nullptr;
        CrInt32u folderCount = 0;
        err = SCRSDK::GetDateFolderList(cam->m_device_handle, &folderList, &folderCount);
        if (!err && folderList && folderCount > 0) {
            for (CrInt32u fi = 0; fi < folderCount; fi++) {
                SCRSDK::CrContentHandle* contentHandles = nullptr;
                CrInt32u contentCount = 0;
                err = SCRSDK::GetContentsHandleList(cam->m_device_handle,
                    folderList[fi].handle, &contentHandles, &contentCount);
                if (err || !contentHandles || contentCount == 0) continue;

                for (CrInt32u ci2 = 0; ci2 < contentCount; ci2++) {
                    SCRSDK::CrMtpContentsInfo info;
                    err = SCRSDK::GetContentsDetailInfo(cam->m_device_handle,
                        contentHandles[ci2], &info);
                    if (err) { errorFiles++; continue; }
                    if (!first) js << ",";
                    js << "\"" << jsonEscape(std::string(info.fileName)) << "\"";
                    first = false;
                    total++;
                }
                SCRSDK::ReleaseContentsHandleList(cam->m_device_handle, contentHandles);
            }
            SCRSDK::ReleaseDateFolderList(cam->m_device_handle, folderList);
        }
        js << "]";
    }
    js << "},\"total\":" << total << ",\"errors\":" << errorFiles << "}";

    // Step 4: Disconnect and reconnect in Remote mode
    for (auto& cam : listCameras) cam->disconnect();
    listCameras.clear();
    std::this_thread::sleep_for(std::chrono::seconds(2));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_filesJson = js.str();
        g_listStatus = "Listing complete. " + std::to_string(total) + " file(s) on " +
            std::to_string(camCount) + " camera(s). Reconnecting...";
        scanAndConnect(false);
        g_listStatus = "Listing complete. " + std::to_string(total) + " file(s).";
    }

    g_listing = false;
}

// ---------------------------------------------------------------------------
// Capture log: authoritative record of triggered captures (per date), written
// at /api/start time so the clip names are exact. File format matches the
// PyQt controller's capture_logs/{date}.json so both can read the same files.
// ---------------------------------------------------------------------------
static std::string g_captureLogDir = "capture_logs";

struct CaptureEntry {
    std::string time;                                        // ISO timestamp
    std::vector<std::pair<std::string, std::string>> clips;  // serial -> file
};

static std::string localDateString()
{
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
    return buf;
}

static std::string localTimeIso()
{
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
    return buf;
}

// Append a line to the persistent per-day event log. Survives restarts, so
// "which cameras were up on day X?" is answerable after the fact. Lives next to
// the capture logs (capture_logs/events-YYYY-MM-DD.log).
static std::mutex g_eventLogMutex;
static void logEvent(const std::string& msg)
{
    try { fs::create_directories(g_captureLogDir); } catch (...) {}
    std::lock_guard<std::mutex> lk(g_eventLogMutex);
    std::ofstream f(g_captureLogDir + "/events-" + localDateString() + ".log",
                    std::ios::app);
    if (f.good()) f << localTimeIso() << "  " << msg << "\n";
}

// Extract the JSON string that starts at the first '"' at/after pos.
static std::string extractJsonString(const std::string& s, size_t& pos)
{
    pos = s.find('"', pos);
    if (pos == std::string::npos) return "";
    std::string out;
    for (pos++; pos < s.size(); pos++) {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) { out += s[++pos]; continue; }
        if (c == '"') { pos++; break; }
        out += c;
    }
    return out;
}

// Tolerant parser for our own / the Python app's capture log files.
static std::vector<CaptureEntry> loadCaptures(const std::string& date)
{
    std::vector<CaptureEntry> entries;
    std::ifstream f(g_captureLogDir + "/" + date + ".json");
    if (!f.good()) return entries;
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();

    size_t pos = 0;
    while ((pos = s.find("\"time\"", pos)) != std::string::npos) {
        CaptureEntry e;
        size_t p = pos + 6;
        p = s.find(':', p); if (p == std::string::npos) break;
        p++;
        e.time = extractJsonString(s, p);
        size_t clipsPos = s.find("\"clips\"", p);
        if (clipsPos == std::string::npos) break;
        size_t open = s.find('{', clipsPos);
        if (open == std::string::npos) break;
        size_t close = s.find('}', open);
        if (close == std::string::npos) break;
        size_t q = open + 1;
        while (q < close) {
            std::string serial = extractJsonString(s, q);
            if (serial.empty() || q >= close) break;
            q = s.find(':', q); if (q == std::string::npos || q >= close) break;
            q++;
            std::string file = extractJsonString(s, q);
            if (!file.empty()) e.clips.push_back({serial, file});
        }
        if (!e.clips.empty()) entries.push_back(e);
        pos = close;
    }
    return entries;
}

static void saveCaptures(const std::string& date,
                         const std::vector<CaptureEntry>& entries)
{
    try { fs::create_directories(g_captureLogDir); } catch (...) {}
    std::ofstream f(g_captureLogDir + "/" + date + ".json", std::ios::trunc);
    f << "[";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i) f << ",";
        f << "\n  {\n    \"time\": \"" << jsonEscape(entries[i].time)
          << "\",\n    \"clips\": {";
        for (size_t j = 0; j < entries[i].clips.size(); j++) {
            if (j) f << ",";
            f << "\n      \"" << jsonEscape(entries[i].clips[j].first) << "\": \""
              << jsonEscape(entries[i].clips[j].second) << "\"";
        }
        f << "\n    }\n  }";
    }
    f << "\n]";
}

static std::set<std::string> loadSynced(const std::string& date)
{
    std::set<std::string> out;
    std::ifstream f(g_captureLogDir + "/" + date + ".synced.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) out.insert(line);
    }
    return out;
}

static void saveSynced(const std::string& date, const std::set<std::string>& files)
{
    try { fs::create_directories(g_captureLogDir); } catch (...) {}
    std::ofstream f(g_captureLogDir + "/" + date + ".synced.txt", std::ios::trunc);
    for (const auto& x : files) f << x << "\n";
}

// Called from /api/start with the clip snapshot taken just before recording.
static void appendCapture(const std::vector<std::pair<std::string, std::string>>& clips)
{
    if (clips.empty()) return;
    std::string date = localDateString();
    auto entries = loadCaptures(date);
    if (!entries.empty() && entries.back().clips == clips) return;  // dedupe
    CaptureEntry e;
    e.time = localTimeIso();
    e.clips = clips;
    entries.push_back(e);
    saveCaptures(date, entries);
}

// A clip counts as downloaded if it is in the download dir (staging inbox) or
// already sorted into the per-date folder next to it.
static bool clipDownloaded(const std::string& fileName)
{
    try {
        if (fs::exists(fs::path(g_downloadPath) / fileName)) return true;
        if (fileName.size() >= 14) {
            std::string d = fileName.substr(1, 8);
            if (d.find_first_not_of("0123456789") == std::string::npos) {
                std::string dateDir = d.substr(0, 4) + "-" + d.substr(4, 2)
                                      + "-" + d.substr(6, 2);
                return fs::exists(fs::path(g_downloadPath).parent_path()
                                  / dateDir / "raw" / fileName);
            }
        }
    } catch (...) {}
    return false;
}

static std::string buildCapturesJson(const std::string& date)
{
    auto entries = loadCaptures(date);
    auto synced = loadSynced(date);
    std::ostringstream js;
    js << "{\"date\":\"" << jsonEscape(date) << "\",\"captures\":[";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i) js << ",";
        js << "{\"time\":\"" << jsonEscape(entries[i].time) << "\",\"clips\":{";
        for (size_t j = 0; j < entries[i].clips.size(); j++) {
            if (j) js << ",";
            js << "\"" << jsonEscape(entries[i].clips[j].first) << "\":\""
               << jsonEscape(entries[i].clips[j].second) << "\"";
        }
        js << "},\"synced\":{";
        for (size_t j = 0; j < entries[i].clips.size(); j++) {
            if (j) js << ",";
            js << "\"" << jsonEscape(entries[i].clips[j].second) << "\":"
               << (synced.count(entries[i].clips[j].second) ? "true" : "false");
        }
        js << "},\"downloaded\":{";
        for (size_t j = 0; j < entries[i].clips.size(); j++) {
            if (j) js << ",";
            const std::string& fn = entries[i].clips[j].second;
            bool dl = synced.count(fn) || clipDownloaded(fn);
            js << "\"" << jsonEscape(fn) << "\":" << (dl ? "true" : "false");
        }
        js << "}}";
    }
    js << "]}";
    return js.str();
}

// ---------------------------------------------------------------------------
// Embedded HTML Frontend
// ---------------------------------------------------------------------------

static const char* HTML_PAGE = R"HTMLRAW(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>FX30 Multi-Camera Controller</title>
<style>
  :root {
    --bg: #1a1a2e; --surface: #16213e; --card: #0f3460;
    --accent: #e94560; --green: #4ecca3; --text: #eee;
    --dim: #888; --border: #2a2a4a;
  }
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
         background: var(--bg); color: var(--text); min-height: 100vh; }

  .header { background: var(--surface); padding: 16px 24px; display: flex;
            align-items: center; gap: 16px; border-bottom: 1px solid var(--border); }
  .header h1 { font-size: 20px; font-weight: 600; }
  .badge { background: var(--accent); color: white; border-radius: 12px;
           padding: 2px 10px; font-size: 13px; font-weight: 600; }
  .badge.ok { background: var(--green); }

  .controls { display: flex; gap: 10px; padding: 16px 24px; flex-wrap: wrap; }
  .btn { border: none; padding: 10px 20px; border-radius: 6px; font-size: 14px;
         font-weight: 600; cursor: pointer; transition: opacity 0.2s; }
  .btn:hover { opacity: 0.85; }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn-start { background: var(--green); color: #111; }
  .btn-stop { background: var(--accent); color: white; }
  .btn-scan { background: #5c6bc0; color: white; }
  .btn-reset { background: #ff9800; color: #111; }
  .btn-format { background: #ef5350; color: white; }
  .btn-dl { background: #7c4dff; color: white; }

  .heat-ok { color: var(--green); }
  .heat-pre { color: #ff9800; }
  .heat-over { color: var(--accent); font-weight: 700; animation: pulse 1s infinite; }

  .camera-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(340px, 1fr));
                 gap: 16px; padding: 0 24px 24px; }

  .cam-card { background: var(--card); border-radius: 10px; padding: 18px;
              border: 1px solid var(--border); }
  .cam-header { display: flex; justify-content: space-between; align-items: center;
                margin-bottom: 12px; }
  .cam-model { font-size: 15px; font-weight: 600; }
  .rec-dot { width: 12px; height: 12px; border-radius: 50%; display: inline-block; }
  .rec-dot.recording { background: var(--accent); animation: pulse 1s infinite; }
  .rec-dot.idle { background: var(--green); }
  .rec-dot.off { background: #555; }

  @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.3; } }

  .battery-bar { height: 8px; border-radius: 4px; background: #333; margin: 8px 0; position: relative; }
  .battery-fill { height: 100%; border-radius: 4px; transition: width 0.3s; }
  .battery-text { font-size: 12px; color: var(--dim); }

  .props-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 6px 16px; margin-top: 10px; }
  .prop { display: flex; justify-content: space-between; font-size: 13px; }
  .prop-label { color: var(--dim); }
  .prop-value { font-weight: 500; }

  .section-title { font-size: 12px; color: var(--dim); text-transform: uppercase;
                   letter-spacing: 1px; margin: 12px 0 6px; grid-column: 1 / -1; }

  .download-panel { background: var(--surface); border-radius: 10px; padding: 18px;
                    margin: 0 24px 24px; border: 1px solid var(--border); }
  .download-panel h3 { font-size: 15px; margin-bottom: 12px; }
  .dl-row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
  .dl-input { background: #1a1a2e; border: 1px solid var(--border); color: var(--text);
              padding: 8px 12px; border-radius: 6px; flex: 1; min-width: 200px; font-size: 14px; }
  .dl-status { font-size: 13px; color: var(--dim); margin-top: 10px; min-height: 20px; }

  .empty-state { text-align: center; padding: 60px 24px; color: var(--dim); }
  .empty-state h2 { font-size: 18px; margin-bottom: 8px; color: var(--text); }

  .scan-banner { background: var(--surface); border-bottom: 1px solid var(--border);
                 padding: 12px 24px; display: none; align-items: center; gap: 12px; }
  .scan-banner.active { display: flex; }
  .spinner { width: 18px; height: 18px; border: 3px solid var(--border);
             border-top-color: #5c6bc0; border-radius: 50%; animation: spin 0.8s linear infinite; flex-shrink: 0; }
  @keyframes spin { to { transform: rotate(360deg); } }
  .scan-text { font-size: 13px; color: var(--dim); }
  .scan-bar-track { flex: 1; max-width: 200px; height: 4px; background: var(--border); border-radius: 2px; overflow: hidden; }
  .scan-bar-fill { height: 100%; background: #5c6bc0; border-radius: 2px;
                   animation: indeterminate 1.5s ease-in-out infinite; width: 40%; }
  @keyframes indeterminate { 0% { transform: translateX(-100%); } 100% { transform: translateX(350%); } }
</style>
</head>
<body>
<div class="header">
  <h1>FX30 Controller</h1>
  <span class="badge" id="camCount">0 cameras</span>
  <span class="badge" id="connStatus">loading...</span>
</div>

<div class="controls">
  <button class="btn btn-start" onclick="apiPost('/api/start')" id="btnStart">Start Recording</button>
  <button class="btn btn-stop" onclick="apiPost('/api/stop')" id="btnStop">Stop Recording</button>
  <button class="btn btn-scan" onclick="apiPost('/api/scan')" id="btnScan">Scan</button>
  <button class="btn btn-reset" onclick="apiPost('/api/reset')" id="btnReset">USB Reset</button>
  <button class="btn btn-format" onclick="confirmFormat()" id="btnFormat">Format All (Slot 1)</button>
</div>

<div class="scan-banner" id="scanBanner">
  <div class="spinner"></div>
  <span class="scan-text" id="scanText">Scanning...</span>
  <div class="scan-bar-track"><div class="scan-bar-fill"></div></div>
</div>

<div id="cameraGrid" class="camera-grid"></div>

<div class="download-panel">
  <h3>Settings Preset</h3>
  <div class="dl-row">
    <button class="btn btn-scan" onclick="apiPost('/api/preset/save')" id="btnPresetSave">Save Current Settings</button>
    <button class="btn btn-dl" onclick="apiPost('/api/preset/apply')" id="btnPresetApply">Apply Preset</button>
  </div>
  <div class="dl-status" id="presetStatus"></div>
</div>

<div class="download-panel">
  <h3>File Download</h3>
  <div class="dl-row">
    <input type="text" class="dl-input" id="dlPath" placeholder="/tmp/fx30_downloads">
    <button class="btn btn-scan" onclick="setPath()">Set Path</button>
    <button class="btn btn-dl" onclick="startDownload()" id="btnDownload">Download All Files</button>
  </div>
  <div class="dl-status" id="dlStatus"></div>
</div>

<script>
let pollInterval = 2000;
let pollTimer = null;

function apiPost(url) {
  fetch(url, {method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.error) console.error(d.error);
    refresh();
  }).catch(console.error);
}

function confirmFormat() {
  if (confirm('Quick format Slot 1 on ALL cameras? This will erase all data on Slot 1.')) {
    apiPost('/api/format');
  }
}



function setPath() {
  const path = document.getElementById('dlPath').value;
  fetch('/api/set-download-path', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({path})
  }).then(r=>r.json()).then(()=>refresh()).catch(console.error);
}

function startDownload() {
  const path = document.getElementById('dlPath').value;
  fetch('/api/download', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({path})
  }).then(r=>r.json()).then(()=>refresh()).catch(console.error);
}

function heatBadge(state) {
  if (state === 2) return ' &mdash; <span class="heat-over">OVERHEATING</span>';
  if (state === 1) return ' &mdash; <span class="heat-pre">PRE-OVERHEAT</span>';
  return '';
}

function batteryColor(pct) {
  if (pct < 0) return '#555';
  if (pct <= 15) return '#e94560';
  if (pct <= 40) return '#ff9800';
  return '#4ecca3';
}

function renderCameras(data) {
  const grid = document.getElementById('cameraGrid');
  const cams = data.cameras;

  const busy = data.downloading || data.scanning;

  // Update header
  document.getElementById('camCount').textContent = cams.length + ' camera' + (cams.length !== 1 ? 's' : '');
  const cs = document.getElementById('connStatus');
  const allConn = cams.length > 0 && cams.every(c=>c.connected);
  if (data.scanning) { cs.textContent = 'scanning'; cs.className = 'badge'; }
  else if (data.downloading) { cs.textContent = 'downloading'; cs.className = 'badge'; }
  else if (cams.length === 0) { cs.textContent = 'no cameras'; cs.className = 'badge'; }
  else { cs.textContent = allConn ? 'connected' : 'partial'; cs.className = 'badge' + (allConn ? ' ok' : ''); }

  // Update scan banner
  const sb = document.getElementById('scanBanner');
  sb.className = 'scan-banner' + (data.scanning ? ' active' : '');
  document.getElementById('scanText').textContent = data.scanStatus || 'Scanning...';

  // Update download panel
  document.getElementById('dlPath').value = data.downloadPath;
  document.getElementById('dlStatus').textContent = data.downloadStatus || '';
  document.getElementById('btnDownload').disabled = busy;
  document.getElementById('btnStart').disabled = busy;
  document.getElementById('btnStop').disabled = busy;
  document.getElementById('btnScan').disabled = busy;
  document.getElementById('btnReset').disabled = busy;
  document.getElementById('btnFormat').disabled = busy;
  document.getElementById('presetStatus').textContent = data.hasPreset ? 'Preset: ' + data.presetPath : 'No preset saved';

  // Adjust poll rate when busy
  pollInterval = busy ? 1000 : 2000;

  if (cams.length === 0) {
    grid.innerHTML = '<div class="empty-state"><h2>No cameras connected</h2><p>Click Scan or USB Reset to discover FX30 cameras</p></div>';
    return;
  }

  grid.innerHTML = cams.map((c, i) => {
    const recClass = !c.connected ? 'off' : (c.recording ? 'recording' : 'idle');
    const recText = !c.connected ? 'Disconnected' : (c.recording ? 'RECORDING' : 'Idle');
    const bPct = c.battery >= 0 ? c.battery : 0;
    const bColor = batteryColor(c.battery);
    const bText = c.battery >= 0 ? c.battery + '%' : 'N/A';

    return '<div class="cam-card">' +
      '<div class="cam-header">' +
        '<span class="cam-model">' + esc(c.model) + '</span>' +
        '<span><span class="rec-dot ' + recClass + '"></span> ' + recText + '</span>' +
      '</div>' +
      '<div class="battery-text">Battery: ' + bText + heatBadge(c.heatState) + '</div>' +
      '<div class="battery-bar"><div class="battery-fill" style="width:' + bPct + '%;background:' + bColor + '"></div></div>' +
      '<div class="props-grid">' +
        '<div class="section-title">Exposure</div>' +
        prop('ISO', c.iso) + prop('Shutter', c.shutterSpeed) +
        prop('F-Stop', c.fNumber) + prop('WB', c.whiteBalance) +
        prop('Color Temp', c.colorTemp > 0 ? c.colorTemp + 'K' : '---') +
        '<div class="section-title">Media</div>' +
        prop('Slot 1', c.mediaSlot1Min >= 0 ? c.mediaSlot1Min + ' min' : 'N/A') +
        prop('Slot 2', c.mediaSlot2Min >= 0 ? c.mediaSlot2Min + ' min' : 'N/A') +
        '<div class="section-title">Recording</div>' +
        prop('Clip Name', c.clipName || '---') +
        prop('Codec', c.movieFormat) + prop('Rec Setting', c.recSetting) +
        prop('Frame Rate', c.frameRate) +
      '</div></div>';
  }).join('');
}

function prop(label, value) {
  return '<div class="prop"><span class="prop-label">' + label + '</span><span class="prop-value">' + esc(String(value)) + '</span></div>';
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function refresh() {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 3000);
  fetch('/api/status', {signal: ctrl.signal}).then(r=>r.json()).then(data=>{
    clearTimeout(timer);
    renderCameras(data);
  }).catch(e=>{
    clearTimeout(timer);
    // Don't overwrite status if we already have data — just skip this poll
    if (document.getElementById('connStatus').textContent === 'loading...') {
      document.getElementById('connStatus').textContent = 'connecting...';
    }
  });
}

function poll() {
  refresh();
  pollTimer = setTimeout(poll, pollInterval);
}

poll();
</script>
</body>
</html>)HTMLRAW";

// ---------------------------------------------------------------------------
// JSON Builders
// ---------------------------------------------------------------------------

static std::string buildCameraJson(size_t index, CameraDevice& cam)
{
    std::string modelStr(cam.m_modelId.begin(), cam.m_modelId.end());
    CameraProperties props;
    if (cam.m_connected) {
        props = cam.getProperties();
    }

    std::ostringstream js;
    js << "{";
    js << "\"index\":" << index << ",";
    js << "\"model\":\"" << jsonEscape(modelStr) << "\",";
    js << "\"connected\":" << (cam.m_connected ? "true" : "false") << ",";
    js << "\"recording\":" << (props.recording ? "true" : "false") << ",";
    js << "\"battery\":" << props.battery << ",";
    js << "\"iso\":\"" << jsonEscape(props.iso) << "\",";
    js << "\"shutterSpeed\":\"" << jsonEscape(props.shutterSpeed) << "\",";
    js << "\"fNumber\":\"" << jsonEscape(props.fNumber) << "\",";
    js << "\"whiteBalance\":\"" << jsonEscape(props.whiteBalance) << "\",";
    js << "\"colorTemp\":" << props.colorTemp << ",";
    js << "\"mediaSlot1Min\":" << props.mediaSlot1Min << ",";
    js << "\"mediaSlot2Min\":" << props.mediaSlot2Min << ",";
    js << "\"movieFormat\":\"" << jsonEscape(props.movieFormat) << "\",";
    js << "\"recSetting\":\"" << jsonEscape(props.recSetting) << "\",";
    js << "\"frameRate\":\"" << jsonEscape(props.frameRate) << "\",";
    js << "\"clipName\":\"" << jsonEscape(props.clipName) << "\",";
    js << "\"heatState\":" << props.heatState;
    js << "}";
    return js.str();
}

static std::string buildStatusJson()
{
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);

    std::ostringstream js;
    js << "{\"cameras\":[";
    if (lock.owns_lock()) {
        for (size_t i = 0; i < g_cameras.size(); i++) {
            if (i > 0) js << ",";
            js << buildCameraJson(i, *g_cameras[i]);
        }
    }
    js << "],";
    js << "\"downloading\":" << (g_downloading.load() ? "true" : "false") << ",";
    js << "\"downloadStatus\":\"" << jsonEscape(g_downloadStatus) << "\",";
    js << "\"downloadPath\":\"" << jsonEscape(g_downloadPath) << "\",";
    js << "\"scanning\":" << (g_scanning.load() ? "true" : "false") << ",";
    js << "\"scanStatus\":\"" << jsonEscape(g_scanStatus) << "\",";
    js << "\"listing\":" << (g_listing.load() ? "true" : "false") << ",";
    js << "\"listStatus\":\"" << jsonEscape(g_listStatus) << "\",";
    js << "\"presetPath\":\"" << jsonEscape(g_presetPath) << "\",";
    js << "\"hasPreset\":" << (fs::exists(g_presetPath) ? "true" : "false");
    js << "}";
    return js.str();
}

// ---------------------------------------------------------------------------
// Simple JSON body parser (extract string value for a key)
// ---------------------------------------------------------------------------

static std::string jsonGetString(const std::string& body, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = body.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = body.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return body.substr(pos + 1, end - pos - 1);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Parse CLI args
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--download-path" && i + 1 < argc) {
            g_downloadPath = argv[++i];
        } else if (arg == "--preset" && i + 1 < argc) {
            g_presetPath = argv[++i];
        } else if (arg == "--capture-log-dir" && i + 1 < argc) {
            g_captureLogDir = argv[++i];
        }
    }

    // Disable stdout buffering
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << "=== FX30 Multi-Camera Web Controller ===\n\n";

    if (!SCRSDK::Init()) {
        std::cerr << "Failed to initialize Sony Camera Remote SDK.\n";
        return 1;
    }

    // Start camera management thread (handles initial scan + auto-rescan)
    std::thread mgmtThread(cameraManagementThread);

    // Create HTTP server (starts immediately, doesn't wait for camera scan)
    httplib::Server svr;

    // CORS: the web app is served from a different origin (e.g.
    // https://signcollect.nl) so cross-origin requests to this local API must
    // be allowed. Add the headers to every response, and answer the browser's
    // OPTIONS preflight before it sends the real request.
    svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // Serve embedded HTML frontend
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML_PAGE, "text/html");
    });

    // GET /api/status
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildStatusJson(), "application/json");
    });

    // POST /api/start
    svr.Post("/api/start", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_listing) {
            res.set_content(g_downloading ? "{\"error\":\"Download in progress\"}"
                                          : "{\"error\":\"Listing in progress\"}", "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        int ok = 0, fail = 0;
        std::vector<std::pair<std::string, std::string>> clips;
        for (auto& cam : g_cameras) {
            // Snapshot the clip name BEFORE starting: it names the clip that
            // this recording will create.
            std::string clipName;
            if (cam->m_connected) clipName = cam->getProperties().clipName;
            if (cam->startRecording()) {
                ok++;
                if (!clipName.empty()) {
                    std::string modelStr(cam->m_modelId.begin(), cam->m_modelId.end());
                    std::string serial = modelStr;
                    size_t o = modelStr.find('('), c = modelStr.find(')');
                    if (o != std::string::npos && c != std::string::npos && c > o)
                        serial = modelStr.substr(o + 1, c - o - 1);
                    clips.push_back({serial, clipName + ".MP4"});
                }
            } else {
                fail++;
            }
        }
        // All-or-nothing: a multi-camera capture with a missing angle is
        // useless. If any camera failed to start, stop the ones that did and
        // report an aborted capture instead of logging it.
        if (fail > 0 && ok > 0) {
            for (auto& cam : g_cameras) {
                if (cam->m_connected) cam->stopRecording();
            }
            res.set_content("{\"ok\":0,\"failed\":" + std::to_string(ok + fail) +
                            ",\"aborted\":true,\"error\":\"" + std::to_string(fail) +
                            " camera(s) failed to start; all cameras stopped\"}",
                            "application/json");
            return;
        }
        if (ok > 0) {
            appendCapture(clips);
            logEvent("CAPTURE started on " + std::to_string(ok) + " camera(s)");
        }
        res.set_content("{\"ok\":" + std::to_string(ok) + ",\"failed\":" + std::to_string(fail) + "}", "application/json");
    });

    // GET /api/captures?date=YYYY-MM-DD - captures triggered that day (default
    // today) with per-file synced status
    svr.Get("/api/captures", [](const httplib::Request& req, httplib::Response& res) {
        std::string date = req.get_param_value("date");
        if (date.empty()) date = localDateString();
        std::lock_guard<std::mutex> lock(g_mutex);
        res.set_content(buildCapturesJson(date), "application/json");
    });

    // POST /api/captures/synced {"date":"YYYY-MM-DD","files":["X.MP4",...]}
    // - reported by the sync client after verifying files on the drive
    svr.Post("/api/captures/synced", [](const httplib::Request& req, httplib::Response& res) {
        std::string date = jsonGetString(req.body, "date");
        if (date.empty()) date = localDateString();
        std::set<std::string> files;
        size_t pos = req.body.find("\"files\"");
        if (pos != std::string::npos) {
            size_t open = req.body.find('[', pos);
            size_t close = req.body.find(']', open);
            if (open != std::string::npos && close != std::string::npos) {
                size_t q = open + 1;
                while (q < close) {
                    std::string fname = extractJsonString(req.body, q);
                    if (fname.empty() || q > close) break;
                    files.insert(fname);
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        // merge with already-known synced files (a partial report must not
        // un-sync files reported earlier)
        auto existing = loadSynced(date);
        files.insert(existing.begin(), existing.end());
        saveSynced(date, files);
        res.set_content("{\"date\":\"" + jsonEscape(date) + "\",\"synced\":" +
                        std::to_string(files.size()) + "}", "application/json");
    });

    // POST /api/stop
    svr.Post("/api/stop", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_listing) {
            res.set_content(g_downloading ? "{\"error\":\"Download in progress\"}"
                                          : "{\"error\":\"Listing in progress\"}", "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        int ok = 0, fail = 0;
        for (auto& cam : g_cameras) {
            if (cam->stopRecording()) ok++; else fail++;
        }
        res.set_content("{\"ok\":" + std::to_string(ok) + ",\"failed\":" + std::to_string(fail) + "}", "application/json");
    });

    // POST /api/scan
    svr.Post("/api/scan", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_listing) {
            res.set_content(g_downloading ? "{\"error\":\"Download in progress\"}"
                                          : "{\"error\":\"Listing in progress\"}", "application/json");
            return;
        }
        if (g_scanning) {
            res.set_content("{\"error\":\"Scan already in progress\"}", "application/json");
            return;
        }
        std::thread([]() {
            std::lock_guard<std::mutex> lock(g_mutex);
            scanAndConnect();
        }).detach();
        res.set_content("{\"status\":\"scan started\"}", "application/json");
    });

    // POST /api/reset
    svr.Post("/api/reset", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_listing) {
            res.set_content(g_downloading ? "{\"error\":\"Download in progress\"}"
                                          : "{\"error\":\"Listing in progress\"}", "application/json");
            return;
        }
        if (g_scanning) {
            res.set_content("{\"error\":\"Scan already in progress\"}", "application/json");
            return;
        }
        std::thread([]() {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& cam : g_cameras) cam->disconnect();
            g_cameras.clear();
            scanAndConnect(true);
        }).detach();
        res.set_content("{\"status\":\"reset started\"}", "application/json");
    });

    // POST /api/format - quick format slot 1 on all cameras
    svr.Post("/api/format", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_scanning || g_listing) {
            res.set_content("{\"error\":\"Busy\"}", "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        int ok = 0, fail = 0;
        for (auto& cam : g_cameras) {
            if (cam->formatSlot1()) ok++; else fail++;
        }
        res.set_content("{\"ok\":" + std::to_string(ok) + ",\"failed\":" + std::to_string(fail) + "}", "application/json");
    });

    // POST /api/preset/save - save current camera settings to preset file
    svr.Post("/api/preset/save", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_cameras.empty()) {
            res.set_content("{\"error\":\"No cameras connected\"}", "application/json");
            return;
        }
        // Save from first connected camera
        for (auto& cam : g_cameras) {
            if (cam->m_connected) {
                if (savePreset(*cam, g_presetPath)) {
                    res.set_content("{\"status\":\"Preset saved to " + jsonEscape(g_presetPath) + "\"}", "application/json");
                } else {
                    res.set_content("{\"error\":\"Failed to save preset\"}", "application/json");
                }
                return;
            }
        }
        res.set_content("{\"error\":\"No connected camera\"}", "application/json");
    });

    // POST /api/preset/apply - apply preset to all cameras now
    svr.Post("/api/preset/apply", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto preset = loadPreset(g_presetPath);
        if (preset.empty()) {
            res.set_content("{\"error\":\"No preset file found at " + jsonEscape(g_presetPath) + "\"}", "application/json");
            return;
        }
        int total = 0;
        for (auto& cam : g_cameras) {
            if (cam->m_connected) {
                total += applyPreset(*cam, preset);
            }
        }
        res.set_content("{\"applied\":" + std::to_string(total) + "}", "application/json");
    });

    // GET /api/preset - get current preset contents
    svr.Get("/api/preset", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(g_presetPath);
        if (!f.is_open()) {
            res.set_content("{\"preset\":null,\"path\":\"" + jsonEscape(g_presetPath) + "\"}", "application/json");
            return;
        }
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        res.set_content("{\"preset\":" + content + ",\"path\":\"" + jsonEscape(g_presetPath) + "\"}", "application/json");
    });

    // GET /api/files - list files on all cameras (mode-switch)
    // POST /api/list-files - enumerate files on cameras without downloading
    svr.Post("/api/list-files", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_scanning || g_listing) {
            res.set_content("{\"error\":\"Busy\"}", "application/json");
            return;
        }
        std::thread(listFilesThread).detach();
        res.set_content("{\"status\":\"listing started\"}", "application/json");
    });

    // GET /api/files - result of the last /api/list-files run
    svr.Get("/api/files", [](const httplib::Request&, httplib::Response& res) {
        if (g_downloading || g_listing) {
            res.set_content(g_downloading ? "{\"error\":\"Download in progress\"}"
                                          : "{\"error\":\"Listing in progress\"}", "application/json");
            return;
        }
        if (g_listing) {
            res.set_content("{\"error\":\"Listing in progress\"}", "application/json");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            res.set_content("{\"files\":" + g_filesJson + "}", "application/json");
        }
    });

    // POST /api/download
    svr.Post("/api/download", [](const httplib::Request& req, httplib::Response& res) {
        if (g_downloading) {
            res.set_content("{\"error\":\"Download already in progress\"}", "application/json");
            return;
        }
        // Parse optional path from body
        if (!req.body.empty()) {
            std::string path = jsonGetString(req.body, "path");
            if (!path.empty()) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_downloadPath = path;
            }
        }
        std::thread(downloadFilesThread).detach();
        res.set_content("{\"status\":\"download started\"}", "application/json");
    });

    // POST /api/set-download-path
    svr.Post("/api/set-download-path", [](const httplib::Request& req, httplib::Response& res) {
        std::string path = jsonGetString(req.body, "path");
        if (path.empty()) {
            res.set_content("{\"error\":\"Missing path\"}", "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_downloadPath = path;
        res.set_content("{\"downloadPath\":\"" + jsonEscape(g_downloadPath) + "\"}", "application/json");
    });

    std::cout << "Server running at http://localhost:" << port << "\n";
    std::cout << "Download path: " << g_downloadPath << "\n";

    svr.listen("0.0.0.0", port);

    // Shutdown
    g_running = false;
    if (mgmtThread.joinable()) mgmtThread.join();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& cam : g_cameras) cam->disconnect();
        g_cameras.clear();
    }
    SCRSDK::Release();

    std::cout << "Server stopped.\n";
    return 0;
}
