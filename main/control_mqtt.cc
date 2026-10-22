#include "control_mqtt.h"

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "backlight.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "http.h"
#include "notify/notify_player.h"
#include "settings.h"
#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

#define TAG "ControlMqtt"

// Upper bound for a remote image download; anything bigger is rejected up front.
static constexpr size_t kMaxImageBytes = 4 * 1024 * 1024;
// Give esp-mqtt a brief moment to flush the goodbye before tearing the client down.
static constexpr int kGoodbyeFlushDelayMs = 50;

ControlMqtt::ControlMqtt()
    : alive_(std::make_shared<std::atomic<bool>>(true)) {
}

ControlMqtt::~ControlMqtt() {
    *alive_ = false;
    enabled_ = false;

    // A connect attempt may still be blocked inside Mqtt::Connect (its own timeout).
    // Wait for it so the client object is not freed under its feet.
    for (int i = 0; i < 120 && connect_task_running_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    Stop();
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------

bool ControlMqtt::LoadConfig() {
    Settings settings("control_mqtt", false);
    // NVS wins; otherwise fall back to the build-time defaults from Kconfig.
    broker_endpoint_ = settings.GetString("endpoint", CONFIG_CONTROL_MQTT_ENDPOINT);
    broker_client_id_ = settings.GetString("client_id");
    broker_username_ = settings.GetString("username", CONFIG_CONTROL_MQTT_USERNAME);
    broker_password_ = settings.GetString("password", CONFIG_CONTROL_MQTT_PASSWORD);
    subscribe_topic_ = settings.GetString("subscribe_topic");
    reply_topic_ = settings.GetString("reply_topic");

    if (broker_endpoint_.empty()) {
        return false;
    }

    // Default client_id: "xiaozhi-{uuid-last-8}"
    if (broker_client_id_.empty()) {
        auto uuid = Board::GetInstance().GetUuid();
        if (uuid.size() > 8) {
            broker_client_id_ = "xiaozhi-" + uuid.substr(uuid.size() - 8);
        } else {
            broker_client_id_ = "xiaozhi-" + uuid;
        }
    }

    // Default device_id (used in hello / topics)
    device_id_ = Board::GetInstance().GetUuid();

    // Default topics
    if (subscribe_topic_.empty()) {
        subscribe_topic_ = "xiaozhi/" + device_id_ + "/cmd";
    }
    if (reply_topic_.empty()) {
        reply_topic_ = "xiaozhi/" + device_id_ + "/reply";
    }

    return true;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool ControlMqtt::Start() {
    if (!LoadConfig()) {
        ESP_LOGI(TAG, "No control_mqtt config found — skipping second broker");
        return true;  // Not an error, just not configured
    }

    if (enabled_.exchange(true)) {
        ESP_LOGW(TAG, "Already started");
        return true;
    }

    if (mqtt_ == nullptr) {
        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        if (network == nullptr) {
            ESP_LOGE(TAG, "No network interface available");
            enabled_ = false;
            return false;
        }
        mqtt_ = network->CreateMqtt(1);  // connect_id=1, separate from xiaozhi.me's 0
        mqtt_->SetKeepAlive(120);

        mqtt_->OnConnected([this]() {
            connected_ = true;
            ESP_LOGI(TAG, "Connected to control broker");
            // Subscribe and announce ourselves on the main task, so device state
            // reads and JSON work stay single-threaded.
            auto alive = alive_;
            Application::GetInstance().Schedule([this, alive]() {
                if (!*alive || !enabled_ || mqtt_ == nullptr || !connected_) {
                    return;
                }
                mqtt_->Subscribe(subscribe_topic_);
                SendHello();
            });
        });

        mqtt_->OnDisconnected([this]() {
            connected_ = false;
            ESP_LOGW(TAG, "Disconnected from control broker (esp-mqtt will retry)");
        });

        mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
            if (!enabled_) {
                return;
            }
            OnMessage(topic, payload);
        });
    }

    StartConnectTask();
    return true;
}

void ControlMqtt::Stop() {
    enabled_ = false;

    if (connected_ && mqtt_ != nullptr) {
        SendGoodbye();
        vTaskDelay(pdMS_TO_TICKS(kGoodbyeFlushDelayMs));
    }
    connected_ = false;

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (connect_task_running_) {
        // The connect task tears the client down itself once Connect() returns.
        return;
    }
    DisconnectLocked();
}

void ControlMqtt::StartConnectTask() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (connect_task_running_ || mqtt_ == nullptr) {
        return;
    }
    connect_task_running_ = true;
    if (xTaskCreate(ConnectTaskEntry, "ctrl_mqtt", 4096 * 2, this, 3, nullptr) != pdPASS) {
        connect_task_running_ = false;
        ESP_LOGE(TAG, "Failed to create control MQTT connect task");
    }
}

