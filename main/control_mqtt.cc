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
#include "lvgl_theme.h"
#include "jpg/jpeg_to_image.h"
#include "http.h"
#include "notify/notify_player.h"
#include "settings.h"
#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

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
// Decoded-pixel limits. A 1 MP RGB565 frame is about 2 MB of PSRAM, so bigger
// pictures would risk exhausting memory during decode/display.
static constexpr int kMaxImageWidth = 1280;
static constexpr int kMaxImageHeight = 1280;
static constexpr int kMaxImagePixels = 1000 * 1000;
// Give esp-mqtt a brief moment to flush the goodbye before tearing the client down.
static constexpr int kGoodbyeFlushDelayMs = 50;

ControlMqtt::ControlMqtt()
    : alive_(std::make_shared<std::atomic<bool>>(true)) {
}

ControlMqtt::~ControlMqtt() {
    *alive_ = false;
    enabled_ = false;

    // A connect attempt may still be blocked inside Mqtt::Connect (its own timeout)
    // and an image task may still be downloading; wait so member state is not freed
    // under their feet.
    for (int i = 0; i < 120 && (connect_task_running_ || image_fetch_running_); ++i) {
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

void ControlMqtt::BroadcastEvent(const std::string& event, const std::string& value,
                                 const std::string& state) {
    // Called from main thread (Application::ReportUserEvent). Best effort: events that
    // happen while the control broker is offline are dropped.
    if (!connected_ || mqtt_ == nullptr || reply_topic_.empty()) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "event", event.c_str());
    if (!value.empty()) {
        cJSON_AddStringToObject(root, "value", value.c_str());
    }
    cJSON_AddStringToObject(root, "state", state.c_str());
    cJSON_AddNumberToObject(root, "uptime_ms",
                            static_cast<double>(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "seq", static_cast<double>(++event_seq_));
    PublishJson(reply_topic_, root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Event sent: %s%s%s", event.c_str(), value.empty() ? "" : "=", value.c_str());
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
        // "stop" must work while the device is actually listening/speaking, so only
        // "start" (and anything else) is restricted to Idle/Notifying.
        auto state_j = cJSON_GetObjectItem(root, "state");
        bool is_stop = cJSON_IsString(state_j) && strcmp(state_j->valuestring, "stop") == 0;
        if (is_stop || state == kDeviceStateIdle || state == kDeviceStateNotifying) {
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
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    // AbortSpeaking() only acts on an active turn; report failure otherwise instead
    // of claiming success for a no-op.
    bool abortable = (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                      state == kDeviceStateSpeaking);
    if (!abortable) {
        ESP_LOGW(TAG, "abort ignored: device is in state %d", state);
        SendReply("abort", false);
        return;
    }
    app.AbortSpeaking(reason);
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

// Decoded/downloaded image handed over to the main task.
struct PendingPreview {
    char* data = nullptr;      // owned; freed with heap_caps_free()
    size_t len = 0;
    bool rgb565 = false;       // data is raw RGB565 with explicit geometry
    int width = 0;
    int height = 0;
    int stride = 0;
    const char* error = nullptr;  // static string, reason for a failed fetch/decode
};

inline bool LooksLikeJpeg(const char* data, size_t len) {
    return len > 3 && static_cast<uint8_t>(data[0]) == 0xFF && static_cast<uint8_t>(data[1]) == 0xD8;
}

inline bool LooksLikePng(const char* data, size_t len) {
    static const uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return len >= sizeof(kPngSignature) &&
           memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0;
}

// Reads the frame size out of a JPEG stream without decoding it, so oversized
// pictures are rejected before a large RGB565 buffer is allocated.
bool GetJpegSize(const uint8_t* data, size_t len, int* out_width, int* out_height) {
    size_t pos = 2;  // Skip SOI
    while (pos + 4 <= len) {
        if (data[pos] != 0xFF) {
            ++pos;
            continue;
        }
        uint8_t marker = data[pos + 1];
        if (marker == 0xFF) {  // Fill byte
            ++pos;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;  // Standalone markers without a length field
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return false;  // EOI or SOS before any SOF
        }
        size_t segment_len = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
        if (segment_len < 2) {
            return false;
        }
        bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                      marker != 0xCC;
        if (is_sof) {
            if (pos + 9 > len) {
                return false;
            }
            *out_height = (data[pos + 5] << 8) | data[pos + 6];
            *out_width = (data[pos + 7] << 8) | data[pos + 8];
            return *out_width > 0 && *out_height > 0;
        }
        pos += 2 + segment_len;
    }
    return false;
}

// Reads width/height from the PNG IHDR chunk.
bool GetPngSize(const char* data, size_t len, int* out_width, int* out_height) {
    if (len < 24 || memcmp(data + 12, "IHDR", 4) != 0) {
        return false;
    }
    auto* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t width = (static_cast<uint32_t>(p[16]) << 24) | (static_cast<uint32_t>(p[17]) << 16) |
                     (static_cast<uint32_t>(p[18]) << 8) | p[19];
    uint32_t height = (static_cast<uint32_t>(p[20]) << 24) | (static_cast<uint32_t>(p[21]) << 16) |
                      (static_cast<uint32_t>(p[22]) << 8) | p[23];
    if (width == 0 || height == 0) {
        return false;
    }
    *out_width = static_cast<int>(width > 0x7FFFFFFF ? 0x7FFFFFFF : width);
    *out_height = static_cast<int>(height > 0x7FFFFFFF ? 0x7FFFFFFF : height);
    return true;
}

inline bool IsImageTooLarge(int width, int height) {
    return width > kMaxImageWidth || height > kMaxImageHeight ||
           static_cast<int64_t>(width) * height > kMaxImagePixels;
}

bool EqualsIgnoreCase(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + ('a' - 'A')) : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Accepts a color name (red, green, ...) or "#RRGGBB".
bool ParseColor(const char* value, uint32_t* out_rgb) {
    if (value[0] == '#') {
        if (strlen(value) != 7) {
            return false;
        }
        char* end = nullptr;
        unsigned long rgb = strtoul(value + 1, &end, 16);
        if (end == nullptr || *end != '\0') {
            return false;
        }
        *out_rgb = static_cast<uint32_t>(rgb) & 0xFFFFFF;
        return true;
    }
    static const struct {
        const char* name;
        uint32_t rgb;
    } kColors[] = {
        {"red", 0xFF0000},   {"green", 0x00FF00},  {"blue", 0x0000FF},
        {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"white", 0xFFFFFF},
        {"black", 0x000000}, {"cyan", 0x00FFFF},   {"purple", 0x800080},
        {"gray", 0x808080},  {"grey", 0x808080},
    };
    for (const auto& entry : kColors) {
        if (EqualsIgnoreCase(value, entry.name)) {
            *out_rgb = entry.rgb;
            return true;
        }
    }
    return false;
}

const char* const kImageTooLargeError = "image too large (max 1280x1280, 1 MP)";

cJSON* MakeErrorExtra(const char* message) {
    cJSON* extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "error", message);
    return extra;
}
}  // namespace

void ControlMqtt::HandleSetImage(cJSON* root) {
    // {"type":"set_image","clear":true} restores the normal UI.
    auto clear_j = cJSON_GetObjectItem(root, "clear");
    if (cJSON_IsTrue(clear_j)) {
        auto* lvgl_display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (lvgl_display == nullptr) {
            SendReply("set_image", false, "", MakeErrorExtra("no LVGL display"));
            return;
        }
        lvgl_display->SetPreviewImage(nullptr);
        SendReply("set_image", true);
        return;
    }

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
    if (image_fetch_running_.exchange(true)) {
        ESP_LOGW(TAG, "set_image: previous download still running");
        delete args;
        SendReply("set_image", false, "", MakeErrorExtra("another image download is in progress"));
        return;
    }
    // JPEG decoding is CPU heavy, so it stays on this task as well.
    if (xTaskCreate(ImageFetchTaskEntry, "ctrl_image", 4096 * 3, args, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create image download task");
        image_fetch_running_ = false;
        delete args;
        SendReply("set_image", false);
    }
}

void ControlMqtt::ImageFetchTaskEntry(void* arg) {
    std::unique_ptr<ImageFetchArgs> args(static_cast<ImageFetchArgs*>(arg));
    // Captured up front: the flag has to be cleared manually before vTaskDelete(),
    // because a stack destructor would never run afterwards.
    ControlMqtt* self = args->self;
    auto alive = args->alive;
    char* data = nullptr;
    size_t total = 0;
    const char* fail_reason = "download failed";

    do {
        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        if (network == nullptr) {
            fail_reason = "no network";
            break;
        }
        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            fail_reason = "http client unavailable";
            break;
        }
        http->SetTimeout(10000);
        if (!http->Open("GET", args->url)) {
            ESP_LOGE(TAG, "HTTP open failed: %d", http->GetLastError());
            fail_reason = "http open failed";
            break;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "HTTP status %d", http->GetStatusCode());
            fail_reason = "http status is not 200";
            break;
        }
        size_t content_length = http->GetBodyLength();
        if (content_length == 0 || content_length > kMaxImageBytes) {
            ESP_LOGE(TAG, "Image size invalid: %zu", content_length);
            fail_reason = (content_length == 0) ? "empty body" : "image too large (max 4 MB)";
            break;
        }
        data = static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (data == nullptr) {
            ESP_LOGE(TAG, "OOM for image (%zu bytes)", content_length);
            fail_reason = "out of memory";
            break;
        }
        while (total < content_length) {
            int ret = http->Read(data + total, content_length - total);
            if (ret <= 0) {
                break;
            }
            total += ret;
        }
        if (total < content_length) {
            ESP_LOGE(TAG, "Incomplete download: %zu/%zu", total, content_length);
            heap_caps_free(data);
            data = nullptr;
            total = 0;
            fail_reason = "download incomplete";
        }
    } while (false);

    args.reset();

    auto* pending = new (std::nothrow) PendingPreview;
    if (pending == nullptr) {
        if (data != nullptr) {
            heap_caps_free(data);
        }
        self->image_fetch_running_ = false;
        Application::GetInstance().Schedule([self, alive]() {
            if (*alive) {
                self->SendReply("set_image", false);
            }
        });
        vTaskDelete(nullptr);
        return;
    }
    pending->data = data;
    pending->len = total;

    if (pending->data != nullptr && total == 0) {
        heap_caps_free(data);
        pending->data = nullptr;
    }

    if (pending->data == nullptr) {
        pending->error = fail_reason;
    } else {
        // Parse the header first: oversized pictures are rejected before any
        // full-size RGB565 buffer is allocated.
        int img_width = 0;
        int img_height = 0;
        bool dims_known = false;
        if (LooksLikeJpeg(data, total)) {
            dims_known = GetJpegSize(reinterpret_cast<const uint8_t*>(data), total, &img_width,
                                     &img_height);
        } else if (LooksLikePng(data, total)) {
            dims_known = GetPngSize(data, total, &img_width, &img_height);
        }
        if (dims_known && IsImageTooLarge(img_width, img_height)) {
            ESP_LOGE(TAG, "Image dimensions too large: %dx%d", img_width, img_height);
            heap_caps_free(data);
            pending->data = nullptr;
            pending->error = kImageTooLargeError;
        } else if (LooksLikeJpeg(data, total)) {
            // LVGL has no JPEG decoder in this build (CONFIG_LV_USE_TJPGD off), so decode
            // through the project's own helper and hand LVGL ready-to-use RGB565 pixels.
            uint8_t* rgb = nullptr;
            size_t rgb_len = 0, width = 0, height = 0, stride = 0;
            esp_err_t ret = jpeg_to_image(reinterpret_cast<const uint8_t*>(data), total, &rgb,
                                          &rgb_len, &width, &height, &stride);
            heap_caps_free(data);
            pending->data = nullptr;
            if (ret != ESP_OK || rgb == nullptr) {
                ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
                if (rgb != nullptr) {
                    heap_caps_free(rgb);
                }
                pending->error = "jpeg decode failed";
            } else {
                pending->data = reinterpret_cast<char*>(rgb);
                pending->len = rgb_len;
                pending->rgb565 = true;
                pending->width = static_cast<int>(width);
                pending->height = static_cast<int>(height);
                pending->stride = static_cast<int>(stride);
            }
        }
    }

    Application::GetInstance().Schedule([self, alive, pending]() {
        std::unique_ptr<PendingPreview> p(pending);
        if (!*alive) {
            if (p->data != nullptr) {
                heap_caps_free(p->data);
            }
            return;
        }
        if (p->error != nullptr) {
            self->SendReply("set_image", false, "", MakeErrorExtra(p->error));
            return;
        }
        auto* lvgl_display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (lvgl_display == nullptr) {
            heap_caps_free(p->data);
            p->data = nullptr;
            self->SendReply("set_image", false, "", MakeErrorExtra("no LVGL display"));
            return;
        }
        try {
            std::unique_ptr<LvglImage> image;
            if (p->rgb565) {
                image = std::make_unique<LvglAllocatedImage>(p->data, p->len, p->width, p->height,
                                                            p->stride, LV_COLOR_FORMAT_RGB565);
            } else {
                image = std::make_unique<LvglAllocatedImage>(p->data, p->len);
            }
            // LvglAllocatedImage takes ownership of the buffer.
            p->data = nullptr;
            // Backstop for formats whose size cannot be read from the header
            // (JPEG/PNG are already checked before decoding).
            auto* dsc = image->image_dsc();
            if (dsc != nullptr && IsImageTooLarge(dsc->header.w, dsc->header.h)) {
                ESP_LOGE(TAG, "Image dimensions too large: %dx%d", dsc->header.w, dsc->header.h);
                self->SendReply("set_image", false, "", MakeErrorExtra(kImageTooLargeError));
                return;
            }
            lvgl_display->SetPreviewImage(std::move(image));
            // Upstream auto-hides previews after 5s; a remote screen image should stay
            // until it is replaced or cleared.
            lvgl_display->CancelPreviewTimeout();
            ESP_LOGI(TAG, "Image applied; it stays until replaced or cleared");
            self->SendReply("set_image", true);
        } catch (const std::exception& e) {
            // The constructor does not take ownership when it throws.
            ESP_LOGE(TAG, "Unsupported image data: %s", e.what());
            if (p->data != nullptr) {
                heap_caps_free(p->data);
                p->data = nullptr;
            }
            self->SendReply("set_image", false, "", MakeErrorExtra(e.what()));
        }
    });

    self->image_fetch_running_ = false;
    vTaskDelete(nullptr);
}

void ControlMqtt::HandleSetTheme(cJSON* root) {
    auto theme_j = cJSON_GetObjectItem(root, "theme");
    if (!cJSON_IsString(theme_j) || theme_j->valuestring[0] == '\0') {
        SendReply("set_theme", false, "", MakeErrorExtra("missing theme name"));
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr || display->GetTheme() == nullptr) {
        SendReply("set_theme", false, "", MakeErrorExtra("display does not support themes"));
        return;
    }
    // Same lookup the on-screen double-tap and the MCP tool use.
    auto* theme = LvglThemeManager::GetInstance().GetTheme(theme_j->valuestring);
    if (theme == nullptr) {
        ESP_LOGW(TAG, "Unknown theme: %s", theme_j->valuestring);
        SendReply("set_theme", false, "", MakeErrorExtra("unknown theme (supported: light, dark)"));
        return;
    }
    display->SetTheme(theme);
    ESP_LOGI(TAG, "Theme switched to %s", theme_j->valuestring);
    SendReply("set_theme", true);
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

    // Optional "color": a name (red, green, ...) or "#RRGGBB".
    uint32_t rgb = 0;
    bool has_color = false;
    auto color_j = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsString(color_j) && color_j->valuestring[0] != '\0') {
        if (!ParseColor(color_j->valuestring, &rgb)) {
            SendReply("set_status", false, "",
                      MakeErrorExtra(
                          "unknown color (use red/green/blue/yellow/orange/white/black or #RRGGBB)"));
            return;
        }
        has_color = true;
    }

    display->SetStatus(text_j->valuestring);
    if (has_color) {
        // SetStatus() resets the color to the theme's, so apply the override after it.
        display->SetStatusColor(rgb);
    }
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
