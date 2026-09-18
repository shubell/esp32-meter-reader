/**
 * @file esp32cam_watermeter_upload.ino
 * @brief ESP32-CAM (AI-Thinker) with full web-configurable settings
 * @Hardware: AI-Thinker ESP32-CAM with OV2640 camera
 * @Board: "AI Thinker ESP32-CAM" in Arduino IDE
 * @Dependent libraries: WiFi (built-in), ArduinoHttpClient
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <Preferences.h>

// ===== FLAG: Enable or disable the internal calibration web server =====
#define ENABLE_CALIBRATION_SERVER true

#if ENABLE_CALIBRATION_SERVER
  #include <WebServer.h>
  WebServer server(80);
#endif

// ===== FACTORY DEFAULTS (used only on first boot or after NVS clear) =====
#define CAM_VFLIP_DEFAULT     1
#define CAM_HMIRROR_DEFAULT   0
#define FLASH_DEFAULT         1
// ===== Interval between captures (sec) =====
#define INTERVAL_SECS_DEFAULT 14400
#define FRAMESIZE_DEFAULT     FRAMESIZE_CIF

Preferences prefs;
#define NVS_NAMESPACE "cam"

// ===== Runtime settings (loaded from NVS at boot) =====
int cam_vflip   = CAM_VFLIP_DEFAULT;
int cam_hmirror = CAM_HMIRROR_DEFAULT;
int flash_enabled = FLASH_DEFAULT;
unsigned long uploadIntervalMs = INTERVAL_SECS_DEFAULT * 1000UL;
int frameSize = FRAMESIZE_DEFAULT;

// ===== Camera pin definitions for AI-Thinker ESP32-CAM =====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ===== Flash LED (on-board) =====
#define FLASH_LED_PIN      4

// ===== WiFi credentials =====
#define _WIFI_SSID "My SSID"
#define _WIFI_PASS "MyPassword"

// ===== HTTP server =====
#define _SERVER_HOST "127.0.0.1"
#define _SERVER_PORT 80


// ===== Forward declarations =====
bool initCamera();
bool reinitCamera();
WiFiClient connectToWiFi();
bool sendImage(WiFiClient wifi);
void applyOrientation();

#if ENABLE_CALIBRATION_SERVER
void handleRoot();
void handleCapture();
void handleSet();
void handleGet();
void handleReset();
#endif

void setup() {
    Serial.begin(115200);
    Serial.println("Booting...");

    // Flash briefly to indicate start
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(1000);
    digitalWrite(FLASH_LED_PIN, LOW);

    // Load all settings from NVS
    prefs.begin(NVS_NAMESPACE, false);
    cam_vflip        = prefs.getInt("vflip",    CAM_VFLIP_DEFAULT);
    cam_hmirror      = prefs.getInt("hmirror",  CAM_HMIRROR_DEFAULT);
    flash_enabled    = prefs.getInt("flash",    FLASH_DEFAULT);
    uploadIntervalMs = (unsigned long)prefs.getInt("interval", INTERVAL_SECS_DEFAULT) * 1000UL;
    frameSize        = prefs.getInt("framesize", FRAMESIZE_DEFAULT);

    Serial.printf("Loaded settings: vflip=%d hmirror=%d flash=%d interval=%lus framesize=%d\n",
                  cam_vflip, cam_hmirror, flash_enabled, uploadIntervalMs / 1000, frameSize);

    if (!initCamera()) {
        Serial.println("Camera configuration failed, halting");
        while (true) delay(1000);   // stop here
    }

    // Connect to WiFi once at startup
    WiFiClient wifi = connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection failed, halting");
        while (true) delay(1000);
    }

    Serial.println("Setup complete. Entering main loop.");
    

#if ENABLE_CALIBRATION_SERVER
    server.on("/", HTTP_GET, handleRoot);
    server.on("/capture", HTTP_GET, handleCapture);
    server.on("/set", HTTP_GET, handleSet);
    server.on("/get", HTTP_GET, handleGet);
    server.on("/reset", HTTP_GET, handleReset);
    server.begin();
    Serial.println("Web server started. Open http://" + WiFi.localIP().toString() + "/ to calibrate.");
#else
    Serial.println("Calibration web server is disabled.");
#endif

    Serial.println("Setup complete.");
}

void applyOrientation() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    s->set_vflip(s, cam_vflip);
    s->set_hmirror(s, cam_hmirror);
    Serial.printf("Orientation applied: vflip=%d hmirror=%d\n", cam_vflip, cam_hmirror);
}

bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    config.frame_size   = (framesize_t)frameSize;
    config.jpeg_quality = 12;
    config.fb_count     = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return false;
    }

    applyOrientation();
    Serial.println("Camera init success");
    return true;
}

// De-init and re-init the camera with current settings (for live resolution change)
bool reinitCamera() {
    Serial.println("Reinitializing camera...");
    esp_camera_deinit();
    delay(100);
    return initCamera();
}

WiFiClient connectToWiFi() {
    WiFiClient wifi;
    WiFi.mode(WIFI_STA);
    WiFi.begin(_WIFI_SSID, _WIFI_PASS);
    WiFi.setSleep(false);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(_WIFI_SSID);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        if (attempts >= 20) {
            Serial.println("\nFailed to connect after 20 attempts");
            return WiFiClient();
        }
        delay(1000);
        Serial.print(".");
        attempts++;
    }

    Serial.println();
    Serial.print("Connected to ");
    Serial.println(_WIFI_SSID);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return wifi;
}

bool sendImage(WiFiClient wifi) {
    if (flash_enabled) {
        digitalWrite(FLASH_LED_PIN, HIGH);
        delay(300);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        if (flash_enabled) digitalWrite(FLASH_LED_PIN, LOW);
        Serial.println("Could not get camera frame");
        return false;
    }

    if (flash_enabled) digitalWrite(FLASH_LED_PIN, LOW);
    Serial.printf("Captured image: %u bytes\n", fb->len);

    HttpClient client = HttpClient(wifi, _SERVER_HOST, _SERVER_PORT);
    Serial.println("Making POST request");

    String path = "/ocr";
    String contentType = "image/jpeg";

    client.post(path.c_str(), contentType.c_str(), fb->len, fb->buf);

    int statusCode = client.responseStatusCode();
    String response = client.responseBody();

    Serial.print("Status code: ");
    Serial.println(statusCode);
    Serial.print("Response: ");
    Serial.println(response);

    esp_camera_fb_return(fb);

    if (statusCode != 202) {
        Serial.println("Upload failed");
        return false;
    }

    return true;
}

// ===== Web server handlers =====
#if ENABLE_CALIBRATION_SERVER

void handleRoot() {
    String html = F(
        "<!DOCTYPE html><html><head><title>ESP32-CAM Config</title>"
        "<meta charset=\"utf-8\">"
        "<style>"
        "body{font-family:sans-serif;margin:20px;max-width:900px;}"
        "button{padding:8px 14px;margin:4px;border:1px solid #888;border-radius:5px;cursor:pointer;background:#eee;}"
        "button.active{background:#4CAF50;color:white;border-color:#3a8a3d;font-weight:bold;}"
        ".row{margin:12px 0;padding:10px;border:1px solid #ddd;border-radius:6px;}"
        ".row label{display:inline-block;min-width:160px;font-weight:bold;}"
        "select,input[type=number]{padding:6px;font-size:14px;}"
        ".note{color:#666;font-size:13px;}"
        "</style></head><body>"
        "<h1>ESP32-CAM Configuration</h1>"

        "<div class=\"row\"><img id=\"cam\" src=\"/capture\" width=\"640\"></div>"

        "<div class=\"row\">"
        "<label>Orientation:</label><br>"
        "<button id=\"btnReset\"  onclick=\"setOri(0,0)\">Normal</button>"
        "<button id=\"btnV\"      onclick=\"setOri(1,0)\">Flip Vertical</button>"
        "<button id=\"btnH\"      onclick=\"setOri(0,1)\">Mirror Horizontal</button>"
        "<button id=\"btn180\"    onclick=\"setOri(1,1)\">Rotate 180&deg;</button>"
        "</div>"

        "<div class=\"row\">"
        "<label for=\"res\">Resolution:</label>"
        "<select id=\"res\" onchange=\"saveRes()\">"
        "<option value=\"5\">QVGA 320x240</option>"
        "<option value=\"8\">CIF 400x296</option>"
        "<option value=\"10\">VGA 640x480</option>"
        "<option value=\"13\">SVGA 800x600</option>"
        "<option value=\"15\">SXGA 1280x1024</option>"
        "<option value=\"16\">UXGA 1600x1200</option>"
        "</select>"
        "<span class=\"note\">&nbsp;Changing resolution takes ~2 s</span>"
        "</div>"

        "<div class=\"row\">"
        "<label for=\"interval\">Upload interval (s):</label>"
        "<input type=\"number\" id=\"interval\" min=\"5\" max=\"86400\" onchange=\"saveInterval()\">"
        "<span class=\"note\">&nbsp;5–86400 seconds</span>"
        "</div>"

        "<div class=\"row\">"
        "<label for=\"flash\">Flash LED:</label>"
        "<button id=\"btnFlash\" onclick=\"toggleFlash()\">Toggle</button>"
        "</div>"

        "<div class=\"row\">"
        "<button onclick=\"resetFactory()\" style=\"background:#f8d7da;border-color:#d33;\">Reset ALL to Factory</button>"
        "</div>"

        "<p id=\"status\"></p>"

        "<script>"
        "function loadSettings(){"
        "  fetch('/get').then(r=>r.text()).then(t=>{"
        "    let p={};"
        "    t.split('&').forEach(kv=>{let a=kv.split('=');p[a[0]]=a[1];});"
        "    document.getElementById('res').value      = p.res;"
        "    document.getElementById('interval').value = p.interval;"
        "    updateOri(p.vflip, p.hmirror);"
        "    updateFlash(p.flash);"
        "    document.getElementById('status').textContent = 'Loaded from device';"
        "  });"
        "}"
        "function updateOri(v,h){"
        "  v=parseInt(v); h=parseInt(h);"
        "  document.getElementById('btnReset').classList.toggle('active', v==0 && h==0);"
        "  document.getElementById('btnV').classList.toggle('active', v==1 && h==0);"
        "  document.getElementById('btnH').classList.toggle('active', v==0 && h==1);"
        "  document.getElementById('btn180').classList.toggle('active', v==1 && h==1);"
        "}"
        "function updateFlash(f){"
        "  f=parseInt(f);"
        "  let b=document.getElementById('btnFlash');"
        "  b.textContent = f ? 'ON' : 'OFF';"
        "  b.classList.toggle('active', f==1);"
        "}"
        "function setOri(v,h){"
        "  fetch('/set?v='+v+'&h='+h).then(()=>{"
        "    updateOri(v,h);"
        "    refreshCam();"
        "    document.getElementById('status').textContent='Orientation saved';"
        "  });"
        "}"
        "function saveRes(){"
        "  let r=document.getElementById('res').value;"
        "  document.getElementById('status').textContent='Applying resolution...';"
        "  fetch('/set?res='+r).then(()=>{"
        "    document.getElementById('status').textContent='Resolution saved';"
        "    refreshCam();"
        "  });"
        "}"
        "function saveInterval(){"
        "  let i=document.getElementById('interval').value;"
        "  fetch('/set?interval='+i).then(()=>{"
        "    document.getElementById('status').textContent='Interval saved: '+i+' s';"
        "  });"
        "}"
        "function toggleFlash(){"
        "  fetch('/get').then(r=>r.text()).then(t=>{"
        "    let p={};"
        "    t.split('&').forEach(kv=>{let a=kv.split('=');p[a[0]]=a[1];});"
        "    let newVal = (p.flash=='1') ? 0 : 1;"
        "    fetch('/set?flash='+newVal).then(()=>{"
        "      updateFlash(newVal);"
        "      document.getElementById('status').textContent='Flash '+(newVal?'ON':'OFF');"
        "    });"
        "  });"
        "}"
        "function resetFactory(){"
        "  if(!confirm('Reset ALL settings to factory defaults?')) return;"
        "  fetch('/reset').then(()=>{location.reload();});"
        "}"
        "function refreshCam(){"
        "  document.getElementById('cam').src='/capture?t='+Date.now();"
        "}"
        "loadSettings();"
        "setInterval(refreshCam, 2000);"
        "</script>"
        "</body></html>");
    server.send(200, "text/html", html);
}

void handleCapture() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        server.send(500, "text/plain", "Camera capture failed");
        return;
    }
    server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

// Returns all settings as "key=value&key=value&..."
void handleGet() {
    String s = "vflip="    + String(cam_vflip)                     +
               "&hmirror=" + String(cam_hmirror)                   +
               "&flash="   + String(flash_enabled)                 +
               "&interval=" + String(uploadIntervalMs / 1000UL)    +
               "&res="     + String(frameSize);
    server.send(200, "text/plain", s);
}

void handleSet() {
    bool needReinit = false;

    if (server.hasArg("v")) {
        cam_vflip = server.arg("v").toInt() ? 1 : 0;
        prefs.putInt("vflip", cam_vflip);
    }
    if (server.hasArg("h")) {
        cam_hmirror = server.arg("h").toInt() ? 1 : 0;
        prefs.putInt("hmirror", cam_hmirror);
    }
    if (server.hasArg("flash")) {
        flash_enabled = server.arg("flash").toInt() ? 1 : 0;
        prefs.putInt("flash", flash_enabled);
    }
    if (server.hasArg("interval")) {
        long secs = server.arg("interval").toInt();
        if (secs < 5) secs = 5;
        if (secs > 86400) secs = 86400;
        uploadIntervalMs = (unsigned long)secs * 1000UL;
        prefs.putInt("interval", (int)secs);
    }
    if (server.hasArg("res")) {
        int newRes = server.arg("res").toInt();
        if (newRes != frameSize) {
            frameSize = newRes;
            prefs.putInt("framesize", frameSize);
            needReinit = true;
        }
    }

    if (server.hasArg("v") || server.hasArg("h")) {
        applyOrientation();
    }

    if (needReinit) {
        if (!reinitCamera()) {
            server.send(500, "text/plain", "Camera reinit failed");
            return;
        }
    }

    server.send(200, "text/plain", "OK");
}

void handleReset() {
    cam_vflip        = CAM_VFLIP_DEFAULT;
    cam_hmirror      = CAM_HMIRROR_DEFAULT;
    flash_enabled    = FLASH_DEFAULT;
    uploadIntervalMs = INTERVAL_SECS_DEFAULT * 1000UL;
    frameSize        = FRAMESIZE_DEFAULT;

    prefs.putInt("vflip",     cam_vflip);
    prefs.putInt("hmirror",   cam_hmirror);
    prefs.putInt("flash",     flash_enabled);
    prefs.putInt("interval",  INTERVAL_SECS_DEFAULT);
    prefs.putInt("framesize", frameSize);

    reinitCamera();
    server.send(200, "text/plain", "Factory reset done");
}
#endif

// ===== Main loop =====
unsigned long lastUpload = 0;

void loop() {
#if ENABLE_CALIBRATION_SERVER
    server.handleClient();
#endif
    // 1. Check WiFi connection, reconnect if lost
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastReconnectAttempt = 0;
        if (millis() - lastReconnectAttempt > 10000) {
            Serial.println("WiFi lost, reconnecting...");
            connectToWiFi();
            lastReconnectAttempt = millis();
        }
    } else {
        if (millis() - lastUpload >= uploadIntervalMs) {
            lastUpload = millis();
            Serial.println("Starting periodic upload...");
            // 2. Create a fresh WiFiClient (the connection is managed by the HTTPClient)
            WiFiClient wifi;
            // 3. Capture and upload
            Serial.println("Starting capture cycle...");
            if (sendImage(wifi)) {
                Serial.println("Upload successful");
            } else {
                Serial.println("Upload failed");
            }
        }
    }
    delay(1);
}