void ControlMqtt::ConnectTaskEntry(void* arg) {
    static_cast<ControlMqtt*>(arg)->ConnectTask();
}

void ControlMqtt::ConnectTask() {
    std::string host = broker_endpoint_;
    int port = 8883;
    auto colon = broker_endpoint_.find(':');
    if (colon != std::string::npos) {
        host = broker_endpoint_.substr(0, colon);
        auto port_str = broker_endpoint_.substr(colon + 1);
        if (!port_str.empty()) {
            int parsed = atoi(port_str.c_str());
            if (parsed > 0 && parsed < 65536) {
                port = parsed;
            }
        }
    }

    ESP_LOGI(TAG, "Connecting to control broker %s:%d", host.c_str(), port);
    bool ok = mqtt_->Connect(host, port, broker_client_id_, broker_username_, broker_password_);
    if (ok) {
        ESP_LOGI(TAG, "Control broker connect returned OK");
    } else {
        // Keep the client alive: esp-mqtt's auto-reconnect keeps retrying and will
        // fire OnConnected once the broker becomes reachable again.
        ESP_LOGW(TAG, "Control broker connect failed — esp-mqtt will keep retrying");
    }

    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        connect_task_running_ = false;
        if (!enabled_) {
            DisconnectLocked();
        }
    }

    vTaskDelete(nullptr);
}

void ControlMqtt::DisconnectLocked() {
    if (mqtt_ != nullptr) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
    connected_ = false;
    ESP_LOGI(TAG, "Stopped");
}

// ---------------------------------------------------------------------------
// Publish helpers
// ---------------------------------------------------------------------------

bool ControlMqtt::PublishJson(const std::string& topic, cJSON* root) {
    if (!connected_ || mqtt_ == nullptr) {
        return false;
    }
    char* str = cJSON_PrintUnformatted(root);
    if (str == nullptr) {
        return false;
    }
    bool ok = mqtt_->Publish(topic, str);
    if (!ok) {
        ESP_LOGW(TAG, "Publish to %s failed", topic.c_str());
    }
    cJSON_free(str);
    return ok;
}

// ---------------------------------------------------------------------------
// Hello / Goodbye / Session update
// ---------------------------------------------------------------------------

void ControlMqtt::SendHello() {
    const char* version = "unknown";
    auto app_desc = esp_app_get_description();
    if (app_desc != nullptr && app_desc->version[0] != '\0') {
        version = app_desc->version;
    }
    const char* state = DeviceStateToString(Application::GetInstance().GetDeviceState());

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "status", state);
    cJSON_AddStringToObject(root, "version", version);
    PublishJson(reply_topic_, root);  // Broadcast on reply topic (hello is a status msg)
    cJSON_Delete(root);
    ESP_LOGI(TAG, "hello sent");
}

void ControlMqtt::SendGoodbye() {
    if (!connected_ || mqtt_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "goodbye");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    PublishJson(reply_topic_, root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "goodbye sent");
}

void ControlMqtt::SendSessionUpdate(const std::string& state_name) {
    if (!connected_ || mqtt_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session_update");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "status", state_name.c_str());
    PublishJson(reply_topic_, root);
    cJSON_Delete(root);
}

void ControlMqtt::BroadcastState(const std::string& state_name) {
    // Called from main thread (Application::OnStateChanged context)
    SendSessionUpdate(state_name);
}

// ---------------------------------------------------------------------------
// Reply
// ---------------------------------------------------------------------------

void ControlMqtt::SendReply(const std::string& cmd_type, bool success,
                            const std::string& reply_topic_override, cJSON* extra) {
    if (!connected_ || mqtt_ == nullptr) {
        if (extra != nullptr) {
            cJSON_Delete(extra);
        }
        return;
    }
    const char* state_str = DeviceStateToString(Application::GetInstance().GetDeviceState());
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "reply");
    cJSON_AddStringToObject(root, "command_type", cmd_type.c_str());
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "device_state", state_str);
    if (extra != nullptr) {
        cJSON_AddItemToObject(root, "extra", extra);
    }
    const std::string& topic = reply_topic_override.empty() ? reply_topic_ : reply_topic_override;
    PublishJson(topic, root);
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Message parsing
// ---------------------------------------------------------------------------

