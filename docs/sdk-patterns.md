# Sony Camera Remote SDK patterns

Notes on the CRSDK calls `simpleCli/app/fx30MultiRecord.cpp` relies on.

### Connection
- `SCRSDK::Init()` / `SCRSDK::Release()` — global init/cleanup
- `SCRSDK::EnumCameraObjects(&enumInfo, timeoutSec)` — discover cameras
- `SCRSDK::Connect(objInfo, callback, &handle, mode, reconnect)` — connect
  - Modes: `CrSdkControlMode_Remote`, `CrSdkControlMode_ContentsTransfer`
  - Reconnect: `CrReconnecting_ON`
- `SCRSDK::Disconnect(handle)` then `SCRSDK::ReleaseDevice(handle)`
- Use promise/future in IDeviceCallback for async connect/disconnect

### USB Reset (macOS)
- Camera hangs from previous process — MUST reset USB on startup
- `resetUSBDevice(0x054c, 0x0e10)` via IOKit, then sleep 6s
- After reset+connect, camera may briefly enter reconnecting state — wait 15s before monitoring

### Connection State
- `IDeviceCallback::OnConnected` → `m_connected = true`
- `IDeviceCallback::OnDisconnected` → `m_connected = false`
- `IDeviceCallback::OnWarning(CrWarning_Connect_Reconnecting)` → SDK auto-reconnects, don't tear down
- Only do full reset after camera is `m_connected == false` for ~20 seconds

### Properties
- `GetSelectDeviceProperties(handle, count, codes[], &props, &nprop)` — batch read
- `ReleaseDeviceProperties(handle, props)` — must release after read
- `CrDeviceProperty.GetCode()`, `.GetCurrentValue()`, `.GetCurrentStr()` (UTF-16 for strings)
- String properties (e.g. RecorderClipName): `GetCurrentStr()` returns `CrInt16u*` with leading control char (0x0F) — strip chars < 0x20

### Setting Properties
- Read first: `GetSelectDeviceProperties` to get full CrDeviceProperty struct
- Set: `devProp.SetCurrentValue(newVal)` then `SetDeviceProperty(handle, &devProp)`
- Wait ~500ms between sets for camera to process
- Settable: ISO, ShutterSpeed, FNumber, WhiteBalance, Colortemp, Movie_File_Format, Movie_Recording_Setting, Movie_Recording_FrameRateSetting

### Commands
- `SendCommand(handle, commandId, param)` — e.g. MovieRecord
- `CrCommandId_MovieRecord` + `CrCommandParam_Down` = start, `CrCommandParam_Up` = stop

### Control Codes
- `ExecuteControlCodeValue(handle, code, value)` — e.g. media format
- `CrControlCode_SelectedMediaFormat` + `CrMediaFormat_QuickFormatSlot1`

### Key Property Codes
- BatteryRemain, IsoSensitivity, ShutterSpeed, FNumber, WhiteBalance, Colortemp
- RecordingState (`CrMovie_Recording_State_Recording`)
- MediaSLOT1_RemainingTime, MediaSLOT2_RemainingTime
- Movie_File_Format, Movie_Recording_Setting, Movie_Recording_FrameRateSetting
- RecorderClipName (string, has leading control char)
- DeviceOverheatingState (0=ok, 1=pre, 2=overheat)

### Value Formats
| Property | Encoding |
|----------|----------|
| ISO | Bits 0-23 = ISO value, bits 24-27 = mode (0=manual, nonzero=auto) |
| Shutter Speed | HIWORD = numerator, LOWORD = denominator (e.g. `0x0001003C` = 1/60) |
| F-Number | Value / 100 (e.g. 280 = F2.8) |

### Contents Transfer (File Download)
1. Disconnect from Remote mode
2. `EnumCameraObjects` → Connect in `CrSdkControlMode_ContentsTransfer`
3. Wait for `ContentsTransferStatus == CrContentsTransfer_ON`
4. `SetSaveInfo(handle, path, prefix, startNo)`
5. `GetDateFolderList` → `GetContentsHandleList` → `GetContentsDetailInfo` → `PullContentsFile`
6. `OnNotifyContentsTransfer` callback: `CrNotify_ContentsTransfer_Complete`
7. `ReleaseDateFolderList`, `ReleaseContentsHandleList`
8. Disconnect, reconnect in Remote mode

### JSON Output
- Must escape ALL control chars (< 0x20) as `\u00xx` — SDK strings contain them
