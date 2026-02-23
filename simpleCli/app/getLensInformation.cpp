// "use LensInformation" sample
#include <chrono>
#include <cmath>
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

bool  m_connected = false;
CrString m_modelId;

std::mutex m_eventPromiseMutex;
std::promise<void>* m_eventPromise = nullptr;
void setEventPromise(std::promise<void>* dp)
{
    std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
    m_eventPromise = dp;
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
        std::cout << "OnWarning:" << CrErrorString(warning) << "\n";
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        switch(warning) {
        case SCRSDK::CrWarning_RequestLensInformation_Result_Success:
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;
        case SCRSDK::CrWarning_RequestLensInformation_Result_DeviceBusy:
        case SCRSDK::CrWarning_RequestLensInformation_Result_Error:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
        }
    }

    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3) {}
    void OnLvPropertyChanged() {}
    void OnLvPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
    void OnPropertyChanged() {}
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes)
    {
        std::cout << "OnPropertyChangedCodes:\n";
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

SCRSDK::CrError _getLensInformation(SCRSDK::CrDeviceHandle deviceHandle, SCRSDK::CrLensInformation** lensInfos, uint32_t* numOfList)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();

    setEventPromise(&eventPromise);
    err = SCRSDK::RequestLensInformation(deviceHandle);
    if(err) GotoError("", err);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);

    err = SCRSDK::GetLensInformation(deviceHandle, lensInfos, numOfList);
    if(err || numOfList == 0) GotoError("", err);
    result = 0;
Error:
    setEventPromise(nullptr);
    return result;
}

int main(void)
{
    int result = -1;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    int64_t  m_device_handle = 0;
    SCRSDK::ICrEnumCameraObjectInfo* enumCameraObjectInfo = nullptr;
    SCRSDK::ICrCameraObjectInfo* objInfo = nullptr;
    DeviceCallback deviceCallback;
    CrInt32u numOfList= 0;
    SCRSDK::CrLensInformation* lensInfos = nullptr;
    int iMin=0;
    int iMax=0;

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
    std::cout << "   lens\n";
    std::cout << "   conv <distance[cm]>\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::vector<std::string> args = _split(inputLine, ' ');

        if(args.size() == 0) {

        } else if(args[0] == "lens") {
            int i;
            if(lensInfos) SCRSDK::ReleaseLensInformation(m_device_handle, lensInfos);

            err = _getLensInformation(m_device_handle, &lensInfos, &numOfList);
            if(err) goto Error;
            std::cout << "OK\n";

            //for(i = 0; i < numOfList; i++)
            //  printf("%d, %d, %d, %d\n", lensInfos[i].dataVersion, lensInfos[i].type, lensInfos[i].normalizedValue, lensInfos[i].focusPosition);

            // check "meter" tableRange
            iMin = -1;
            iMax = numOfList-1;
            for(i = 0; i < (int)numOfList; i++) {
                if(lensInfos[i].type == SCRSDK::CrLensInformationType_Meter) {
                    if(iMin == -1) {
                        iMin = i;
                    }
                } else if(lensInfos[i].type == SCRSDK::CrLensInformationType_Feet) {
                    if(iMin != -1) {
                        iMax = i-1;
                        break;
                    }
                }
            }
            //printf("%d-%d(%d)\n", iMin, iMax, numOfList);
        } else if(args[0] == "conv" && args.size() >= 2) {
            uint32_t distance;
            int i;
            try { distance = stoi(args[1]); } catch(const std::exception&) { GotoError("", 0); }
            if(!lensInfos) continue;

            if(distance <= lensInfos[iMin].focusPosition) {
                printf("%d\n", lensInfos[iMin].normalizedValue);

            } else if(distance >= lensInfos[iMax].focusPosition) {
                printf("%d\n", lensInfos[iMax].normalizedValue);

            } else {
                for(i = iMin; i <= iMax-1; i++) {
                    if(distance <= lensInfos[i+1].focusPosition) {
                        // log-log linear regression
                        double lnx1 = std::log(lensInfos[i+0].focusPosition);
                        double lnx2 = std::log(lensInfos[i+1].focusPosition);
                        double lny1 = std::log(lensInfos[i+0].normalizedValue);
                        double lny2 = std::log(lensInfos[i+1].normalizedValue);
                        double k = (lny2 - lny1) / (lnx2 - lnx1);
                        double lnx = std::log(distance);
                        double lny = (lnx - lnx1) * k + lny1;
                        int32_t y = (int32_t)std::round(std::exp(lny));
                        printf("%d\n", y);
                        break;
                    }
                }
            }
        } else if(args[0] == "q" || args[0] == "Q") {
            break;
        } else {
            std::cout << "unknown command\n";
        }
    }

    result = 0;
Error:
    if(lensInfos) SCRSDK::ReleaseLensInformation(m_device_handle, lensInfos);
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