void ControlMqtt::OnMessage(const std::string& topic, const std::string& payload) {
    // Runs in the MQTT task: parse here, then hand the result to the main task.
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", payload.c_str());
        return;
    }
    auto alive = alive_;
    Application::GetInstance().Schedule([this, alive, root]() {
        if (!*alive) {
            cJSON_Delete(root);
            return;
        }
        if (enabled_) {
            DispatchCommand(root);
        }
        cJSON_Delete(root);
    });
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void ControlMqtt::DispatchCommand(cJSON* root) {
    // Called on main task (already Schedule()'d)
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "Cmd has no type");
        return;
    }
    std::string cmd = type->valuestring;
    ESP_LOGI(TAG, "Received cmd: %s", cmd.c_str());

    // Only allow mutating commands when not already in a session with xiaozhi.me
    auto state = Application::GetInstance().GetDeviceState();

    if (cmd == "listen") {
        // Only allow listen in Idle or Notifying state
        if (state == kDeviceStateIdle || state == kDeviceStateNotifying) {
            HandleListen(root);
        } else {
            ESP_LOGW(TAG, "Ignoring listen: device is in state %d", state);
            SendReply("listen", false);
        }
    } else if (cmd == "abort") {
        // Abort is safe anytime
        HandleAbort(root);
    } else if (cmd == "mcp") {
        // MCP only makes sense in Idle (no xiaozhi.me session)
        if (state == kDeviceStateIdle) {
            HandleMcp(root);
        } else {
            ESP_LOGW(TAG, "Ignoring mcp: device in state %d", state);
            SendReply("mcp", false);
        }
    } else if (cmd == "system") {
        HandleSystem(root);
    } else if (cmd == "set_image") {
        HandleSetImage(root);
    } else if (cmd == "set_theme") {
        HandleSetTheme(root);
    } else if (cmd == "set_volume") {
        HandleSetVolume(root);
    } else if (cmd == "set_brightness") {
        HandleSetBrightness(root);
    } else if (cmd == "toggle_wake_word") {
        HandleToggleWakeWord(root);
    } else if (cmd == "notify") {
        if (state == kDeviceStateIdle) {
            HandleNotify(root);
        } else {
            ESP_LOGW(TAG, "Ignoring notify: device in state %d", state);
            SendReply("notify", false);
        }
    } else if (cmd == "set_status") {
        HandleSetStatus(root);
    } else if (cmd == "toggle_power_save") {
        HandleTogglePowerSave(root);
    } else if (cmd == "query_status") {
        HandleQueryStatus(root);
    } else if (cmd == "play_sound") {
        HandlePlaySound(root);
    } else {
        ESP_LOGW(TAG, "Unknown cmd type: %s", cmd.c_str());
        SendReply(cmd, false);
    }
}

// ---------------------------------------------------------------------------
// Individual command handlers (all run on main task)
// ---------------------------------------------------------------------------

void ControlMqtt::HandleListen(cJSON* root) {
    auto state_j = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state_j)) {
        SendReply("listen", false);
        return;
    }
    if (strcmp(state_j->valuestring, "start") == 0) {
        Application::GetInstance().ToggleChatState();
    } else if (strcmp(state_j->valuestring, "stop") == 0) {
        Application::GetInstance().StopListening();
    } else {
        SendReply("listen", false);
        return;
    }
    SendReply("listen", true);
}

void ControlMqtt::HandleAbort(cJSON* root) {
    auto reason_j = cJSON_GetObjectItem(root, "reason");
    AbortReason reason = kAbortReasonNone;
    if (cJSON_IsString(reason_j) && strcmp(reason_j->valuestring, "wake_word_detected") == 0) {
        reason = kAbortReasonWakeWordDetected;
    }
    Application::GetInstance().AbortSpeaking(reason);
    SendReply("abort", true);
}

void ControlMqtt::HandleMcp(cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(payload)) {
        SendReply("mcp", false);
        return;
    }
    char* str = cJSON_PrintUnformatted(payload);
    if (str == nullptr) {
        SendReply("mcp", false);
        return;
    }
    Application::GetInstance().SendMcpMessage(std::string(str));
    cJSON_free(str);
    SendReply("mcp", true);
}

