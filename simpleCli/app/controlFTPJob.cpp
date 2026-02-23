// "FTP job list" sample
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
#include <unordered_map>
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
  #define CrCin std::wcin
  #define CrPrintf std::wprintf
#else
  using CrString = std::string;
  #define CRSTR(s) s
  #define CrCout std::cout
  #define CrCin std::cin
  #define CrPrintf std::printf
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

    void OnCompleteDownload(CrChar* filename, CrInt32u type)
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
        case SCRSDK::CrWarning_RequestFTPJobList_Result_Success:
        case SCRSDK::CrWarning_ControlFTPJobList_Set_Result_OK:
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;

        case SCRSDK::CrWarning_RequestFTPJobList_Result_DeviceBusy:
        case SCRSDK::CrWarning_RequestFTPJobList_Result_Error:
        case SCRSDK::CrWarning_ControlFTPJobList_Set_Result_Invalid:
        case SCRSDK::CrWarning_ControlFTPJobList_Set_Result_NG:
        case SCRSDK::CrWarning_ControlFTPJobList_Set_Result_DeviceBusy:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
        }
    }


    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3)
    {
        std::cout << "OnWarningExt:" << CrWarningExtString(warning, param1, param2, param3).c_str() << "\n";
    }

    void OnLvPropertyChanged() {}
    void OnLvPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
    void OnPropertyChanged() {}
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}

    void OnNotifyFTPTransferResult(CrInt32u notify, CrInt32u numOfSuccess, CrInt32u numOfFail)
    {
        std::cout << "OnNotifyFTPTransferResult" << CrErrorString(notify) << "\n";
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

SCRSDK::CrError _getMediaProfile(int64_t device_handle, SCRSDK::CrMediaProfile slot)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;

    {
        const std::unordered_map<SCRSDK::CrMediaProfile, SCRSDK::CrDevicePropertyCode> map_DP_status
        {
            {SCRSDK::CrMediaProfile_Slot1, SCRSDK::CrDeviceProperty_MediaSLOT1_Status},
            {SCRSDK::CrMediaProfile_Slot2, SCRSDK::CrDeviceProperty_MediaSLOT2_Status},
            {SCRSDK::CrMediaProfile_Slot3, SCRSDK::CrDeviceProperty_MediaSLOT3_Status},
        };

        SCRSDK::CrDeviceProperty devProp;

        auto iter = map_DP_status.find(slot);
        if(iter == end(map_DP_status)) GotoError("", 0);

        err = _getDeviceProperty(device_handle, (uint32_t)iter->second, &devProp);
        if(err) GotoError("", err);
        if(devProp.GetCurrentValue() != (int)SCRSDK::CrSlotStatus_OK) GotoError("not ready", 0);
    }

    {
        CrInt32u nums = 0;
        SCRSDK::CrMediaProfileInfo* mediaProfileList = nullptr;
        err = SCRSDK::GetMediaProfile(device_handle, slot, &mediaProfileList, &nums);
        if(err || nums == 0) GotoError("", err);

        for (CrInt32u i = 0; i < nums; i++) {
            #define _CHAR(a) (a ? (const char*)a : "")
            
            printf("id:%d\n", i);
            std::cout << "contentName:" << _CHAR(mediaProfileList[i].contentName) << "\n";
            std::cout << "contentUrl:" << _CHAR(mediaProfileList[i].contentUrl) << "\n";
            std::cout << "contentType:" << _CHAR(mediaProfileList[i].contentType) << "\n";
            std::cout << "contentFrameRate:" << _CHAR(mediaProfileList[i].contentFrameRate) << "\n";
            std::cout << "contentAspectRatio:" << _CHAR(mediaProfileList[i].contentAspectRatio) << "\n";
            std::cout << "contentChannel:" << _CHAR(mediaProfileList[i].contentChannel) << "\n";
            std::cout << "contentVideoType:" << _CHAR(mediaProfileList[i].contentVideoType) << "\n";
            std::cout << "contentAudioType:" << _CHAR(mediaProfileList[i].contentAudioType) << "\n";
            if (mediaProfileList[i].proxyUrl != nullptr) {
                std::cout << "proxyUrl:" << _CHAR(mediaProfileList[i].proxyUrl) << "\n";
                std::cout << "proxyType:" << _CHAR(mediaProfileList[i].proxyType) << "\n";
                std::cout << "proxyFrameRate:" << _CHAR(mediaProfileList[i].proxyFrameRate) << "\n";
                std::cout << "proxyAspectRatio:" << _CHAR(mediaProfileList[i].proxyAspectRatio) << "\n";
                std::cout << "proxyChannel:" << _CHAR(mediaProfileList[i].proxyChannel) << "\n";
                std::cout << "proxyVideoType:" << _CHAR(mediaProfileList[i].proxyVideoType) << "\n";
                std::cout << "proxyAudioType:" << _CHAR(mediaProfileList[i].proxyAudioType) << "\n";
            }
            std::cout << "thumbnailUrl:" << _CHAR(mediaProfileList[i].thumbnailUrl) << "\n";
            std::cout << "metaUrl:" << _CHAR(mediaProfileList[i].metaUrl) << "\n";

            std::cout << "umid:";
            for(int j = 0; j < 32; j++) {
                printf("%02x", mediaProfileList[i].umid[j]);
            }
            std::cout << "\n";

            printf("duration:%d\n", mediaProfileList[i].duration);
            printf("restrictionFrame:%d\n", mediaProfileList[i].restrictionFrame);
            printf("isTrimmingAvailable:%d\n", mediaProfileList[i].isTrimmingAvailable);
            std::cout << "\n";
        }
        SCRSDK::ReleaseMediaProfile(device_handle, mediaProfileList);
    }
Error:
    return err;
}

