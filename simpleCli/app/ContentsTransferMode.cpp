// "pull contents in ContentsTransferMode" sample
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
  #define CrPrintf std::wprintf
  #define DELIMITER CRSTR("\\")
#else
  using CrString = std::string;
  #define CRSTR(s) s
  #define CrCout std::cout
  #define CrPrintf std::printf
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
int64_t  m_device_handle = 0;

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
        std::cout << "OnNotifyContentsTransfer(" << CrErrorString(notify) << "):";
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        switch(notify) {
        case SCRSDK::CrNotify_ContentsTransfer_Start:
            break;
        case SCRSDK::CrNotify_ContentsTransfer_Complete:
            CrCout << filename;
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;
        default:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
        }
        std::cout << "\n";
    }

    void OnWarning(CrInt32u warning)
    {
        if (warning == SCRSDK::CrWarning_Connect_Reconnecting) {
            CrCout << "Reconnecting to " << m_modelId << "\n";
            return;
        }
        std::cout << "OnWarning:" << CrErrorString(warning) << "\n";
        switch(warning) {
        case SCRSDK::CrWarning_ContentsTransferMode_DeviceBusy:
        case SCRSDK::CrWarning_ContentsTransferMode_StatusError:
        case SCRSDK::CrWarning_ContentsTransferMode_CanceledFromCamera:
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
        for(uint32_t i = 0; i < num; ++i) {
            std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
            if(m_setDPCode && m_setDPCode == codes[i]) {
                std::string name = CrDevicePropertyString((SCRSDK::CrDevicePropertyCode)codes[i]);
                SCRSDK::CrDeviceProperty devProp;
                SCRSDK::CrError err = _getDeviceProperty(m_device_handle, codes[i], &devProp);
                if(err) break;
                int64_t current = devProp.GetCurrentValue();
                printf("OnPropertyChangedCodes:%s=%d\n", name.c_str(), (int)current);
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

SCRSDK::CrError _getThumbnail(int64_t m_device_handle, SCRSDK::CrContentHandle content, CrString path)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = 0;
    #define  BUF_SIZE 0x28000 // @@@@ temp
    SCRSDK::CrImageDataBlock image_data;
    CrInt8u image_buff[BUF_SIZE];
    SCRSDK::CrFileType fileType = SCRSDK::CrFileType_None;

    image_data.SetSize(BUF_SIZE);
    image_data.SetData(image_buff);

    err = SCRSDK::GetContentsThumbnailImage(m_device_handle, content, &image_data, &fileType);
    if(err) GotoError("", err);

    if (image_data.GetSize() <= 0 || fileType == SCRSDK::CrFileType_None)
        GotoError("", 0);

    {
        CrString filename = DELIMITER CRSTR("Thumbnail.JPG");
        if (fileType == SCRSDK::CrFileType_Heif) {
            filename= DELIMITER CRSTR("Thumbnail.HIF");
        }
        filename = path.append(filename);
        std::ofstream file(filename, std::ios::out | std::ios::binary);
        if (file.bad()) GotoError("", 0);

        file.write((char*)image_data.GetImageData(), image_data.GetImageSize());
        file.close();
        CrCout << "complete:" << filename.data() << "\n";
    }
    result = 0;
Error:
    return result;
}

int main(void)
{
    int result = -1;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    SCRSDK::ICrEnumCameraObjectInfo* enumCameraObjectInfo = nullptr;
    SCRSDK::ICrCameraObjectInfo* objInfo = nullptr;
    DeviceCallback deviceCallback;

    SCRSDK::CrMtpFolderInfo* folderList = nullptr;
    SCRSDK::CrContentHandle* contentsHandleList = nullptr;
    std::string inputLine;

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
            SCRSDK::CrSdkControlMode_ContentsTransfer,
            SCRSDK::CrReconnecting_ON,
            userId.c_str(), userPassword.c_str(), fingerprint.c_str(), (uint32_t)fingerprint.size());
        if(err) GotoError("", err);

    //  std::future_status status = eventFuture.wait_for(std::chrono::milliseconds(3000));
    //  if(status != std::future_status::ready) GotoError("timeout",0);
        try{
            eventFuture.get();
        } catch(const std::exception&) GotoError("", 0);
    }

    // wait contentsTransfer=ON
    {
        SCRSDK::CrDeviceProperty devProp;
        std::promise<void> eventPromise;
        std::future<void> eventFuture = eventPromise.get_future();
        {
            std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
            m_setDPCode = SCRSDK::CrDeviceProperty_ContentsTransferStatus;
            m_eventPromise = &eventPromise;
        }

        try{
            eventFuture.get();
        } catch(const std::exception&) GotoError("", 0);

        err = _getDeviceProperty(m_device_handle, SCRSDK::CrDeviceProperty_ContentsTransferStatus, &devProp);
        if(err) GotoError("", err);
        if(devProp.GetCurrentValue() != SCRSDK::CrContentsTransfer_ON) GotoError("", 0);
    }

    // set work directory
    {
        CrCout << "path=" << path.data() << "\n";
        err = SCRSDK::SetSaveInfo(m_device_handle, const_cast<CrChar*>(path.data()), const_cast<CrChar*>(CRSTR("DSC")), -1/*startNo*/);
        if(err) GotoError("", err);
    }

    #if defined(__linux__)
    {
        CrInt32u bufferSize = 0;
        err = SCRSDK::GetDeviceSetting(m_device_handle, SCRSDK::Setting_Key_PartialBuffer, &bufferSize);
        if(err) GotoError("", err);
        printf("PartialBuffer %d[MB]->", bufferSize); std::getline(std::cin, inputLine);
        if(inputLine != "") {
            try { bufferSize = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
            err = SCRSDK::SetDeviceSetting(m_device_handle, SCRSDK::Setting_Key_PartialBuffer, bufferSize);
            if(err) GotoError("", err);
        }
    }
    #endif

    while(1) {
        int index = 0;
        SCRSDK::CrFolderHandle folderHandle;
        SCRSDK::CrContentHandle contentHandle;
        // select folder
        {
            CrInt32u f_nums = 0;
            err = SCRSDK::GetDateFolderList(m_device_handle, &folderList, &f_nums);
            if(err) GotoError("", err);
            if(!folderList || f_nums == 0) GotoError("", 0);

            for (CrInt32u i = 0; i < f_nums; ++i) {
                CrPrintf(CRSTR(" %d: %s\n"), i+1, folderList[i].folderName);
            }
            std::cout << "folder:"; std::getline(std::cin, inputLine);
            try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
            if (index < 1 || index > (int)f_nums) GotoError("", 0);

            folderHandle = folderList[index-1].handle;
            SCRSDK::ReleaseDateFolderList(m_device_handle, folderList);
            folderList = nullptr;
        }
        // select content
        {
            SCRSDK::CrMtpContentsInfo contentsDetailInfo;
            CrInt32u c_nums = 0;
            err = SCRSDK::GetContentsHandleList(m_device_handle, folderHandle, &contentsHandleList, &c_nums);
            if(err) GotoError("", err);
            if(!contentsHandleList || c_nums == 0) GotoError("", 0);

            for (CrInt32u i = 0; i < c_nums; i++) {
                err = SCRSDK::GetContentsDetailInfo(m_device_handle, contentsHandleList[i], &contentsDetailInfo);
                if(err) GotoError("", err);
                CrPrintf(CRSTR(" %d: %s\n"), i+1, contentsDetailInfo.fileName);
            }
            std::cout << "content:"; std::getline(std::cin, inputLine);
            try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
            if (index < 1 || index > (int)c_nums) GotoError("", 0);

            err = SCRSDK::GetContentsDetailInfo(m_device_handle, contentsHandleList[index-1], &contentsDetailInfo);
            if(err) GotoError("", err);
            contentHandle = contentsDetailInfo.handle;
            SCRSDK::ReleaseContentsHandleList(m_device_handle, contentsHandleList);
            contentsHandleList = nullptr;
        }
        // select type & pull content
        {
            std::cout << " 1:Original\n"" 2:2M\n"" 3:Thmbnail\n""type:"; std::getline(std::cin, inputLine);
            try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
            if (index < 1 || index > 3) GotoError("", 0);

            if(index == 3) {
                err = _getThumbnail(m_device_handle, contentHandle, path);
                if(err) goto Error;
            } else {
                std::promise<void> eventPromise;
                std::future<void> eventFuture = eventPromise.get_future();
                setEventPromise(&eventPromise);
                if(index == 1) {
                    err = SCRSDK::PullContentsFile(m_device_handle, contentHandle, SCRSDK::CrPropertyStillImageTransSize_Original);
                } else {
                    // only for .JPG, .ARW, .HIF
                    err = SCRSDK::PullContentsFile(m_device_handle, contentHandle, SCRSDK::CrPropertyStillImageTransSize_SmallSize);
                }
                if(err) GotoError("", err);
                try{
                    eventFuture.get();
                } catch(const std::exception&) GotoError("", 0);

                std::this_thread::sleep_for(std::chrono::milliseconds(100));  // workaround for GetDateFolderList error
            }
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
    if(m_device_handle) {
        if(folderList) SCRSDK::ReleaseDateFolderList(m_device_handle, folderList);
        if(contentsHandleList) SCRSDK::ReleaseContentsHandleList(m_device_handle, contentsHandleList);
        SCRSDK::ReleaseDevice(m_device_handle);
    }
    SCRSDK::Release();

    return result;
}
