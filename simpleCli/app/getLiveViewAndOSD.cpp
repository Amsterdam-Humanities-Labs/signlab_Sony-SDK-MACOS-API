// "get live view and OSD" sample
#include <chrono>
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
  #define DELIMITER CRSTR("\\")
#else
  using CrString = std::string;
  #define CRSTR(s) s
  #define CrCout std::cout
  #define DELIMITER CRSTR("/")
#endif


#include "CrDeviceProperty.h"
#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "CrDebugString.h"   // use CrDebugString.cpp

#define PrintError(msg, err) { fprintf(stderr, "Error in %s(%d):" msg ",%s\n", __FUNCTION__, __LINE__, (err ? CrErrorString(err).c_str():"")); }
#define GotoError(msg, err) { PrintError(msg, err); goto Error; }

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

SCRSDK::CrError _setDeviceProperty(int64_t device_handle, uint32_t code, uint64_t data)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = 0;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();
    std::future_status status;

    SCRSDK::CrDeviceProperty devProp;

    err = _getDeviceProperty(device_handle, code, &devProp);
    if(err) GotoError("", err);
    if (devProp.GetCurrentValue() == data) {
        std::cout << "skipped\n";
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        m_setDPCode = code;
        m_eventPromise = &eventPromise;
    }

    devProp.SetCurrentValue(data);
    err = SCRSDK::SetDeviceProperty(device_handle, &devProp);
    if(err) GotoError("", err);

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
    }

    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3) {}
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

SCRSDK::CrError _getLiveView(int64_t device_handle, CrString path)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = 0;
    CrInt32 num = 0;
    SCRSDK::CrLiveViewProperty* property = nullptr;
    SCRSDK::CrImageInfo imageInfo;
    SCRSDK::CrImageDataBlock image_data;
    CrInt32u bufSize = 0;
    CrInt8u* image_buff = nullptr;

    err = SCRSDK::GetLiveViewProperties(device_handle, &property, &num);  if(err) GotoError("", err);
    SCRSDK::ReleaseLiveViewProperties(device_handle, property);

    err = SCRSDK::GetLiveViewImageInfo(device_handle, &imageInfo);  if(err) GotoError("", err);
    bufSize = imageInfo.GetBufferSize();
    if (bufSize <= 0) GotoError("", 0);

    image_buff = new CrInt8u[bufSize];
    if (!image_buff) GotoError("", 0);

    image_data.SetData(image_buff);
    image_data.SetSize(bufSize);

    err = SCRSDK::GetLiveViewImage(device_handle, &image_data);  if(err) GotoError("", err);
    if (image_data.GetSize() <= 0) GotoError("", 0);

    {
        path.append(DELIMITER CRSTR("LiveView000000.JPG"));
        std::ofstream file(path, std::ios::out | std::ios::binary);
        if (file.bad()) GotoError("", 0);
        file.write((char*)image_data.GetImageData(), image_data.GetImageSize());
        file.close();
        CrCout << path.data() << '\n';
    }
    result = 0;
Error:
    if(image_buff) delete[] image_buff;
    return result;
}

SCRSDK::CrError _getOsdImage(int64_t device_handle, CrString path)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = 0;
    SCRSDK::CrDeviceProperty devProp;
    SCRSDK::CrOSDImageDataBlock image_data;
    CrInt8u* image_buff = nullptr;

    err = _getDeviceProperty(device_handle, SCRSDK::CrDeviceProperty_OSDImageMode, &devProp);
    if(err) GotoError("", err);

    if (devProp.GetCurrentValue() != SCRSDK::CrOSDImageMode_On)
        GotoError("please turn on OSD image", 0);

    image_buff = new CrInt8u[CR_OSD_IMAGE_MAX_SIZE]; if(!image_buff) GotoError("", 0);
    image_data.SetData(image_buff);

    err = SCRSDK::GetOSDImage(device_handle, &image_data); if(err) GotoError("", err);
    if (image_data.GetImageSize() <= 0) GotoError("", 0);

    {
        path.append(DELIMITER CRSTR("OSDImage000000.PNG"));
        std::ofstream file(path, std::ios::out | std::ios::binary);
        if (file.bad()) GotoError("", 0)
        file.write((char*)image_data.GetImageData(), image_data.GetImageSize());
        file.close();
        CrCout << path.data() << '\n';
    }
    result = 0;
Error:
    if(image_buff) delete[] image_buff;
    return result;
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

int main(void)
{
    int result = -1;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    int64_t  m_device_handle = 0;
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
    std::cout << "   p <1(Main),2(httpLV)> - set live view protocol\n";
    std::cout << "   l                     - get live view\n";
    std::cout << "   t                     - turn On OSD image\n";
    std::cout << "   o                     - get OSD image\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::vector<std::string> args = _split(inputLine, ' ');

        if(args.size() == 0) {

        } else if(args[0] == "p" && args.size() >=2) {
            uint32_t val;
            try { val = stoi(args[1]); } catch(const std::exception&) { GotoError("", 0); }
            err = _setDeviceProperty(m_device_handle, SCRSDK::CrDeviceProperty_LiveViewProtocol, val);
            if(err) goto Error;

        } else if(args[0] == "l" || args[0] == "L") {
            err = _getLiveView(m_device_handle, path);
            if(err) goto Error;

        } else if(args[0] == "t" || args[0] == "T") {
            err = _setDeviceProperty(m_device_handle, SCRSDK::CrDeviceProperty_OSDImageMode, SCRSDK::CrOSDImageMode_On);
            if(err) goto Error;

        } else if(args[0] == "o" || args[0] == "O") {
            err = _getOsdImage(m_device_handle, path);
            if(err) goto Error;

        } else if(args[0] == "q" || args[0] == "Q") {
            break;
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
        eventFuture.wait_for(std::chrono::milliseconds(3000));
    }
    if(m_device_handle) SCRSDK::ReleaseDevice(m_device_handle);
    SCRSDK::Release();

    return result;
}
