// "pull/delete/playback contents in RemoteTransferMode" sample
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
uint32_t m_postview_size = 0;
CrString m_postview_filename = CRSTR("");

std::mutex m_eventPromiseMutex;
uint32_t m_setDPCode = 0;
std::promise<void>* m_eventPromise = nullptr;
void setEventPromise(std::promise<void>* dp)
{
    std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
    m_eventPromise = dp;
}

std::ofstream* m_fileVideo = nullptr;
std::ofstream* m_fileAudio = nullptr;

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

    void OnNotifyPostViewImage(CrChar* filename, CrInt32u size)
    {
        CrCout << "OnNotifyPostViewImage:" << filename << "," << size << "\n";
        m_postview_size = size;
        m_postview_filename = filename;
    }

    void OnNotifyContentsTransfer(CrInt32u notify, SCRSDK::CrContentHandle contentHandle, CrChar* filename)
    {
        std::cout << "OnNotifyContentsTransfer:" << CrErrorString(notify) << "\n";
    }
    
    void OnNotifyRemoteTransferResult(CrInt32u notify, CrInt32u per, CrChar* filename)
    {
        std::cout << "OnNotifyRemoteTransferResult:" << CrErrorString(notify) << "\n";
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        switch(notify) {
        case SCRSDK::CrNotify_RemoteTransfer_Result_OK:
            CrCout << filename << "\n";
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;
        case SCRSDK::CrNotify_RemoteTransfer_Result_NG:
        case SCRSDK::CrNotify_RemoteTransfer_Result_DeviceBusy:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
        }
    }

    void OnNotifyRemoteTransferResult(CrInt32u notify, CrInt32u per, CrInt8u* data, CrInt64u size)
    {
        std::cout << "OnNotifyRemoteTransferResult xx:" << CrErrorString(notify) << "\n";
        std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
        if(m_eventPromise) {
            m_eventPromise->set_value();
            m_eventPromise = nullptr;
        }
    }

    void OnNotifyRemoteTransferContentsListChanged(CrInt32u notify, CrInt32u slotNumber, CrInt32u addSize)
    {
        std::cout << "OnNotifyRemoteTransferContentsListChanged:" << CrErrorString(notify) << "\n";
    }

    void OnReceivePlaybackData(CrInt8u mediaType, CrInt32 dataSize, CrInt8u* data, CrInt64 pts, CrInt64 dts, CrInt32 param1, CrInt32 param2)
    {
    //  std::cout << "OnReceivePlaybackData:\n";
        switch(mediaType) {
        case SCRSDK::CrMoviePlaybackDataType_Video:
            if(m_fileVideo) m_fileVideo->write((char*)data, dataSize);
            break;
        case SCRSDK::CrMoviePlaybackDataType_Audio:
            if(m_fileAudio) m_fileAudio->write((char*)data, dataSize);
            break;
        }
    }
    void OnReceivePlaybackTimeCode(CrInt32u timeCode)
    {
    //  printf("OnReceivePlaybackTimeCode:%d\n", timeCode);
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
        case SCRSDK::CrNotify_Playback_Result_StopComplete:
            if(m_fileVideo) {
                m_fileVideo->close();
                m_fileVideo = nullptr;
            }
            if(m_fileAudio) {
                m_fileAudio->close();
                m_fileAudio = nullptr;
            }
            [[fallthrough]];
        case SCRSDK::CrNotify_Playback_StatusChanged:
            if(m_eventPromise) {
                m_eventPromise->set_value();
                m_eventPromise = nullptr;
            }
            break;

        case SCRSDK::CrNotify_Playback_Result_NormalTermination:
            SCRSDK::ControlMoviePlayback(m_device_handle, SCRSDK::CrMoviePlaybackControlType_Stop, 0);
            break;

        case SCRSDK::CrWarning_Playback_Result_Invalid:
        case SCRSDK::CrWarning_Playback_Result_CameraOperateTermination:
        case SCRSDK::CrWarning_Playback_Result_SystemError:
        case SCRSDK::CrWarning_Playback_Result_HighTemperature:
        case SCRSDK::CrWarning_Playback_Result_MediaRemoval:
        case SCRSDK::CrWarning_Playback_Result_ContentsError:
        case SCRSDK::CrWarning_Playback_Result_KeepAliveTimeout:
        case SCRSDK::CrWarning_Playback_Result_Start_Fail:
        case SCRSDK::CrWarning_Playback_Result_Stop_Fail:
        case SCRSDK::CrWarning_Playback_Result_Play_Fail:
        case SCRSDK::CrWarning_Playback_Result_Pause_Fail:
            if(m_eventPromise) {
                m_eventPromise->set_exception(std::make_exception_ptr(std::runtime_error("error")));
                m_eventPromise = nullptr;
            }
            break;
/*
        case SCRSDK::CrNotify_Playback_Result_PlaybackInfo_Success:
        case SCRSDK::CrWarning_Playback_Result_PlaybackInfo_Error:
*/
        }
    }

    void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3)
    {
        std::cout << "OnWarningExt:" << CrWarningExtString(warning, param1, param2, param3).c_str() << "\n";
        if(warning == SCRSDK::CrWarningExt_DeleteContent) {
            std::lock_guard<std::mutex> lock(m_eventPromiseMutex);
            switch(param1) {
            case SCRSDK::CrWarningExtParam_DeleteContentResult_OK:
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
        }
    }
    void OnLvPropertyChanged() {}
    void OnLvPropertyChangedCodes(CrInt32u num, CrInt32u* codes) {}
    void OnPropertyChanged() {}
    void OnPropertyChangedCodes(CrInt32u num, CrInt32u* codes)
    {
        for(uint32_t i = 0; i < num; ++i) {
            if(codes[i] == SCRSDK::CrDeviceProperty_PullPostViewImageStatus) {
                SCRSDK::CrDeviceProperty devProp;
                SCRSDK::CrError err = _getDeviceProperty(m_device_handle, SCRSDK::CrDeviceProperty_PullPostViewImageStatus, &devProp);
                if(!err) {
                    std::cout << "OnPropertyChangedCodes:PullPostViewImageStatus=" << (devProp.GetCurrentValue() ? "Exists" : "Not Exists") << "\n";
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

SCRSDK::CrError _shooting(int64_t device_handle)
{
    SCRSDK::CrError err = 0;

    err = SCRSDK::SendCommand(device_handle, SCRSDK::CrCommandId_Release, SCRSDK::CrCommandParam_Down); if (err) GotoError("", err);
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    err = SCRSDK::SendCommand(device_handle, SCRSDK::CrCommandId_Release, SCRSDK::CrCommandParam_Up); if (err) GotoError("", err);

    std::cout << "OK\n";
Error:
    return err;
}

void _showContentsInfo(SCRSDK::CrContentsInfo& contentsInfo)
{
    std::cout << "\n";
    std::cout << "[ContentsInfo]\n";
    printf("contentType = %d\n", contentsInfo.contentType);
    printf("contentId = %u\n", contentsInfo.contentId);
    printf("dirNumber = %u\n", contentsInfo.dirNumber);
    printf("fileNumber = %u\n", contentsInfo.fileNumber);
    printf("groupType = %d\n", contentsInfo.groupType);
    printf("representative = %u\n", contentsInfo.representative);
    printf("creationDatetimeUTC = %04d/%02d/%02d %02d:%02d:%02d.%03d\n",
            contentsInfo.creationDatetimeUTC.year, 
            contentsInfo.creationDatetimeUTC.month, 
            contentsInfo.creationDatetimeUTC.day,
            contentsInfo.creationDatetimeUTC.hour, 
            contentsInfo.creationDatetimeUTC.minute,
            contentsInfo.creationDatetimeUTC.sec, 
            contentsInfo.creationDatetimeUTC.msec);
    printf("modificationDatetimeUTC = %04d/%02d/%02d %02d:%02d:%02d.%03d\n",
            contentsInfo.modificationDatetimeUTC.year, 
            contentsInfo.modificationDatetimeUTC.month, 
            contentsInfo.modificationDatetimeUTC.day,
            contentsInfo.modificationDatetimeUTC.hour, 
            contentsInfo.modificationDatetimeUTC.minute,
            contentsInfo.modificationDatetimeUTC.sec, 
            contentsInfo.modificationDatetimeUTC.msec);
    printf("creationDatetimeLocaltime = %04d/%02d/%02d %02d:%02d:%02d.%03d\n",
            contentsInfo.creationDatetimeLocaltime.year, 
            contentsInfo.creationDatetimeLocaltime.month, 
            contentsInfo.creationDatetimeLocaltime.day,
            contentsInfo.creationDatetimeLocaltime.hour, 
            contentsInfo.creationDatetimeLocaltime.minute,
            contentsInfo.creationDatetimeLocaltime.sec, 
            contentsInfo.creationDatetimeLocaltime.msec);
    printf("modificationDatetimeLocaltime = %04d/%02d/%02d %02d:%02d:%02d.%03d\n",
            contentsInfo.modificationDatetimeLocaltime.year, 
            contentsInfo.modificationDatetimeLocaltime.month, 
            contentsInfo.modificationDatetimeLocaltime.day,
            contentsInfo.modificationDatetimeLocaltime.hour, 
            contentsInfo.modificationDatetimeLocaltime.minute,
            contentsInfo.modificationDatetimeLocaltime.sec, 
            contentsInfo.modificationDatetimeLocaltime.msec);
    printf("rating = %d\n", contentsInfo.rating);
    printf("protectionStatus = %u\n", contentsInfo.protectionStatus);
    printf("dummyContent = %u\n", contentsInfo.dummyContent);
    printf("shotMarkNum = %u\n", contentsInfo.shotMarkNum);
    for(CrInt32u i = 0; i < contentsInfo.shotMarkNum; i++) {
        printf("No:%d, shotMark = %u\n",(i+1), contentsInfo.shotMark[i]);
    }
    printf("filesNum = %u\n", contentsInfo.filesNum);
}

void _showContentsFile(SCRSDK::CrContentsFile& contentsFile)
{
    printf("fileId = %u\n", contentsFile.fileId);
    printf("filePathLength = %u\n", contentsFile.filePathLength);
    if( contentsFile.filePathLength > 0 ) {
        printf("filePath = %s\n", contentsFile.filePath);
    }
    printf("fileFormat = %u\n", contentsFile.fileFormat);
    printf("fileSize = %d\n", (int)contentsFile.fileSize);
    std::cout << "umid = 0x";
    for(int j = 0; j < sizeof(contentsFile.umid); j++) {
        printf("%02x", contentsFile.umid[j]);
    }
    std::cout << "\n";

    printf("isImageParamExsist = %u\n", contentsFile.isImageParamExsist);
    if( contentsFile.isImageParamExsist == true ) {
        printf("imagePixWidth = %u\n", contentsFile.imageParam.imagePixWidth);
        printf("imagePixHeight = %u\n", contentsFile.imageParam.imagePixHeight);
    }

    printf("isVideoParamExsist = %u\n", contentsFile.isVideoParamExsist);
    if( contentsFile.isVideoParamExsist == true ) {
        printf("startTimeCode = %u\n", contentsFile.videoParam.startTimeCode);
        printf("endTimeCode = %u\n", contentsFile.videoParam.endTimeCode);
        printf("videoCodec = %d\n", contentsFile.videoParam.videoCodec);
        printf("proxyStatus = %u\n", contentsFile.videoParam.proxyStatus);
        printf("gopStructure = %d\n", contentsFile.videoParam.gopStructure);
        printf("width = %u\n", contentsFile.videoParam.width);
        printf("height = %u\n", contentsFile.videoParam.height);
        printf("aspectRatio = %d\n", contentsFile.videoParam.aspectRatio);
        printf("colorFormat = %d\n", contentsFile.videoParam.colorFormat);
        printf("imageBitDepth = %u\n", contentsFile.videoParam.imageBitDepth);
        printf("framesPerThousandSeconds = %u\n", contentsFile.videoParam.framesPerThousandSeconds);
        printf("scanType = %d\n", contentsFile.videoParam.scanType);
        printf("bitrateMbps = %u\n", contentsFile.videoParam.bitrateMbps);
        printf("imageFramesPerThousandSeconds = %u\n", contentsFile.videoParam.imageFramesPerThousandSeconds);
        printf("profileIndication = %d\n", contentsFile.videoParam.profileIndication);
        printf("profileLevel = %u\n", contentsFile.videoParam.profileLevel);
        printf("rdd18metaCaptureGammaEquation = %d\n", contentsFile.videoParam.rdd18metaCaptureGammaEquation);
        printf("rdd18metaColorPrimaries = %d\n", contentsFile.videoParam.rdd18metaColorPrimaries);
        printf("rdd18metaCodingEquations = %d\n", contentsFile.videoParam.rdd18metaCodingEquations);
    }

    printf("isAudioParamExsist = %u\n", contentsFile.isAudioParamExsist);
    if( contentsFile.isAudioParamExsist == true ) {
        printf("audioCodec = %d\n", contentsFile.audioParam.audioCodec);
        printf("audioBitDepth = %u\n", contentsFile.audioParam.audioBitDepth);
        printf("samplingRate = %u\n", contentsFile.audioParam.samplingRate);
        printf("numberOfChannels = %d\n", contentsFile.audioParam.numberOfChannels);
    }
    std::cout << "\n";
}

SCRSDK::CrError _selectContent(int64_t device_handle, SCRSDK::CrSlotNumber slotNumber, CrInt32u& contentId, CrInt32u& fileId)
{
    int result = SCRSDK::CrError_Generic_Unknown;
    SCRSDK::CrError err = SCRSDK::CrError_None;
    int index = 0;

    SCRSDK::CrCaptureDate* m_captureDateList = nullptr;
    SCRSDK::CrContentsInfo* m_contentsInfoList = nullptr;
    std::string inputLine;
    CrInt32u outNums = 0;

    SCRSDK::CrContentsInfo contentsInfo;
    SCRSDK::CrContentsFile contentsFile;

    // select date
    err = SCRSDK::GetRemoteTransferCapturedDateList(device_handle, slotNumber, &m_captureDateList, &outNums);
    if(err || outNums == 0) GotoError("", err);

    for(CrInt32u i = 0; i < outNums; i++) {
        printf("[%d] %04d/%02d/%02d\n",(int)i, m_captureDateList[i].year, m_captureDateList[i].month, m_captureDateList[i].day);
    }

    std::cout << "date(0~ ):"; std::getline(std::cin, inputLine);
    try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
    if(index < 0 || index >= (int)outNums) GotoError("", 0);

    // select content
    err = SCRSDK::GetRemoteTransferContentsInfoList(device_handle, slotNumber, SCRSDK::CrGetContentsInfoListType_Range_Day,
                                                    &m_captureDateList[index], /*maxNums*/0,
                                                    &m_contentsInfoList, &outNums);
    if(err) GotoError("", err);

    for(CrInt32u i = 0;i < outNums; i++) {
        printf("[%d] %04d/%02d/%02d %02d:%02d:%02d, FileName:%s\n", i,
            m_contentsInfoList[i].creationDatetimeLocaltime.year,
            m_contentsInfoList[i].creationDatetimeLocaltime.month,
            m_contentsInfoList[i].creationDatetimeLocaltime.day,
            m_contentsInfoList[i].creationDatetimeLocaltime.hour,
            m_contentsInfoList[i].creationDatetimeLocaltime.minute,
            m_contentsInfoList[i].creationDatetimeLocaltime.sec,
            m_contentsInfoList[i].files[0].filePath);
    }

    std::cout << "index(0~ ):"; std::getline(std::cin, inputLine);
    try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
    if(index < 0 || index >= (int)outNums) GotoError("", 0);

    contentsInfo = m_contentsInfoList[index];

    // select file
    if (contentsInfo.filesNum > 1) {
        for(CrInt32u i = 0; i < contentsInfo.filesNum; i++) {
            printf("[%d] FileId:%d, FileName:%s\n", i,
                contentsInfo.files[i].fileId,
                contentsInfo.files[i].filePath);
        }
        std::cout << "index(0~ ):"; std::getline(std::cin, inputLine);
        try { index = stoi(inputLine); } catch(const std::exception&) { GotoError("", 0); }
        if(index < 0 || index >= (int)contentsInfo.filesNum) GotoError("", 0);
    } else {
        index = 0;
    }
    contentsFile = contentsInfo.files[index];

    _showContentsInfo(contentsInfo);
    _showContentsFile(contentsFile);

    contentId = contentsInfo.contentId;
    fileId = contentsFile.fileId;

    result = 0;
Error:
    if (m_contentsInfoList) SCRSDK::ReleaseRemoteTransferContentsInfoList(device_handle, m_contentsInfoList);
    if (m_captureDateList) SCRSDK::ReleaseRemoteTransferCapturedDateList(device_handle, m_captureDateList);
    return result;
}

SCRSDK::CrError _controlMoviePlayback(int64_t device_handle, SCRSDK::CrMoviePlaybackControlType operation)
{
    SCRSDK::CrError err = SCRSDK::CrError_None;
/*
    CrMoviePlaybackControlType_Start = 0x00000001,
    CrMoviePlaybackControlType_Stop,
    CrMoviePlaybackControlType_Play = 0x00000004,
    CrMoviePlaybackControlType_Pause,
    CrMoviePlaybackControlType_Seek,
*/
    std::promise<void> eventPromise;
    std::future<void> eventFuture = eventPromise.get_future();
    std::future_status status;

    setEventPromise(&eventPromise);
    err = SCRSDK::ControlMoviePlayback(device_handle, operation, 0);
    if(err) GotoError("", err);

    status = eventFuture.wait_for(std::chrono::milliseconds(10000));
    if(status != std::future_status::ready) GotoError("timeout",0);
    try{
        eventFuture.get();
    } catch(const std::exception&) GotoError("", 0);
    return 0;
Error:
    setEventPromise(nullptr);
    return SCRSDK::CrError_Generic_Unknown;
}

SCRSDK::CrError _playbackStart(int64_t device_handle, SCRSDK::CrSlotNumber slotNumber, CrInt32u contentId, CrInt32u fileId, std::string ipAddress, CrString path)
{
    SCRSDK::CrError err = SCRSDK::CrError_None;
    SCRSDK::CrMoviePlaybackSetting setting;

    if(contentId == 0) {
        std::cout << "select content\n";
        return SCRSDK::CrError_Generic_Unknown;
    }
    if(ipAddress == "") {
        std::cout << "enter ip address of this pc\n";
        return SCRSDK::CrError_Generic_Unknown;
    }

    setting.slotId = slotNumber;
    setting.contentsId = contentId;
    setting.fileId = fileId;
    setting.SetIpAddress(ipAddress.c_str());

    err = SCRSDK::SetMoviePlaybackSetting(device_handle, &setting, 1);
    if(err) GotoError("", err);

    err = _controlMoviePlayback(device_handle, SCRSDK::CrMoviePlaybackControlType_Start);
    if(err) goto Error;

    m_fileVideo = new std::ofstream(path + CrString(DELIMITER CRSTR("temp.hevc")), std::ios::out | std::ios::binary);
    if (m_fileVideo->bad()) GotoError("", 0);

    m_fileAudio = new std::ofstream(path + CrString(DELIMITER CRSTR("temp.aac")), std::ios::out | std::ios::binary);
    if (m_fileAudio->bad()) GotoError("", 0);
    CrCout << "write files to:" << path.data() << "\n" "Please convert files with \"ffmpeg -i temp.hevc -i temp.aac -c copy output.mp4\"\n";

    err = _controlMoviePlayback(device_handle, SCRSDK::CrMoviePlaybackControlType_Play);
    if(err) goto Error;
    return 0;
Error:
    if(m_fileVideo) {
        m_fileVideo->close();
        m_fileVideo = nullptr;
    }
    if(m_fileAudio) {
        m_fileAudio->close();
        m_fileAudio = nullptr;
    }
    return SCRSDK::CrError_Generic_Unknown;
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
            SCRSDK::CrSdkControlMode_RemoteTransfer,
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

    #define _printUsage() { \
        std::cout << "usage:\n"; \
        std::cout << "  s [1~2(slot)]                             - Select content\n"; \
        std::cout << "  g [0(content),1(thumbnail),2(screennail)] - Get content\n"; \
        std::cout << "  d                                         - Delete content\n"; \
        std::cout << "  ip <192.168.1.2(ip of this PC)>           - set ip of this PC\n"; \
        std::cout << "  p [1(start),2(stop),4(resume),5(pause),6(seek)] - Playback content\n"; \
        std::cout << "\n"; \
        std::cout << "  shot                                      - Shutter Release\n"; \
        std::cout << "  postview <1(enable),0(disable)> <0(legacy),0x8000(file),0x8001(ram)>\n"; \
        std::cout << "  pull\n"; \
        std::cout << "To exit, please enter 'q'.\n"; \
    }
    _printUsage();

    {
        SCRSDK::CrSlotNumber slotNumber = SCRSDK::CrSlotNumber_Slot1;
        CrInt32u contentId = 0;
        CrInt32u fileId = 0;
        std::string ipAddress;

        while(1) {
            std::string inputLine;
            std::cout << "cmd:"; std::getline(std::cin, inputLine);
            std::vector<std::string> args = _split(inputLine, ' ');
            int args2 = 0;
            if(args.size() >= 2 && args[0] != "ip") {
                try { args2 = stol(args[1]); } catch(const std::exception&) { GotoError("", 0); }
            }

            if(args.size() == 0) {
                
            } else if(args[0] == "s") {  // select content
                if(args2 != 0) slotNumber = (SCRSDK::CrSlotNumber)args2;
                err = _selectContent(m_device_handle, slotNumber, contentId, fileId);
                //if(err) goto Error;

            } else if(args[0] == "d" || args[0] == "g") { // delete/get content
                std::promise<void> eventPromise;
                std::future<void> eventFuture = eventPromise.get_future();

                if(contentId == 0) {
                    std::cout << "select media\n";
                    continue;
                }

                #if defined(__linux__)
                  #define DivisionSize 0x1000000 // 16MB (16 * 1024 * 1024)
                #else
                  #define DivisionSize 0x5000000 // 80MB (80 * 1024 * 1024)
                #endif
                setEventPromise(&eventPromise);
                if(args[0] == "d") {
                    err = SCRSDK::DeleteRemoteTransferContentsFile(
                                    m_device_handle, slotNumber, contentId);
                    contentId = 0;
                } else if(args2 == 0) {
                    err = SCRSDK::GetRemoteTransferContentsDataFile(
                                    m_device_handle, slotNumber, contentId, fileId,
                                    DivisionSize, nullptr, nullptr);
                } else if(args2 == 1) {
                    err = SCRSDK::GetRemoteTransferContentsCompressedDataFile(
                                    m_device_handle, slotNumber, contentId, fileId,
                                    SCRSDK::CrGetContentsCompressedDataType_Thumbnail, nullptr, nullptr);
                } else if(args2 == 2) {
                    err = SCRSDK::GetRemoteTransferContentsCompressedDataFile(
                                    m_device_handle, slotNumber, contentId, fileId,
                                    SCRSDK::CrGetContentsCompressedDataType_Screennail, nullptr, nullptr);
                } else {
                    GotoError("", 0);
                }
                if(err) GotoError("", err);

                try{
                    eventFuture.get();
                } catch(const std::exception&) GotoError("", 0);
                std::cout << "OK\n";

            } else if(args[0] == "ip") {
                if(args.size() >= 2) {
                    ipAddress = args[1];
                }
            } else if(args[0] == "p") { // playback content
                if(args2 == 0 || args2 == 1) { /*start*/
                    err = _playbackStart(m_device_handle, slotNumber, contentId, fileId, ipAddress, path);
                } else {
                    err = _controlMoviePlayback(m_device_handle, (SCRSDK::CrMoviePlaybackControlType)args2);
                }
                //if(err) goto Error;

            } else if (args[0] == "shot" || args[0] == "SHOT") { // Shutter Release
                err = _shooting(m_device_handle);
                //if (err) goto Error;

                // ffmpeg -i temp.hevc -i temp.aac -c copy output.mp4
            } else if(args[0] == "postview" && args.size() >= 3) {
                int data = 0;
                try { data = (int)_stoll(args[1]); } catch(const std::exception&) { GotoError("", 0); }
                err = SCRSDK::SetDeviceSetting(m_device_handle, SCRSDK::Setting_Key_EnablePostView, data);
                if(err) GotoError("", err)

                try { data = (int)_stoll(args[2]); } catch(const std::exception&) { GotoError("", 0); }
                err = SCRSDK::SetDeviceSetting(m_device_handle, SCRSDK::Setting_Key_PostViewTransferringType, data);
                if(err) GotoError("", err)
            } else if(args[0] == "pull") {
                if(m_postview_size) {
                    uint32_t size = m_postview_size;
                    CrString filename = path + DELIMITER + m_postview_filename;

                    uint8_t* buf = (uint8_t*)malloc(size);
                    if(!buf) GotoError("", 0);

                    m_postview_size = 0;
                    m_postview_filename = CRSTR("");

                    err = SCRSDK::PullPostViewImage(m_device_handle, buf, size);
                    if(err) GotoError("", err)

                    std::ofstream file(filename, std::ios::out | std::ios::binary);
                    if (file.bad()) GotoError("", 0);
                    file.write((char*)buf, size);
                    file.close();
                    CrCout << filename.data() << "," << size << "\n";

                    free(buf);
                }
            } else if(args[0] == "q" || args[0] == "Q") {
                break;
            } else {
                _printUsage();
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
    if(m_device_handle) SCRSDK::ReleaseDevice(m_device_handle);
    SCRSDK::Release();

    if(m_fileVideo) {
        m_fileVideo->close();
        m_fileVideo = nullptr;
    }
    if(m_fileAudio) {
        m_fileAudio->close();
        m_fileAudio = nullptr;
    }

    return result;
}