void ControlMqtt::HandleSystem(cJSON* root) {
    auto cmd_j = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd_j)) {
        SendReply("system", false);
        return;
    }
    if (strcmp(cmd_j->valuestring, "reboot") == 0) {
        SendReply("system", true);
        vTaskDelay(pdMS_TO_TICKS(100));  // let the reply reach the outbox
        Application::GetInstance().Reboot();
    } else {
        ESP_LOGW(TAG, "Unknown system command: %s", cmd_j->valuestring);
        SendReply("system", false);
    }
}

namespace {
// Heap-owned arguments for the image download task.
struct ImageFetchArgs {
    ControlMqtt* self;
    std::string url;
    std::shared_ptr<std::atomic<bool>> alive;
};
}  // namespace

void ControlMqtt::HandleSetImage(cJSON* root) {
    auto url_j = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url_j) || url_j->valuestring[0] == '\0') {
        SendReply("set_image", false);
        return;
    }
    if (dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay()) == nullptr) {
        ESP_LOGW(TAG, "set_image: no LVGL display available");
        SendReply("set_image", false);
        return;
    }

    auto* args = new (std::nothrow) ImageFetchArgs{this, url_j->valuestring, alive_};
    if (args == nullptr) {
        SendReply("set_image", false);
        return;
    }
    if (xTaskCreate(ImageFetchTaskEntry, "ctrl_image", 4096 * 2, args, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create image download task");
        delete args;
        SendReply("set_image", false);
    }
}

void ControlMqtt::ImageFetchTaskEntry(void* arg) {
    std::unique_ptr<ImageFetchArgs> args(static_cast<ImageFetchArgs*>(arg));
    char* data = nullptr;
    size_t total = 0;

    do {
        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        if (network == nullptr) {
            break;
        }
        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            break;
        }
        http->SetTimeout(10000);
        if (!http->Open("GET", args->url)) {
            ESP_LOGE(TAG, "HTTP open failed: %d", http->GetLastError());
            break;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "HTTP status %d", http->GetStatusCode());
            break;
        }
        size_t content_length = http->GetBodyLength();
        if (content_length == 0 || content_length > kMaxImageBytes) {
            ESP_LOGE(TAG, "Image size invalid: %zu", content_length);
            break;
        }
        data = static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (data == nullptr) {
            ESP_LOGE(TAG, "OOM for image (%zu bytes)", content_length);
            break;
        }
        while (total < content_length) {
            int ret = http->Read(data + total, content_length - total);
            if (ret <= 0) {
                break;
            }
            total += ret;
        }
    } while (false);

    if (total == 0 && data != nullptr) {
        ESP_LOGE(TAG, "Downloaded 0 bytes");
        heap_caps_free(data);
        data = nullptr;
    }

    auto* self = args->self;
    auto alive = args->alive;
    args.reset();

    Application::GetInstance().Schedule([self, alive, data, total]() {
        if (!*alive) {
            if (data != nullptr) {
                heap_caps_free(data);
            }
            return;
        }
        if (data == nullptr) {
            self->SendReply("set_image", false);
            return;
        }
        auto* lvgl_display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (lvgl_display == nullptr) {
            heap_caps_free(data);
            self->SendReply("set_image", false);
            return;
        }
        try {
            auto image = std::make_unique<LvglAllocatedImage>(data, total);
            lvgl_display->SetPreviewImage(std::move(image));
            self->SendReply("set_image", true);
        } catch (const std::exception& e) {
            // Constructor may throw for unsupported/corrupt data; it does not take
            // ownership in that case, so release the buffer here.
            ESP_LOGE(TAG, "Unsupported image data: %s", e.what());
            heap_caps_free(data);
            self->SendReply("set_image", false);
        }
    });

    vTaskDelete(nullptr);
}

void ControlMqtt::HandleSetTheme(cJSON* root) {
    auto theme_j = cJSON_GetObjectItem(root, "theme");
    if (!cJSON_IsString(theme_j)) {
        SendReply("set_theme", false);
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        SendReply("set_theme", false);
        return;
    }
    // Theme objects are board-defined; there is no generic registry to look them up.
    auto* current = display->GetTheme();
    ESP_LOGW(TAG, "set_theme is not implemented (current=%s, requested=%s)",
             current != nullptr ? current->name().c_str() : "none", theme_j->valuestring);

    cJSON* extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "error", "theme switching is not implemented for this board");
    SendReply("set_theme", false, "", extra);
}

