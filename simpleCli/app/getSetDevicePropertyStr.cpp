// get/set deviceProperty, send command sample (support set str)
#include <chrono>
#include <cinttypes>
#include <codecvt>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(__APPLE__)
  #if defined(USE_EXPERIMENTAL_FS) // for jetson
    #include <experimental/filesystem>
    namespace fs = std::experimental::filesystem;
  #else
    #include <filesystem>
    namespace fs = std::filesystem;
  #endif
#endif

#if defined(__APPLE__) || defined(__linux__)
  #include <unistd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
    using CrString = std::wstring;
#else
    using CrString = std::string;
#endif

// macro for multibyte character
#if defined(_WIN32) || defined(_WIN64)
  #define CRSTR(s) L ## s
  #define CrCout std::wcout
  #define Utf8ToCr(a) wstring_convert.from_bytes(a)
  static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> wstring_convert;
#else
  #define CRSTR(s) s
  #define CrCout std::cout
  #define Utf8ToCr(a) (a)
  static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> wstring_convert;
#endif


#include "CrDeviceProperty.h"
#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "CrDebugString.h"   // use CrDebugString.cpp

#define PrintError(msg, err) { fprintf(stderr, "Error in %s(%d):" msg ",%s\n", __FUNCTION__, __LINE__, (err ? CrErrorString(err).c_str():"")); }
#define GotoError(msg, err) { PrintError(msg, err); goto Error; }

bool  m_connected = false;
CrString m_modelId;
int64_t  m_device_handle = 0;
bool  m_lvProtocolChanged = false;

std::mutex m_eventPromiseMutex;
uint32_t m_setDPCode = 0;
std::promise<void>* m_eventPromise = nullptr;
void setEventPromise(std::promise<void>* dp)
{
    std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
    m_eventPromise = dp;
}

SCRSDK::CrError _getDeviceProperty(int64_t device_handle, uint32_t code, SCRSDK::CrDeviceProperty* devProp)
{
    std::int32_t nprop = 0;
    SCRSDK::CrDeviceProperty* prop_list = nullptr;
    SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(device_handle, 1, &code, &prop_list, &nprop);
    if(err) GotoError("", err);
    if(prop_list && nprop >= 1) {
        *devProp = prop_list[0];
    }
Error:
    if(prop_list) SCRSDK::ReleaseDeviceProperties(device_handle, prop_list);
    return err;
}

SCRSDK::CrError _setDeviceProperty(int64_t device_handle, uint32_t code, uint64_t data, bool blocking=true)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = 0;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();
    std::future_status status;

    SCRSDK::CrDeviceProperty devProp;

    err = _getDeviceProperty(device_handle, code, &devProp);
    if(err) GotoError("", err);
    if (devProp.GetValueType() == SCRSDK::CrDataType_STR) GotoError("STR is not supported", 0);
    if (blocking && devProp.GetCurrentValue() == data) {
        std::cout << "skipped\n";
        return 0;
    }

    if(blocking) {
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        m_setDPCode = code;
        m_eventPromise = &eventPromise;
    }

    devProp.SetCurrentValue(data);
    err = SCRSDK::SetDeviceProperty(device_handle, &devProp);
    if(err) GotoError("", err);

    if(!blocking) return 0;

    status = eventFuture.wait_for(std::chrono::milliseconds(3000));
    if(status != std::future_status::ready) GotoError("timeout", 0);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);
    std::cout << "OK\n";

    result = 0;
Error:
    setEventPromise(nullptr);
    return result;
}

