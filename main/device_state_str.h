#ifndef _DEVICE_STATE_STR_H_
#define _DEVICE_STATE_STR_H_

#include "device_state.h"

// 设备状态的稳定字符串形式，供控制通道与日志使用。
// 放在 fork 自有头文件里，避免修改上游的 device_state.h。
inline const char* DeviceStateToString(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown: return "unknown";
        case kDeviceStateStarting: return "starting";
        case kDeviceStateWifiConfiguring: return "wifi_configuring";
        case kDeviceStateIdle: return "idle";
        case kDeviceStateConnecting: return "connecting";
        case kDeviceStateListening: return "listening";
        case kDeviceStateSpeaking: return "speaking";
        case kDeviceStateNotifying: return "notifying";
        case kDeviceStateUpgrading: return "upgrading";
        case kDeviceStateActivating: return "activating";
        case kDeviceStateAudioTesting: return "audio_testing";
        case kDeviceStateFatalError: return "fatal_error";
    }
    return "unknown";
}

#endif  // _DEVICE_STATE_STR_H_