SCRSDK::CrError _addMediaProfileToFTPJob(int64_t device_handle, SCRSDK::CrMediaProfile slot, uint32_t mediaIndex, uint32_t proxyFlag)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;

    SCRSDK::CrDeviceProperty devProp;
    CrInt32u nums = 0;
    SCRSDK::CrMediaProfileInfo* mediaProfileList = nullptr;
    SCRSDK::CrFTPJobSetting* ftpJobSetting = nullptr;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();

    err = SCRSDK::GetMediaProfile(device_handle, slot, &mediaProfileList, &nums);
    if(err || nums == 0 || mediaIndex >= nums) GotoError("", err);

    err = _getDeviceProperty(device_handle, SCRSDK::CrDeviceProperty_SelectFTPServerID, &devProp);
    if(err) GotoError("", err);

    if (proxyFlag) {
        if(mediaProfileList[mediaIndex].proxyUrl == nullptr) GotoError("no proxy", 0);

        ftpJobSetting = new SCRSDK::CrFTPJobSetting(
            /*trimType*/ SCRSDK::CrFTPJobTrimType_NoTrim,
            /*serverId*/ (CrInt32u)devProp.GetCurrentValue(),
            /*slotId*/ (SCRSDK::CrFTPJobSlotId)slot,
            /*clipPath*/ (char*)mediaProfileList[mediaIndex].proxyUrl,
            /*metaPath*/ nullptr,
            /*transferDir*/ nullptr,
            /*inFrame*/ 0,
            /*outFrame*/ 0,
            /*duration*/ 0,
            /*destClipName*/ nullptr,
            /*umid*/ nullptr,
            /*videoType*/ nullptr,
            /*compJobAction*/ SCRSDK::CrFTPJobCompleteAction_NoAction,
            /*deleteJobAction*/ SCRSDK::CrFTPJobDeleteAction_NoAction);
    } else {
        ftpJobSetting = new SCRSDK::CrFTPJobSetting(
            /*trimType*/ SCRSDK::CrFTPJobTrimType_NoTrim,
            /*serverId*/ (CrInt32u)devProp.GetCurrentValue(),
            /*slotId*/ (SCRSDK::CrFTPJobSlotId)slot,
            /*clipPath*/ (char*)mediaProfileList[mediaIndex].contentUrl,
            /*metaPath*/ (char*)mediaProfileList[mediaIndex].metaUrl,
            /*transferDir*/ nullptr,
            /*inFrame*/ 0,
            /*outFrame*/ 0,
            /*duration*/ 0,
            /*destClipName*/ nullptr,
            /*umid*/ nullptr,
            /*videoType*/ nullptr,
            /*compJobAction*/ SCRSDK::CrFTPJobCompleteAction_NoAction,
            /*deleteJobAction*/ SCRSDK::CrFTPJobDeleteAction_NoAction);
    }
    setEventPromise(&eventPromise);

    err = SCRSDK::ControlFTPJobList(device_handle, SCRSDK::CrFTPJobControlType_Add, ftpJobSetting, 1, SCRSDK::CrFTPJobDeleteType_Individual);
    if(err) GotoError("", err);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);
    result = 0;
