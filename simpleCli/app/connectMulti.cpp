// connect to multi-camera sample
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
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

// macro for multibyte character
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
#include "CrDebugString.h"   // use CrDebugString.cpp

#define PrintError(msg, err) { fprintf(stderr, "Error in %s(%d):" msg ",%s\n", __FUNCTION__, __LINE__, (err ? CrErrorString(err).c_str():"")); }
#define GotoError(msg, err) { PrintError(msg, err); goto Error; }

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

class CameraDevice : public SCRSDK::IDeviceCallback
{
public:
    int64_t  m_device_handle = 0;
    bool  m_connected = false;
    CrString m_modelId;

    std::mutex m_eventPromiseMutex;
    uint32_t m_setDPCode = 0;
    std::promise<void>* m_eventPromise = nullptr;
    void setEventPromise(std::promise<void>* dp)
    {
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        m_eventPromise = dp;
    }

    CameraDevice() {};
    ~CameraDevice() {};

    // override of IDeviceCallback

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
        std::cout << "OnWarning:" << CrErrorString(warning) << "\n";
    }

    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3) {}
    void OnLvPropertyChanged() {}
    void OnLvPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
    void OnPropertyChanged() {}
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes)
    {
        //std::cout << "OnPropertyChangedCodes:\n";
        for(uint32_t i = 0; i < num; ++i) {
            std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
            if(m_setDPCode && m_setDPCode == codes[i]) {
                m_setDPCode = 0;
                if(m_eventPromise) {
                    m_eventPromise->set_value();
                    m_eventPromise = nullptr;
                }
            }
        }
    }

    // method

    SCRSDK::CrError _getIdPassword(const SCRSDK::ICrCameraObjectInfo* objInfo, std::string& fingerprint, std::string& userId, std::string& userPassword)
    {
        char fpBuff[128] = {0};
        CrInt32u fpLen = 0;
        SCRSDK::CrError err = SCRSDK::GetFingerprint((SCRSDK::ICrCameraObjectInfo*)objInfo, fpBuff, &fpLen);
        if(err) GotoError("", err);
        fingerprint = std::string(fpBuff, fpLen);

        std::cout << "fingerprint: " << fingerprint.c_str() << "\n";
        std::cout << "id:";       std::getline(std::cin, userId);
        std::cout << "password:"; std::getline(std::cin, userPassword);
        return 0;
    Error:
        return err;
    }

    SCRSDK::CrError connect(const SCRSDK::ICrCameraObjectInfo* objInfo)
    {
        SCRSDK::CrError err = SCRSDK::CrError_None;
        int result = SCRSDK::CrError_Generic_Unknown;
        std::string  fingerprint = "";
        std::string  userId = "";
        std::string  userPassword = "";
        std::promise<void> eventPromise;
        std::future<void> eventFuture = eventPromise.get_future();

        m_modelId = _getModelId(objInfo);

        if (objInfo->GetSSHsupport() == SCRSDK::CrSSHsupport_ON) {
            err = _getIdPassword(objInfo, fingerprint, userId, userPassword); if(err) goto Error;
        }

        setEventPromise(&eventPromise);
        err = SCRSDK::Connect((SCRSDK::ICrCameraObjectInfo*)objInfo, this, &m_device_handle,
            SCRSDK::CrSdkControlMode_Remote,
            SCRSDK::CrReconnecting_ON,
            userId.c_str(), userPassword.c_str(), fingerprint.c_str(), (uint32_t)fingerprint.size());
        if(err) GotoError("", err);

    //  std::future_status status = eventFuture.wait_for(std::chrono::milliseconds(3000));
    //  if(status != std::future_status::ready) GotoError("timeout",0);
        try{
            eventFuture.get();
        } catch(const std::exception&) GotoError("", 0);
        result = 0;
    Error:
        return result;
    }

    SCRSDK::CrError disconnect(void)
    {
        if(m_connected) {
            std::promise<void> eventPromise;
            std::future<void> eventFuture = eventPromise.get_future();
            setEventPromise(&eventPromise);
            SCRSDK::Disconnect(m_device_handle);
            eventFuture.wait_for(std::chrono::milliseconds(3000));
            m_connected = false;
        }
        if(m_device_handle) {
            SCRSDK::ReleaseDevice(m_device_handle);
            m_device_handle = 0;
        }
        return SCRSDK::CrError_None;
    }

    SCRSDK::CrError getDeviceProperty(uint32_t code, SCRSDK::CrDeviceProperty* devProp)
    {
        std::int32_t nprop = 0;
        SCRSDK::CrDeviceProperty* prop_list = nullptr;
        SCRSDK::CrError err = SCRSDK::GetSelectDeviceProperties(m_device_handle, 1, &code, &prop_list, &nprop);
        if(err) GotoError("", err);
        if(prop_list && nprop >= 1) {
            *devProp = prop_list[0];
        }
    Error:
        if(prop_list) SCRSDK::ReleaseDeviceProperties(m_device_handle, prop_list);
        return err;
    }

    SCRSDK::CrError setDeviceProperty(uint32_t code, uint64_t data, bool blocking=true)
    {
        int result = SCRSDK::CrError_Generic_Unknown;
        SCRSDK::CrError err = 0;
        std::promise<void> eventPromise;
        std::future<void> eventFuture = eventPromise.get_future();
        std::future_status status;

        SCRSDK::CrDeviceProperty devProp;

        err = getDeviceProperty(code, &devProp);
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
        err = SCRSDK::SetDeviceProperty(m_device_handle, &devProp);
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
};

