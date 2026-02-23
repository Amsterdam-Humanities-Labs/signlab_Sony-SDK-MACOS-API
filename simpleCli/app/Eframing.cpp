// eframing sample
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
#else
  using CrString = std::string;
  #define CRSTR(s) s
  #define CrCout std::cout
#endif


#include "CrDeviceProperty.h"
#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"

#define PrintError(msg, err) { fprintf(stderr, "Error in %s(%d):" msg ",0x%x\n", __FUNCTION__, __LINE__, err); }
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
        printf("Connection error 0x%x\n", error);
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
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
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

    SCRSDK::CrEframingInfo eframingInfo;

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
    std::cout << "   set <horizontal_denominator> <vertical_denominator> <eframingType>\n";
    std::cout << "   addarea <in/out> <areaNo> <x> <y> <width> <height>\n";
    std::cout << "   removearea <in/out> <areaNo>\n";
    std::cout << "   commit\n";
    std::cout << "   update <in/out> <areaNo> <x> <y> <width> <height>\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::vector<std::string> args = _split(inputLine, ' ');

        const std::string cmdName = args.size()==0 ? "" : args[0];

        if(cmdName == "set" && args.size() >= 4) {
            uint32_t val_h=0, val_v=0;
            try { val_h = stoi(args[1]); } catch(const std::exception&) { GotoError("", 0); }
            try { val_v = stoi(args[2]); } catch(const std::exception&) { GotoError("", 0); }

            SCRSDK::CrEframingType type = SCRSDK::CrEframingType_None;
            try {
                std::string sType = args[3];
                if(sType == "None") type = SCRSDK::CrEframingType_None;
                else if(sType == "Auto") type = SCRSDK::CrEframingType_Auto;
                else if(sType == "Single") type = SCRSDK::CrEframingType_Single;
                else if(sType == "PTZ") type = SCRSDK::CrEframingType_PTZ;
                else if(sType == "HoldCurrentPosition") type = SCRSDK::CrEframingType_HoldCurrentPosition;
                else if(sType == "ForceZoomOut") type = SCRSDK::CrEframingType_ForceZoomOut;
                else GotoError("unknown type", 0);
            } catch(const std::exception&) { GotoError("", 0); }
            
            eframingInfo.horizontal_denominator = val_h * 1024;
            eframingInfo.vertical_denominator = val_v * 1024;
            eframingInfo.eframingType = type;

        }else if(cmdName == "addarea" && args.size() >= 7) {
            try {
                SCRSDK::CrEframingAreaGroup group = (args[1] == "in") ? SCRSDK::CrEframingInputArea : SCRSDK::CrEframingOutputArea;
                SCRSDK::CrEframingAreaNumber areaNo = static_cast<SCRSDK::CrEframingAreaNumber>(std::stoi(args[2]));
                SCRSDK::CrEframingRectangle rect;
                rect.x = std::stoi(args[3]) * 1024;
                rect.y = std::stoi(args[4]) * 1024;
                rect.width = std::stoi(args[5]) * 1024;
                rect.height = std::stoi(args[6]) * 1024;

                if (group == SCRSDK::CrEframingInputArea) {
                    eframingInfo.addInputInfo(areaNo, rect);
                    std::cout << "Added input area to eframingInfo.\n";
                } else if (group == SCRSDK::CrEframingOutputArea) {
                    eframingInfo.addOutputInfo(areaNo, rect);
                    std::cout << "Added output area to eframingInfo.\n";
                } else {
                    GotoError("Invalid group for addarea", 0);
                }
            } catch (const std::exception&) {
                GotoError("Invalid arguments for addarea", 0);
            }
        }else if(cmdName == "removearea" && args.size() >= 3) {
            try {
                SCRSDK::CrEframingAreaGroup group = (args[1] == "in") ? SCRSDK::CrEframingInputArea : SCRSDK::CrEframingOutputArea;
                SCRSDK::CrEframingAreaNumber areaNo = static_cast<SCRSDK::CrEframingAreaNumber>(std::stoi(args[2]));

                if (group == SCRSDK::CrEframingInputArea) {
                    eframingInfo.removeInputInfo(areaNo);
                    std::cout << "Removed input area from eframingInfo.\n";
                } else if (group == SCRSDK::CrEframingOutputArea) {
                    eframingInfo.removeOutputInfo(areaNo);
                    std::cout << "Removed output area from eframingInfo.\n";
                } else {
                    GotoError("Invalid group for removearea", 0);
                }
            } catch (const std::exception&) {
                GotoError("Invalid arguments for removearea", 0);
            }
        }else if(cmdName == "commit") {
            err = SCRSDK::ExecuteEframing(m_device_handle, &eframingInfo);
            if (err) {
                GotoError("Failed to execute eframing", err);
            } else {
                std::cout << "Eframing executed successfully.\n";
            }
        }else if(cmdName == "update" && args.size() >= 7) {
            try {
                SCRSDK::CrEframingAreaGroup group = (args[1] == "in") ? SCRSDK::CrEframingInputArea : SCRSDK::CrEframingOutputArea;
                SCRSDK::CrEframingAreaNumber areaNo = static_cast<SCRSDK::CrEframingAreaNumber>(std::stoi(args[2]));
                CrInt16 x = static_cast<CrInt16>(std::stoi(args[3]) * 1024);
                CrInt16 y = static_cast<CrInt16>(std::stoi(args[4]) * 1024);
                CrInt16 width = static_cast<CrInt16>(std::stoi(args[5]) * 1024);
                CrInt16 height = static_cast<CrInt16>(std::stoi(args[6]) * 1024);

                err = SCRSDK::UpdateEframingArea(m_device_handle, areaNo, group, x, y, width, height);
                if (err) {
                    GotoError("Failed to update eframing area", err);
                } else {
                    std::cout << "Eframing area updated successfully.\n";
                }
            } catch (const std::exception&) {
                GotoError("Invalid arguments for update", 0);
            }
        }else if(inputLine == "q" || inputLine == "Q") {
            break;
        }else{
            std::cout << "unknown DP nor CMD\n";
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