SCRSDK::CrError _setDevicePropertyStr(int64_t device_handle, uint32_t code, std::string str, bool blocking=true)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = 0;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();
    std::future_status status;

    SCRSDK::CrDeviceProperty devProp;

    err = _getDeviceProperty(device_handle, code, &devProp);
    if(err) GotoError("", err);
    if (devProp.GetValueType() != SCRSDK::CrDataType_STR) {
        int64_t _stoll(std::string inputLine);
        int64_t data = 0;
        try{ data = _stoll(str); } catch(const std::exception&) {GotoError("", 0);}
        return _setDeviceProperty(device_handle, code, data, blocking);
    }

    if(blocking) {
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        m_setDPCode = code;
        m_eventPromise = &eventPromise;
    }

    {
        #define MAX_CURRENT_STR 255
        size_t len = str.length() + 1;
        if(len == 1) len = 0;
        CrInt16u setStr[MAX_CURRENT_STR + 1] = {0};
        if (len > MAX_CURRENT_STR) GotoError("Character size error", 0);
        setStr[0] = static_cast<CrInt16u>(len);

        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
        std::u16string utf16Str = converter.from_bytes(str);
        memcpy(&setStr[1], utf16Str.c_str(), len * 2);
        if (code == SCRSDK::CrDeviceProperty_ImageID_String) {
            for (int i = 0; i < len; i++) {
                uint16_t data = setStr[1 + i];
                setStr[1 + i] = (data >> 8) | (data << 8);
            }
        }

        if(!memcmp(devProp.GetCurrentStr(), setStr, (1+len)*2)) {
            std::cout << "skipped\n";
            return 0;
        }

        devProp.SetCurrentStr((CrInt16u*)setStr);
    }
    err = SCRSDK::SetDeviceProperty(device_handle, &devProp);
    if(err) GotoError("", err);

    if(!blocking) return 0;

    status = eventFuture.wait_for(std::chrono::milliseconds(3000));
    if(status != std::future_status::ready) GotoError("timeout", 0);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);
    std::cout << "OK\n";

    result = 0;
Error:
    setEventPromise(nullptr);
    return result;
}

std::vector<int64_t> _getPossible(SCRSDK::CrDataType dataType, const uint8_t* values, uint32_t valueSize)
{
/*
    CrInt32u GetSetValueSize();
    CrInt8u* GetSetValues();
*/
    std::vector<int64_t> possible;

    int dataLen = 1;
    switch(dataType & 0x100F) {
    case SCRSDK::CrDataType_UInt8:  dataLen = sizeof(uint8_t); break;
    case SCRSDK::CrDataType_Int8:   dataLen = sizeof(int8_t); break;
    case SCRSDK::CrDataType_UInt16: dataLen = sizeof(uint16_t); break;
    case SCRSDK::CrDataType_Int16:  dataLen = sizeof(int16_t); break;
    case SCRSDK::CrDataType_UInt32: dataLen = sizeof(uint32_t); break;
    case SCRSDK::CrDataType_Int32:  dataLen = sizeof(int32_t); break;
    case SCRSDK::CrDataType_UInt64: dataLen = sizeof(uint64_t); break;
    default: return possible;
    }

    const uint8_t* buf = values;
    uint32_t nval = valueSize / dataLen;
    possible.resize(nval);
    for (uint32_t i = 0; i < nval; ++i) {
        int64_t data = 0;
        switch(dataType & 0x100F) {
        case SCRSDK::CrDataType_UInt8:  data = (reinterpret_cast<uint8_t const*>(buf))[i]; break;
        case SCRSDK::CrDataType_Int8:   data = (reinterpret_cast<int8_t const*>(buf))[i]; break;
        case SCRSDK::CrDataType_UInt16: data = (reinterpret_cast<uint16_t const*>(buf))[i]; break;
        case SCRSDK::CrDataType_Int16:  data = (reinterpret_cast<int16_t const*>(buf))[i]; break;
        case SCRSDK::CrDataType_UInt32: data = (reinterpret_cast<uint32_t const*>(buf))[i]; break;
        case SCRSDK::CrDataType_Int32:  data = (reinterpret_cast<int32_t const*>(buf))[i]; break;
        case SCRSDK::CrDataType_UInt64: data = (reinterpret_cast<uint64_t const*>(buf))[i]; break;
        default: break;
        }
        possible.at(i) = data;
    }
    return possible;
}

