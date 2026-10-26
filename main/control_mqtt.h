#ifndef CONTROL_MQTT_H
#define CONTROL_MQTT_H

#include <mqtt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// Second MQTT broker — pure control-plane, no UDP audio, no xiaozhi.me session.
// All device mutations are dispatched through Application::Schedule() for thread safety.
// The initial (blocking) connect runs in its own task; afterwards esp-mqtt's
// built-in auto-reconnect keeps the link alive, so no manual retry timer is needed.
class ControlMqtt {
public:
    ControlMqtt();
    ~ControlMqtt();

    // Start: connect to the control broker (non-blocking), subscribe to cmd topic, send hello.
    // Returns true on success (or if no config was found — silently skips).
    bool Start();

    // Stop: send goodbye, disconnect and release the client. Safe to call when not started.
    void Stop();

    // Broadcast device state change to the control broker (optional session_update mirror).
    void BroadcastState(const std::string& state_name);

    // Broadcast a local user action (best effort, QoS 0). Empty value is omitted.
    void BroadcastEvent(const std::string& event, const std::string& value,
                        const std::string& state);

    // Returns true if a control broker is configured in NVS.
    bool IsConfigured() const { return !subscribe_topic_.empty(); }

    bool IsConnected() const { return connected_; }

    // Called when the user stops a broadcast from the device itself (screen
    // double-click / IMU gesture). Lets notify_result report outcome=user_stopped
    // instead of "finished", which is what a plain transition to idle would produce.
    void MarkNotificationUserStopped() { user_stopped_ = true; }

private:
    // Read control_mqtt namespace from NVS; returns false if no endpoint configured.
    bool LoadConfig();

    // Build a JSON payload and publish to the given topic.
    bool PublishJson(const std::string& topic, cJSON* root);

    // --- Connection management ---
    // Blocking connect runs in a dedicated task so the caller (activation/main task)
    // is never stalled by the MQTT connect timeout.
    void StartConnectTask();
    static void ConnectTaskEntry(void* arg);
    void ConnectTask();

    // Must be called with lifecycle_mutex_ held.
    void DisconnectLocked();

    std::unique_ptr<Mqtt> mqtt_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> connect_task_running_{false};
    // Only one image download/decode at a time: each one can hold megabytes of PSRAM.
    std::atomic<bool> image_fetch_running_{false};
    std::mutex lifecycle_mutex_;
    std::shared_ptr<std::atomic<bool>> alive_;

    // --- Config (populated by LoadConfig) ---
    std::string broker_endpoint_;
    std::string broker_client_id_;
    std::string broker_username_;
    std::string broker_password_;
    std::string subscribe_topic_;  // "xiaozhi/{device_id}/cmd"
    std::string reply_topic_;      // "xiaozhi/{device_id}/reply"
    std::string device_id_;

    // Monotonic sequence for user-action events (main task only).
    uint32_t event_seq_ = 0;

    // --- Notification bookkeeping (main task only) ---
    // Last state seen through BroadcastState(), used to detect leaving "notifying".
    std::string last_state_;
    // Id echoed from the notify command so the server can correlate notify_result.
    std::string notify_id_;
    uint32_t notify_start_ms_ = 0;
    bool notify_active_ = false;
    // Set by abort_notify: a cancelled notification leaves the notifying state
    // exactly like a finished one, so the flag is needed to tell them apart.
    bool cancel_requested_ = false;
    // Set when the user stops the broadcast locally (screen double-click, BOOT key,
    // IMU gesture). cancel_requested_ is checked first, so a remote listen/abort that
    // also stops playback is never misreported as an on-device gesture.
    bool user_stopped_ = false;

    // --- Message dispatch ---
    void OnMessage(const std::string& topic, const std::string& payload);
    void DispatchCommand(cJSON* root);

    // --- Individual command handlers ---
    // All run inside Application::Schedule() (main task).
    void HandleListen(cJSON* root);
    void HandleAbort(cJSON* root);
    void HandleMcp(cJSON* root);
    void HandleSystem(cJSON* root);
    void HandleSetImage(cJSON* root);
    void HandleSetTheme(cJSON* root);
    void HandleSetVolume(cJSON* root);
    void HandleSetBrightness(cJSON* root);
    void HandleToggleWakeWord(cJSON* root);
    void HandleNotify(cJSON* root);
    void HandleAbortNotify(cJSON* root);
    void HandleSetStatus(cJSON* root);
    void HandleTogglePowerSave(cJSON* root);
    void HandleSetImu(cJSON* root);
    void HandleQueryStatus(cJSON* root);

    // Image download runs off the main task; the decoded image is applied on it.
    static void ImageFetchTaskEntry(void* arg);

    // Send a hello/goodbye/session_update broadcast.
    void SendHello();
    void SendGoodbye();
    void SendSessionUpdate(const std::string& state_name);

    // Send a notify_result broadcast when a notification leaves the notifying state.
    void SendNotifyResult(const std::string& id, const std::string& outcome, uint32_t duration_ms);

    // Track the notification lifecycle from state changes (main task only).
    void TrackNotificationState(const std::string& state_name);

    // Reply to a cmd message on the reply topic.
    void SendReply(const std::string& cmd_type, bool success,
                   const std::string& reply_topic_override = "", cJSON* extra = nullptr,
                   const std::string& id = "");
};

#endif  // CONTROL_MQTT_H
