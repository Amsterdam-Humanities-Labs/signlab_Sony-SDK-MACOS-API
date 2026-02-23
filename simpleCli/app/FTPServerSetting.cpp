// "FTP server setting" sample
#include <chrono>
#include <codecvt>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
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

#if defined(_WIN32) || defined(_WIN64)
  #include <fcntl.h>
  #include <io.h>
#else
  #include <unistd.h>
#endif

// macro for multibyte character
#if defined(_WIN32) || defined(_WIN64)
  using CrString = std::wstring;
  #define CRSTR(s) L ## s
  #define CrCout std::wcout
  #define CrCin std::wcin
  #define CrPrintf std::wprintf
  #define DELIMITER CRSTR("\\")
  #define CrToUtf8(a) wstring_convert.to_bytes(a)
  #define Utf8ToCr(a) wstring_convert.from_bytes(a)
  static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> wstring_convert;
#else
  using CrString = std::string;
  #define CRSTR(s) s
  #define CrCout std::cout
  #define CrCin std::cin
  #define CrPrintf std::printf
  #define DELIMITER CRSTR("/")
  #define CrToUtf8(a) (a)
  #define Utf8ToCr(a) (a)
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
        case SCRSDK::CrWarning_RequestFTPServerSettingList_Success:
        case SCRSDK::CrWarning_SetFTPServerSetting_Result_OK:
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;
        case SCRSDK::CrWarning_RequestFTPServerSettingList_DeviceBusy:
        case SCRSDK::CrWarning_RequestFTPServerSettingList_Error:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
    //  case SCRSDK::CrWarning_FTP_Display_Name_List_Changed:
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
        printf("OnNotifyFTPTransferResult(%s):success=%d,fail=%d\n", CrErrorString(notify).c_str(), numOfSuccess, numOfFail);
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        switch(notify) {
        case SCRSDK::CrNotify_FTPTransferResult_Success:
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;
        case SCRSDK::CrNotify_FTPTransferResult_Failure:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
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

int64_t _stoll(CrString inputLine)
{
    int64_t data = 0;
    if(inputLine.empty()) throw std::runtime_error("error");
    try {
        if (inputLine.compare(0, 2, CRSTR("0x")) == 0 || inputLine.compare(0, 2, CRSTR("0X")) == 0) {
            data = std::stoull(inputLine.substr(2), nullptr, 16);
        } else {
            data = std::stoll(inputLine, nullptr, 10);
        }
    } catch(const std::exception& ex) {
        throw ex;
    }
    return data;
}

void _printFTPServerSetting(SCRSDK::CrFTPServerSetting* setting)
{
    #define _printUtf8(msg, dp) { \
        const char* _dp = dp; \
        std::cout << msg; \
        if(_dp) CrCout << Utf8ToCr(_dp); \
        std::cout << "\n"; \
    }

    printf("serverId:%d\n",                 setting->serverId);
    printf("serviceType:%d\n",              setting->serviceType);
    _printUtf8("displayName:",              setting->GetDisplayName());
    _printUtf8("hostName:",                 setting->GetHostName());
    printf("portNumber:%d\n",               setting->portNumber);
    _printUtf8("userName:",                 setting->GetUserName_());
    printf("passwordExists:%d\n",           setting->passwordExists);
    _printUtf8("password:",                 setting->GetPassword());
    printf("passiveMode:%d\n",              setting->passiveMode);
    _printUtf8("destinationDir:",           setting->GetDestinationDir());
    printf("secureProtocol:%d\n",           setting->secureProtocol);
    printf("directoryHierarchyType:%d\n",   setting->directoryHierarchyType);
    printf("overwriteType:%d\n",            setting->overwriteType);
    printf("rootCertificateErrorSetting:%d\n", setting->rootCertificateErrorSetting);
    return;
}