CrString _getCurrentStr(SCRSDK::CrDeviceProperty* devProp)
{
    CrString current;
    uint16_t* dp = devProp->GetCurrentStr();
    if(!dp) GotoError("", 0);
    if(*dp > 0) {
    if(devProp->GetCode() == SCRSDK::CrDeviceProperty_ImageID_String) {
        for(int i = 0; i < *dp; i++) {
            uint16_t data = dp[1+i];
            dp[1+i] = (data>>8) | (data<<8);
        }
    }
    #if defined (_WIN32) || defined(_WIN64)
        current = CrString((wchar_t*)(dp+1));
    #else
        int length = dp[0];
        current = wstring_convert.to_bytes(std::u16string((dp+1), (dp+1)+length));
    #endif
    }
Error:
    return current;
}

class DeviceCallback : public SCRSDK::IDeviceCallback
{
public:
    DeviceCallback() {};
    ~DeviceCallback() {};

    void OnConnected(SCRSDK::DeviceConnectionVersioin version)
    {
        CrCout << "Connected to " << m_modelId << "\n";
        m_connected = true;
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if(m_eventPromise) {
            m_eventPromise->set_value();
            m_eventPromise = nullptr;
        }
    }

    void OnError(CrInt32u error)
    {
        printf("Connection error:%s\n", CrErrorString(error).c_str());
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if(m_eventPromise) {
            m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
            m_eventPromise = nullptr;
        }
    }

    void OnDisconnected(CrInt32u error)
    {
        CrCout << "Disconnected from " << m_modelId << "\n";
        m_connected = false;
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if(m_eventPromise) {
            m_eventPromise->set_value();
            m_eventPromise = nullptr;
        }
    }

    void OnCompleteDownload(CrChar* filename, CrInt32u type )
    {
        CrCout << "OnCompleteDownload:" << filename << "\n";
    }

    void OnNotifyContentsTransfer(CrInt32u notify, SCRSDK::CrContentHandle contentHandle, CrChar* filename)
    {
        std::cout << "OnNotifyContentsTransfer.\n";
    }

    void OnWarning(CrInt32u warning)
    {
        if (warning == SCRSDK::CrWarning_Connect_Reconnecting) {
            CrCout << "Reconnecting to " << m_modelId << "\n";
            return;
        }
        if (warning == SCRSDK::CrNotify_LiveView_Alt_Ready) {
            m_lvProtocolChanged = true;
        }
        std::cout << "OnWarning:" << CrErrorString(warning) << "\n";
    }

    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3)
    {
        std::cout << "OnWarningExt:" << CrWarningExtString(warning, param1, param2, param3).c_str() << "\n";
    }

    void OnLvPropertyChanged() {}
    void OnLvPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
    void OnPropertyChanged() {}
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes)
    {
        std::cout << "OnPropertyChangedCodes:\n";
        for(uint32_t i = 0; i < num; ++i) {
            std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
            if(m_setDPCode && m_setDPCode == codes[i]) {
                m_setDPCode = 0;
                if(m_eventPromise) {
                    m_eventPromise->set_value();
                    m_eventPromise = nullptr;
                }
            }
            std::string name = CrDevicePropertyString((SCRSDK::CrDevicePropertyCode)codes[i]);

            SCRSDK::CrDeviceProperty devProp;
            SCRSDK::CrError err = _getDeviceProperty(m_device_handle, codes[i], &devProp);
            if(err) break;
            if(devProp.GetValueType() == SCRSDK::CrDataType_STR) {
                CrCout << "  " << CrString(name.begin(), name.end()) << "=\"" << _getCurrentStr(&devProp) << "\"\n";
            } else {
                int64_t current = devProp.GetCurrentValue();
                if(current < 10) {
                    printf("  %s=%" PRId64 "\n", name.c_str(), current);
                } else {
                    printf("  %s=0x%" PRIx64 "(%" PRId64 ")\n", name.c_str(), current, current);
                }
            }
        }
    }
};

CrString _getModelId(const SCRSDK::ICrCameraObjectInfo* objInfo)
{
    CrString id;
    if (CrString(objInfo->GetConnectionTypeName()) == CRSTR("IP")) {
        id = CrString(objInfo->GetMACAddressChar());
    } else {
        id = CrString((CrChar*)objInfo->GetId());
    }
    return CrString(objInfo->GetModel()).append(CRSTR(" (")).append(id).append(CRSTR(")"));
}