int main(void)
{
    int result = -1;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    SCRSDK::ICrEnumCameraObjectInfo* enumCameraObjectInfo = nullptr;
    uint32_t count = 0;
    #define DEVICE_MAX  2
    CameraDevice devices[DEVICE_MAX];
    uint32_t device_num = 0;
    uint32_t i;

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

    // enumeration & select camera

    err = SCRSDK::EnumCameraObjects(&enumCameraObjectInfo, 3/*timeInSec*/);
    if(err || !enumCameraObjectInfo) GotoError("no camera", err);

    count = enumCameraObjectInfo->GetCount();
    for (i = 0; i < count; i++) {
        auto* info = enumCameraObjectInfo->GetCameraObjectInfo(i);
        CrCout << '[' << i << "] " << _getModelId(info) << "\n";
    }

    device_num = count;
    if(device_num > DEVICE_MAX) device_num = DEVICE_MAX;
    for(i = 0; i < device_num; i++) {
        uint32_t index;
        std::string inputLine;
        printf("select camera%d:", i); std::getline(std::cin, inputLine);
        try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
        if(index < 0 || index >= count) GotoError("", 0);

        // connect
        err = devices[i].connect(enumCameraObjectInfo->GetCameraObjectInfo(index));
        if(err) goto Error;

        // set work directory
        err = SCRSDK::SetSaveInfo(devices[i].m_device_handle, const_cast<CrChar*>(path.data()), const_cast<CrChar*>(CRSTR("DSC")), -1/*startNo*/);
        if(err) GotoError("", err);
    }

    CrCout << "path=" << path.data() << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::cout << "usage:\n";
    std::cout << "   set <camera index> <DP name> <param> [1-blocking,0-no blocking]\n";
    std::cout << "   get <camera index> <DP name>\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        uint32_t deviceId = 0;
        int32_t code;
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::vector<std::string> args = _split(inputLine, ' ');

        if(args.size() == 0) continue;

        if(args[0] == "q" || args[0] == "Q") break;

        if(args.size() < 3) continue;

        try{ deviceId = (int)_stoll(args[1]); } catch(const std::exception&) {continue;}
        if(deviceId >= device_num) continue;

        code = CrDevicePropertyCode(args[2]);
        if(code < 0) continue;

        if(args[0] == "get") {
            SCRSDK::CrDeviceProperty devProp;
            err = devices[deviceId].getDeviceProperty(code, &devProp);
            if(err) continue;

            if(devProp.GetValueType() != SCRSDK::CrDataType_STR) {
                printf("0x%" PRIx64 "(%" PRId64 ")\n", devProp.GetCurrentValue(), devProp.GetCurrentValue());  // macro for %lld
            }
        } else if(args[0] == "set" && args.size() >= 4) {
            int64_t data = 0;
            bool blocking = true;

            try{ data = _stoll(args[3]); } catch(const std::exception&) {continue;}
            if(args.size() >= 5)
                try{ blocking = _stoll(args[4]); } catch(const std::exception&) {continue;}

            err = devices[deviceId].setDeviceProperty(code, data, blocking);
            if(err) continue;
        } else {
            std::cout << "unknown command\n";
        }
    }

    result = 0;
Error:
    if(enumCameraObjectInfo) enumCameraObjectInfo->Release();
    for(i = 0; i < device_num; i++)
        devices[i].disconnect();
    SCRSDK::Release();

    return result;
}