SCRSDK::CrError _setFTPServerSetting(int64_t device_handle, std::vector<CrString>& args)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;

    CrInt16u serverId = 0;
    std::string displayName;
    std::string hostName;
    CrInt16u portNumber = 0;
    std::string userName;
    SCRSDK::CrFTPServerPasswordExists               passwordExists = SCRSDK::CrFTPServerPassword_NotUse;
    std::string                                     password = "";
    SCRSDK::CrFTPServerPassiveMode                  passiveMode = SCRSDK::CrFTPServerPassiveMode_On;
    std::string                                     destinationDir = "";
    SCRSDK::CrFTPServerUsingSecureProtocol          secureProtocol = SCRSDK::CrFTPServerUsingSecureProtocol_Off;
    SCRSDK::CrFTPServerDirectoryHierarchyType       directoryHierarchyType = SCRSDK::CrFTPServerDirectoryHierarchyType_Standard;
    SCRSDK::CrFTPServerSameNameFileOverwriteType    overwriteType = SCRSDK::CrFTPServerSameNameFileOverwriteType_Overwrite;
    SCRSDK::CrFTPServerRootCertificateErrorSetting  rootCertificateErrorSetting = SCRSDK::CrFTPServerRootCertificateErrorSetting_NotConnect;

    SCRSDK::CrFTPServerSetting* setSetting = nullptr;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();

    try {
        if (args.size() < 6) GotoError("", 0);
        serverId = (CrInt16u)_stoll(args[1]);
        displayName = CrToUtf8(args[2]);
        hostName = CrToUtf8(args[3]);
        portNumber = (CrInt16u)_stoll(args[4]);
        userName = CrToUtf8(args[5]);
        if (args.size() >= 7) passwordExists                = (SCRSDK::CrFTPServerPasswordExists)_stoll(args[6]);
        if (args.size() >= 8) password                      = CrToUtf8(args[7]);
        if (args.size() >= 9) passiveMode                   = (SCRSDK::CrFTPServerPassiveMode)_stoll(args[8]);
        if (args.size() >= 10) destinationDir               = CrToUtf8(args[9]);
        if (args.size() >= 11) secureProtocol               = (SCRSDK::CrFTPServerUsingSecureProtocol)_stoll(args[10]);
        if (args.size() >= 12) directoryHierarchyType       = (SCRSDK::CrFTPServerDirectoryHierarchyType)_stoll(args[11]);
        if (args.size() >= 13) overwriteType                = (SCRSDK::CrFTPServerSameNameFileOverwriteType)_stoll(args[12]);
        if (args.size() >= 14) rootCertificateErrorSetting  = (SCRSDK::CrFTPServerRootCertificateErrorSetting)_stoll(args[13]);
    } catch(const std::exception&) { GotoError("", 0); }

    setSetting = new SCRSDK::CrFTPServerSetting(
                                serverId,
                                SCRSDK::CrFTPServerServiceType_FTP,
                                displayName.c_str(),
                                hostName.c_str(),
                                portNumber,
                                userName.c_str(),
                                passwordExists,
                                password.c_str(),
                                passiveMode,
                                destinationDir.c_str(),
                                secureProtocol,
                                directoryHierarchyType,
                                overwriteType,
                                rootCertificateErrorSetting);
    _printFTPServerSetting(setSetting);

    setEventPromise(&eventPromise);

    err = SCRSDK::SetFTPServerSetting(device_handle, setSetting);
    if(err) GotoError("", err);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);

    std::cout << "OK\n";
    result = 0;
Error:
    setEventPromise(nullptr);
    if(setSetting) delete setSetting;
    return result;
}

SCRSDK::CrError _getFTPServerSetting(int64_t device_handle, uint32_t index)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();

    // request FTPServerSettingList
    setEventPromise(&eventPromise);

    err = SCRSDK::RequestFTPServerSettingList(device_handle);
    if(err) GotoError("", err);

    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);

    // Get FTPServerSettingList
    {
        SCRSDK::CrFTPServerSetting* ftpServerSettingList = nullptr;
        CrInt32u ftpServerSettingListSize = 0;

        err = SCRSDK::GetFTPServerSettingList(device_handle, &ftpServerSettingList, &ftpServerSettingListSize);
        if(err || ftpServerSettingListSize == 0) GotoError("", err);

        printf("serverId=1~%d\n", (int)ftpServerSettingListSize);
        if (index > 0 && index <= ftpServerSettingListSize) {
            _printFTPServerSetting(ftpServerSettingList + (index-1));
        }
        SCRSDK::ReleaseFTPServerSettingList(device_handle, ftpServerSettingList);
    }

    result = 0;
Error:
    setEventPromise(nullptr);
    return result;
}


std::vector<CrString> CrSplit(const CrString &inputLine)
{
    std::vector<CrString> strArray;
    CrString tmp;
    bool inQuotes = false;

    for (size_t i = 0; i < inputLine.size(); ++i) {
        CrChar c = inputLine[i];

        if (c == CRSTR('"')) {
            inQuotes = !inQuotes;
        } else if (c == CRSTR(' ') && !inQuotes) {
            strArray.push_back(tmp);
            tmp.clear();
        } else {
            tmp += c;
        }
    }
    strArray.push_back(tmp);
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

  #if defined(_WIN32) || defined(_WIN64)
    _setmode(_fileno(stdin), _O_U16TEXT);
  #endif

    std::cout << "usage:\n";
    std::cout << "   set <id> <display> <host> <port> <user> [passExist] [pass] [passive] [dest] [secure] [hierarchy] [overwrite] [rootcert]\n";
    std::cout << "   get <id>\n";
    std::cout << "   result <slot>\n";
    std::cout << "To exit, please enter 'q'.\n";

    while(1) {
        CrString inputLine;
        std::getline(CrCin, inputLine);
        std::vector<CrString> args = CrSplit(inputLine);

        if((args[0] == CRSTR("set") || args[0] == CRSTR("s")) && args.size() >= 6) {
            err = _setFTPServerSetting(m_device_handle, args);
            if(err) goto Error;

        } else if((args[0] == CRSTR("get") || args[0] == CRSTR("g")) && args.size() >= 2) {
            uint32_t id;
            try { id = stoi(args[1]); } catch(const std::exception&) { GotoError("", 0); }
            err = _getFTPServerSetting(m_device_handle, id);
            if(err) goto Error;

        } else if(args[0] == CRSTR("result") && args.size() >= 2) {
            uint32_t slot;
            try { slot = stoi(args[1]); } catch(const std::exception&) { GotoError("", 0); }
            err = SCRSDK::RequestFTPTransferResult(m_device_handle, (SCRSDK::CrSlotNumber)slot);
            if(err) goto Error;

        } else if(args[0] == CRSTR("q") || args[0] == CRSTR("Q")) {
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