SCRSDK::CrError _getIdPassword(SCRSDK::ICrCameraObjectInfo* objInfo, std::string& fingerprint, std::string& userId, std::string& userPassword)
{
    char fpBuff[128] = {0};
    CrInt32u fpLen = 0;
    SCRSDK::CrError err = SCRSDK::GetFingerprint(objInfo, fpBuff, &fpLen);
    if(err) GotoError("", err);
    fingerprint = std::string(fpBuff, fpLen);

    std::cout << "fingerprint: " << fingerprint.c_str() << "\n";
    std::cout << "id:";       std::getline(std::cin, userId);
    std::cout << "password:"; std::getline(std::cin, userPassword);
    return 0;
Error:
    return err;
}

std::vector<std::string> _split(std::string inputLine, char delimiter)
{
    std::vector<std::string> strArray;
    if (inputLine.empty()) return strArray;

    std::string tmp;
    std::stringstream ss{inputLine};
    while (getline(ss, tmp, delimiter)) {
        strArray.push_back(tmp);
    }
    return strArray;
}

int64_t _stoll(std::string inputLine)
{
    int64_t data = 0;
    if(inputLine.empty()) throw std::runtime_error("error");
    try {
        if (inputLine.compare(0, 2, "0x") == 0 || inputLine.compare(0, 2, "0X") == 0) {
            data = std::stoull(inputLine.substr(2), nullptr, 16);
        } else {
            data = std::stoll(inputLine, nullptr, 10);
        }
    } catch(const std::exception& ex) {
        throw ex;
    }
    return data;
}