Error:
    setEventPromise(nullptr);
    if(ftpJobSetting) delete ftpJobSetting;
    return result;
}


void _printFTPJobInfo(SCRSDK::CrFTPJobInfo* jobInfo)
{
    #define _printChar(msg, dp) { \
        const char* _dp = dp; \
        std::cout << msg; \
        if(_dp) std::cout << _dp; \
        std::cout << "\n"; \
    }

    printf("jobId:%d\n",        jobInfo->jobId);
    printf("serverId:%d\n",     jobInfo->serverId);
    printf("slotId:%d\n",       jobInfo->slotId);
    printf("jobStatus:%d\n",    jobInfo->jobStatus);
    printf("chunkNum:%d\n",     jobInfo->chunkNum);
    printf("fileSize:%d\n",     (int)jobInfo->fileSize);
    printf("transferSize:%d\n", (int)jobInfo->transferSize);
    _printChar("clipName:",     jobInfo->GetClipName());
    _printChar("mainName:",     jobInfo->GetMainName());
    _printChar("metaName:",     jobInfo->GetMetaName());
    return;
}

SCRSDK::CrError _getFTPJobInfo(int64_t device_handle/*, uint32_t index*/)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();

    // request FTPJobList
    setEventPromise(&eventPromise);

    err = SCRSDK::RequestFTPJobList(device_handle);
    if(err) GotoError("", err);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);

    // Get FTPJobList
    {
        SCRSDK::CrFTPJobInfo* ftpJobList = nullptr;
        CrInt32u ftpJobListSize = 0;

        err = SCRSDK::GetFTPJobList(device_handle, &ftpJobList, &ftpJobListSize);
        if(err || ftpJobListSize == 0) GotoError("", err);

        for(uint32_t i = 0; i < ftpJobListSize; i++) {
            _printFTPJobInfo(ftpJobList + i);
            std::cout << "\n";
        }
        SCRSDK::ReleaseFTPJobList(device_handle, ftpJobList);
    }

    result = 0;
Error:
    setEventPromise(nullptr);
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
    std::cout << "   media <1~3(slot)>               - get content info on media\n";
    std::cout << "   add <1~3(slot)> <0~(contentId)> - add FTP job\n";
    std::cout << "   job                             - get job info\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        std::string inputLine;
        std::getline(std::cin, inputLine);
        std::vector<std::string> args = _split(inputLine, ' ');

        if(args.size() == 0) {
            
        } else if((args[0] == "media" || args[0] == "m") && args.size() >= 2) {
            int slot = 0;
            try {
                slot = stol(args[1]);
            } catch(const std::exception&) { GotoError("", 0); }
            err = _getMediaProfile(m_device_handle, (SCRSDK::CrMediaProfile)slot);
            if(err) goto Error;

        } else if((args[0] == "add" || args[0] == "a") && args.size() >= 3) {
            int slot = 0;
            int mediaIndex = 0;
            int proxyFlag = 0;
            try {
                slot = stol(args[1]);
                mediaIndex = stol(args[2]);
                if(args.size() >= 4) proxyFlag = stol(args[3]);
            } catch(const std::exception&) { GotoError("", 0); }

            err = _addMediaProfileToFTPJob(m_device_handle, (SCRSDK::CrMediaProfile)slot, mediaIndex, proxyFlag);
            if(err) goto Error;

        } else if((args[0] == "job" || args[0] == "j") ) {
            err = _getFTPJobInfo(m_device_handle);
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