void ControlMqtt::HandleSetVolume(cJSON* root) {
    auto level_j = cJSON_GetObjectItem(root, "level");
    if (!cJSON_IsNumber(level_j)) {
        SendReply("set_volume", false);
        return;
    }
    int level = level_j->valueint;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->SetOutputVolume(level);
        SendReply("set_volume", true);
    } else {
        SendReply("set_volume", false);
    }
}

void ControlMqtt::HandleSetBrightness(cJSON* root) {
    auto level_j = cJSON_GetObjectItem(root, "level");
    if (!cJSON_IsNumber(level_j)) {
        SendReply("set_brightness", false);
        return;
    }
    int level = level_j->valueint;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight) {
        // Persist so the level survives a reboot.
        backlight->SetBrightness(static_cast<uint8_t>(level), true);
        SendReply("set_brightness", true);
    } else {
        SendReply("set_brightness", false);
    }
}

void ControlMqtt::HandleToggleWakeWord(cJSON* root) {
    auto enable_j = cJSON_GetObjectItem(root, "enable");
    bool enable = cJSON_IsBool(enable_j) ? enable_j->valueint : true;
    auto& app = Application::GetInstance();
    // Don't override xiaozhi.me's session management; only toggle in Idle
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGW(TAG, "Ignoring toggle_wake_word: device is not idle");
        SendReply("toggle_wake_word", false);
        return;
    }
    app.GetAudioService().EnableWakeWordDetection(enable);
    SendReply("toggle_wake_word", true);
}

void ControlMqtt::HandleNotify(cJSON* root) {
    auto url_j = cJSON_GetObjectItem(root, "audio_url");
    if (!cJSON_IsString(url_j) || url_j->valuestring[0] == '\0') {
        SendReply("notify", false);
        return;
    }
    std::string audio_url = url_j->valuestring;

    std::vector<NotifySubtitle> subtitles;
    auto subs = cJSON_GetObjectItem(root, "subtitles");
    if (cJSON_IsArray(subs)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, subs) {
            auto start = cJSON_GetObjectItem(item, "start_ms");
            auto text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsNumber(start) && cJSON_IsString(text)) {
                NotifySubtitle sub;
                sub.start_ms = static_cast<uint32_t>(start->valuedouble);
                sub.text = text->valuestring;
                subtitles.push_back(sub);
            }
        }
    }

    Application::GetInstance().StartNotification(std::move(audio_url), std::move(subtitles));
    SendReply("notify", true);
}

void ControlMqtt::HandleSetStatus(cJSON* root) {
    auto text_j = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_j)) {
        SendReply("set_status", false);
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        SendReply("set_status", false);
        return;
    }
    display->SetStatus(text_j->valuestring);
    SendReply("set_status", true);
}

void ControlMqtt::HandleTogglePowerSave(cJSON* root) {
    auto enable_j = cJSON_GetObjectItem(root, "enable");
    bool enable = cJSON_IsBool(enable_j) ? enable_j->valueint : true;
    // Don't override xiaozhi.me's session management
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGW(TAG, "Ignoring toggle_power_save: device is not idle");
        SendReply("toggle_power_save", false);
        return;
    }
    Board::GetInstance().SetPowerSaveLevel(
        enable ? PowerSaveLevel::LOW_POWER : PowerSaveLevel::PERFORMANCE);
    SendReply("toggle_power_save", true);
}

void ControlMqtt::HandleQueryStatus(cJSON* root) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();

    cJSON* extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "device_state", DeviceStateToString(app.GetDeviceState()));

    bool charging = false, discharging = false;
    int battery = -1;
    board.GetBatteryLevel(battery, charging, discharging);
    if (battery >= 0) {
        cJSON_AddNumberToObject(extra, "battery", battery);
        cJSON_AddBoolToObject(extra, "charging", charging);
    }

    float temp = 0;
    if (board.GetTemperature(temp)) {
        cJSON_AddNumberToObject(extra, "temperature", temp);
    }

    cJSON_AddBoolToObject(extra, "mqtt_connected", connected_);
    SendReply("query_status", true, "", extra);
}

void ControlMqtt::HandlePlaySound(cJSON* root) {
    auto sound_j = cJSON_GetObjectItem(root, "sound");
    if (!cJSON_IsString(sound_j)) {
        SendReply("play_sound", false);
        return;
    }
    Application::GetInstance().PlaySound(std::string(sound_j->valuestring));
    SendReply("play_sound", true);
}