int main(void)
{
    int result = -1;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    SCRSDK::ICrEnumCameraObjectInfo* enumCameraObjectInfo = nullptr;
    SCRSDK::ICrCameraObjectInfo* objInfo = nullptr;
    DeviceCallback deviceCallback;

  #if defined(__APPLE__)
    #define MAC_MAX_PATH 255
    char pathBuf[MAC_MAX_PATH] = {0};
    if(NULL == getcwd(pathBuf, sizeof(pathBuf) - 1)) return 1;
    CrString path = pathBuf;
  #else
    CrString path = fs::current_path().native();
  #endif

    bool boolRet = SCRSDK::Init();
    if(!boolRet) GotoError("", 0);

    // enumeration
    {
        uint32_t count = 0;
        uint32_t index = 1;

        err = SCRSDK::EnumCameraObjects(&enumCameraObjectInfo, 3/*timeInSec*/);
        if(err || !enumCameraObjectInfo) GotoError("no camera", err);

        count = enumCameraObjectInfo->GetCount();
        if(count >= 2) {
            for (uint32_t i = 0; i < count; ++i) {
                auto* info = enumCameraObjectInfo->GetCameraObjectInfo(i);
                CrCout << '[' << i + 1 << "] " << _getModelId(info) << "\n";
            }

            std::string inputLine;
            std::cout << "select camera:"; std::getline(std::cin, inputLine);
            try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
            if(index < 1 || index > count) GotoError("", 0);
        }
        objInfo = (SCRSDK::ICrCameraObjectInfo*)enumCameraObjectInfo->GetCameraObjectInfo(index - 1);
        m_modelId = _getModelId(objInfo);
    }

    // connect
    {
        std::string  fingerprint = "";
        std::string  userId = "";
        std::string  userPassword = "";
        std::promise<void> eventPromise;
        std::future<void> eventFuture = eventPromise.get_future();

        if (objInfo->GetSSHsupport() == SCRSDK::CrSSHsupport_ON) {
            err = _getIdPassword(objInfo, fingerprint, userId, userPassword); if(err) goto Error;
        }

        setEventPromise(&eventPromise);
        err = SCRSDK::Connect(objInfo, &deviceCallback, &m_device_handle,
            SCRSDK::CrSdkControlMode_Remote,
            SCRSDK::CrReconnecting_ON,
            userId.c_str(), userPassword.c_str(), fingerprint.c_str(), (uint32_t)fingerprint.size());
        if(err) GotoError("", err);

    //  std::future_status status = eventFuture.wait_for(std::chrono::milliseconds(3000));
    //  if(status != std::future_status::ready) GotoError("timeout",0);
        try{
            eventFuture.get();
        } catch(const std::exception&) GotoError("", 0);
    }

    // set work directory
    {
        CrCout << "path=" << path.data() << "\n";
        err = SCRSDK::SetSaveInfo(m_device_handle, const_cast<CrChar*>(path.data()), const_cast<CrChar*>(CRSTR("DSC")), -1/*startNo*/);
        if(err) GotoError("", err);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::cout << "usage:\n";
    std::cout << "   setdp <DP name> <param> [1-blocking,0-no blocking]\n";
    std::cout << "   getdp <DP name>\n";
    std::cout << "   infodp <DP name>\n";
    std::cout << "   listdp\n";
    std::cout << "   send <command name> <param>\n";
    std::cout << "   setcc  <control code> <param>\n";
    std::cout << "   infocc <control code>\n";
    std::cout << "   listcc\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::vector<std::string> args = _split(inputLine, ' ');

        if(args.size() == 0) {

        } else if(args[0] == "q" || args[0]== "Q") {
            break;
        } else if(args[0] == "send" && args.size() >= 3) {
            int64_t data = 0;
            int32_t code = CrCommandIdCode(args[1]);
            if(code < 0) continue;
            try{ data = _stoll(args[2]); } catch(const std::exception&) {continue;}

            err = SCRSDK::SendCommand(m_device_handle, code, (SCRSDK::CrCommandParam)data);
            if(err) GotoError("", err);

        } else if(args[0] == "setdp" && args.size() >= 2) {
            bool blocking = true;
            int32_t code = CrDevicePropertyCode(args[1]);
            if(code < 0) continue;
            std::string value = (args.size() >= 3) ? args[2]: "";

            if(args.size() >= 4) 
                try{ blocking = _stoll(args[3]); } catch(const std::exception&) {continue;}

            err = _setDevicePropertyStr(m_device_handle, code, value, blocking);
            if(err) continue;

        } else if((args[0] == "getdp" || args[0] == "infodp") && args.size() >= 2) {
        // device property get/info
            int32_t code = CrDevicePropertyCode(args[1]);
            if(code < 0) continue;

            SCRSDK::CrDeviceProperty devProp;
            err = _getDeviceProperty(m_device_handle, code, &devProp);
            if(err) continue;
            SCRSDK::CrDataType dataType = devProp.GetValueType();

            if(args[0] == "getdp") {
                if(dataType == SCRSDK::CrDataType_STR) {
                    CrCout << _getCurrentStr(&devProp) << "\n";
                } else {
                    printf("0x%" PRIx64 "(%" PRId64 ")\n", devProp.GetCurrentValue(), devProp.GetCurrentValue());  // macro for %lld
                }
            } else if(args[0] == "infodp") {
                printf("  get enable=%d\n", devProp.IsGetEnableCurrentValue());
                printf("  set enable=%d\n", devProp.IsSetEnableCurrentValue());
                printf("  variable  =%d\n", devProp.GetPropertyVariableFlag());
                printf("  enable    =%d\n", devProp.GetPropertyEnableFlag());
                printf("  valueType =0x%x\n", dataType);
                if(dataType == SCRSDK::CrDataType_STR) {
                    CrCout << "  current   =\"" << _getCurrentStr(&devProp) << "\"\n";
                } else {
                    printf("  current   =0x%" PRIx64 "(%" PRId64 ")\n", devProp.GetCurrentValue(), devProp.GetCurrentValue());

                    std::vector<int64_t> possible = _getPossible(devProp.GetValueType(), devProp.GetValues(), devProp.GetValueSize());
                    printf("  possible  =");
                    for(int i = 0; i < possible.size(); i++) {
                        printf("0x%" PRIx64 "(%" PRId64 "),", possible[i], possible[i]);
                    }
                    printf("\n");
                }
            }
        } else if(args[0] == "listdp") {
            std::int32_t nprop = 0;
            SCRSDK::CrDeviceProperty* prop_list = nullptr;
            SCRSDK::CrError err = SCRSDK::GetDeviceProperties(m_device_handle, &prop_list, &nprop);
            if(err) GotoError("", err);

            for(int i = 0; i < nprop; i++) {
                printf("%s\n", CrDevicePropertyString((SCRSDK::CrDevicePropertyCode)prop_list[i].GetCode()).c_str());
            }
            SCRSDK::ReleaseDeviceProperties(m_device_handle, prop_list);

        } else if(args[0] == "setcc" && args.size() >= 3) {
            int64_t data = 0;
            SCRSDK::CrControlCode code = CrControlCode(args[1]);
            if(code < 0) continue;

            SCRSDK::CrControlCodeInfo* info = NULL;
            err = SCRSDK::GetSelectControlCode(m_device_handle, code, &info);
            if(err) GotoError("", err);

            SCRSDK::CrDataType dataType = info->GetValueType();
            SCRSDK::ReleaseControlCodes(m_device_handle, info);

            if(dataType != SCRSDK::CrDataType_STR) {
                try{ data = _stoll(args[2]); } catch(const std::exception&) {continue;}
                err = SCRSDK::ExecuteControlCodeValue(m_device_handle, code, data);
                if(err) GotoError("", err);
            } else {
                CrString str = Utf8ToCr(args[2]);
                size_t len = str.length() + 1;
                if (len == 1) GotoError("Character size error", 0);
                if (len > (size_t)info->GetValueSize()) GotoError("Character size error", 0);
                std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
                std::u16string utf16Str = converter.from_bytes(args[2]);
                err = SCRSDK::ExecuteControlCodeString(m_device_handle, code, (CrInt16u)utf16Str.length() + 1, (CrInt16u*)utf16Str.c_str());
                if(err) GotoError("", err);
            }

        } else if(args[0] == "infocc" && args.size() >= 2) {
            int64_t data = 0;
            SCRSDK::CrControlCode code = CrControlCode(args[1]);
            if(code < 0) continue;

            SCRSDK::CrControlCodeInfo* info = NULL;
            err = SCRSDK::GetSelectControlCode(m_device_handle, code, &info);
            if(err) GotoError("", err);

            SCRSDK::CrDataType dataType = info->GetValueType();
            printf("  code      =0x%x\n", code);
            printf("  valueType =0x%x\n", dataType);
            if(dataType != SCRSDK::CrDataType_STR) {
                std::vector<int64_t> possible = _getPossible(dataType, info->GetValues(), info->GetValueSize());
                printf("  possible  =");
                for(int i = 0; i < possible.size(); i++) {
                    printf("0x%" PRIx64 "(%" PRId64 "),", possible[i], possible[i]);
                }
                printf("\n");
            }
            SCRSDK::ReleaseControlCodes(m_device_handle, info);

        } else if(args[0] == "listcc") {
            SCRSDK::CrControlCodeInfo* infos = NULL;
            CrInt32u numOfInfos = 0;
            err = GetSupportedControlCodes(m_device_handle, &infos, &numOfInfos);
            if(err) GotoError("", err);

            for(int i = 0; i < (int)numOfInfos; i++) {
                printf("%s\n", CrControlCodeString(infos[i].GetCode()).c_str());
            }
            SCRSDK::ReleaseControlCodes(m_device_handle, infos);

        } else {
            std::cout << "unknown command\n";
        }
    }

    result = 0;
Error:
    if(enumCameraObjectInfo) enumCameraObjectInfo->Release();

    if(m_connected) {
        std::promise<void> eventPromise;
        std::future<void> eventFuture = eventPromise.get_future();
        setEventPromise(&eventPromise);
        SCRSDK::Disconnect(m_device_handle);
        int msec = 3000; // 3 seconds
        if (m_lvProtocolChanged) msec *= 10; // x10
        // If CrLiveViewProtocol_Alt is selected, the disconnection process may take some time.
        eventFuture.wait_for(std::chrono::milliseconds(msec));
    }
    if (m_device_handle) SCRSDK::ReleaseDevice(m_device_handle);
    SCRSDK::Release();

    return result;
}
