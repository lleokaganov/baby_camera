// Baby monitor: AI-Thinker ESP32-CAM (HW-297) + INMP441 I2S microphone.
//
// Audio and video are deliberately two independent streams:
//   :82/audio  - endless WAV, always running
//   :81/stream - MJPEG, only while somebody is watching
//   :80/       - page, /level, /set
//
// MJPEG cannot carry sound at all (it is just concatenated JPEGs), and a baby
// monitor does not need lip sync - it needs to be heard. Keeping the two apart
// also means a stalled video connection cannot silence the microphone.
//
// The camera driver owns I2S0 (it clocks the parallel DVP bus), so the
// microphone runs on I2S1. They do not collide.

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>

#include "esp_camera.h"
#include "esp_http_server.h"
#include "driver/i2s.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/stream_buffer.h"

#include "config.h"
#include "page.h"

// ----------------------------------------------------------------- camera pins
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ----------------------------------------------------------------- state
static StreamBufferHandle_t s_audio = nullptr;
static volatile float       s_level = 0.0f;   // 0..1 RMS of the last block
static volatile int         s_gain  = MIC_DEFAULT_GAIN;

// Diagnostics. "mic ok" at boot only proves the driver installed; it says
// nothing about whether the bus actually carries data. These separate the
// three ways silence can happen: i2s_read never returning (reads stays 0),
// the data line stuck at a constant (rawMin == rawMax), or a genuinely quiet
// microphone (raw swings, level near zero).
static volatile uint32_t s_reads   = 0;
static volatile int32_t  s_rawMin  = 0;
static volatile int32_t  s_rawMax  = 0;
static volatile uint32_t s_readErr = 0;

// Which half of the I2S frame to capture. Switchable at runtime because the
// ESP32 receiver is known to disagree with the datasheets about which slot
// "left" is, and reflashing once per guess is not a debugging method.
// 0 = ONLY_LEFT, 1 = ONLY_RIGHT.
static volatile int s_chan    = MIC_DEFAULT_CHAN;
static volatile int s_chanReq = -1;

// How far to shift the 32-bit slot down to 16 bits. 16 is correct for a
// 24-bit left-justified sample; SPH0645-class parts need a different value.
static volatile int s_shift = MIC_DEFAULT_SHIFT;

static httpd_handle_t s_ui = nullptr, s_video = nullptr, s_sound = nullptr;

// ----------------------------------------------------------------- WAV header
// The stream never ends, so the length fields are a lie either way. 0xFFFFFFFF
// is the obvious lie, but several browsers read it as -1 and refuse to start
// decoding while happily going on downloading -- a silent failure that looks
// exactly like a dead microphone. 0x7FFFFFFF is a lie they believe.
struct __attribute__((packed)) WavHeader {
  char     riff[4];
  uint32_t riffSize;
  char     wave[4];
  char     fmt[4];
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char     data[4];
  uint32_t dataSize;
};

static void buildWavHeader(WavHeader *h) {
  memcpy(h->riff, "RIFF", 4);
  h->riffSize = 0x7FFFFFFF;
  memcpy(h->wave, "WAVE", 4);
  memcpy(h->fmt, "fmt ", 4);
  h->fmtSize       = 16;
  h->audioFormat   = 1;             // PCM
  h->numChannels   = 1;
  h->sampleRate    = MIC_SAMPLE_RATE;
  h->bitsPerSample = 16;
  h->blockAlign    = h->numChannels * h->bitsPerSample / 8;
  h->byteRate      = h->sampleRate * h->blockAlign;
  memcpy(h->data, "data", 4);
  h->dataSize = 0x7FFFFFFF;
}

// ----------------------------------------------------------------- microphone
static bool micInit(int chan) {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = MIC_SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441 sends 24 in 32
  cfg.channel_format       = chan ? I2S_CHANNEL_FMT_ONLY_RIGHT
                                  : I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = MIC_BLOCK;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = false;
  cfg.fixed_mclk           = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = MIC_SCK_PIN;
  pins.ws_io_num    = MIC_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_SD_PIN;

  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) return false;
  s_chan = chan;
  return true;
}

// Always running, whether anyone listens or not: that keeps the level meter
// live and means a new listener hears sound immediately instead of waiting
// for the DMA chain to fill.
static void micTask(void *) {
  static int32_t raw[MIC_BLOCK];
  static int16_t pcm[MIC_BLOCK];

  for (;;) {
    // Re-install from inside this task: doing it from the HTTP handler would
    // tear the driver out from under a concurrent i2s_read.
    if (s_chanReq >= 0) {
      int want = s_chanReq; s_chanReq = -1;
      i2s_driver_uninstall(I2S_NUM_1);
      micInit(want);
      s_reads = 0; s_readErr = 0; s_rawMin = 0; s_rawMax = 0;
    }

    size_t got = 0;
    // Bounded wait: portMAX_DELAY would hide a peripheral that never delivers,
    // and the whole point here is to be able to see that case.
    esp_err_t err = i2s_read(I2S_NUM_1, raw, sizeof(raw), &got, pdMS_TO_TICKS(500));
    const int n = (err == ESP_OK) ? (int)(got / sizeof(int32_t)) : 0;
    if (err != ESP_OK) s_readErr++;
    if (n <= 0) {
        // Both failure paths MUST sleep. i2s_read can return instantly when the
        // peripheral delivers nothing, and a bare `continue` then spins at task
        // priority forever -- which starved the HTTP servers sharing this core
        // and made the whole board look hung while it still answered pings.
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
    }
    s_reads++;

    int32_t lo = raw[0], hi = raw[0];
    for (int i = 1; i < n; i++) {
        if (raw[i] < lo) lo = raw[i];
        if (raw[i] > hi) hi = raw[i];
    }
    s_rawMin = lo; s_rawMax = hi;

    const int gain = s_gain;
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) {
      // 24-bit sample sits left-justified in the slot; >>16 takes the top 16,
      // then software gain, because the INMP441 is quiet at conversational
      // distance and we want to hear breathing, not just crying.
      int32_t v = (raw[i] >> s_shift) * gain;
      if (v >  32767) v =  32767;
      if (v < -32768) v = -32768;
      pcm[i] = (int16_t)v;
      acc += (uint32_t)(v * v);
    }
    s_level = sqrtf((float)(acc / n)) / 32768.0f;

    // Never block: with no listener the buffer is full and samples are dropped
    // on purpose. Blocking here would stall the meter too.
    xStreamBufferSend(s_audio, pcm, n * sizeof(int16_t), 0);
  }
}

// ----------------------------------------------------------------- lamp
// GPIO4 drives the bright white flash LED. Dimmable rather than on/off: at full
// power it is a camera flash, far too harsh to point at a cot.
// LEDC channel 4, because the camera driver holds channel 0 for the XCLK.
#define LAMP_PIN      4
#define LAMP_CHANNEL  4
static volatile int s_lamp = 0;   // 0..255

static void lampInit() {
  ledcSetup(LAMP_CHANNEL, 5000, 8);
  ledcAttachPin(LAMP_PIN, LAMP_CHANNEL);
  ledcWrite(LAMP_CHANNEL, 0);
}

static void lampSet(int v) {
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  s_lamp = v;
  ledcWrite(LAMP_CHANNEL, v);
}

// ----------------------------------------------------------------- handlers
static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t levelHandler(httpd_req_t *req) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"level\":%.4f,\"gain\":%d,\"uptime\":%lu,\"rssi\":%d,\"heap\":%u,"
           "\"reads\":%lu,\"readErr\":%lu,\"rawMin\":%ld,\"rawMax\":%ld,\"lamp\":%d,\"chan\":%d,\"shift\":%d}",
           s_level, s_gain, (unsigned long)(millis() / 1000),
           WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
           (unsigned long)s_reads, (unsigned long)s_readErr,
           (long)s_rawMin, (long)s_rawMax, s_lamp, s_chan, s_shift);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setHandler(httpd_req_t *req) {
  char q[64], v[16];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "gain", v, sizeof(v)) == ESP_OK) {
    int g = atoi(v);
    if (g >= 1 && g <= 64) s_gain = g;
  }
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "lamp", v, sizeof(v)) == ESP_OK) {
    lampSet(atoi(v));
  }
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "chan", v, sizeof(v)) == ESP_OK) {
    s_chanReq = atoi(v) ? 1 : 0;
  }
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "shift", v, sizeof(v)) == ESP_OK) {
    int sh = atoi(v);
    if (sh >= 8 && sh <= 24) s_shift = sh;
  }
  return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t audioHandler(httpd_req_t *req) {
  WavHeader h;
  buildWavHeader(&h);

  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (httpd_resp_send_chunk(req, (const char *)&h, sizeof(h)) != ESP_OK) return ESP_FAIL;

  // Start from silence rather than from whatever has been sitting in the ring.
  xStreamBufferReset(s_audio);

  uint8_t buf[1024];
  for (;;) {
    size_t n = xStreamBufferReceive(s_audio, buf, sizeof(buf), pdMS_TO_TICKS(300));
    if (n == 0) {
        // Send real silence rather than looping. A handler that never writes
        // never learns that the client has gone, and since this server runs
        // one request at a time it would hold the port shut for everybody.
        memset(buf, 0, 256);
        n = 256;
    }
    if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) break;
  }
  return ESP_OK;
}

#define BOUNDARY "frame"
static esp_err_t streamHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part[80];
  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    int len = snprintf(part, sizeof(part),
                       "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                       "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
    esp_err_t r = httpd_resp_send_chunk(req, part, len);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (r != ESP_OK) break;
  }
  return ESP_OK;
}

// ----------------------------------------------------------------- servers
// Three servers, not one. esp_http_server processes requests serially in a
// single task, so one endless handler would block every other endpoint.
// Each instance also needs its own ctrl_port - sharing it silently fails.
//
// lru_purge_enable is not optional here. Without it, once max_open_sockets is
// reached the server accepts further connections and closes them with no
// response at all -- every endpoint appears dead while the board still pings
// happily. A couple of forgotten browser tabs are enough to reach that state,
// because the page holds an audio stream and polls four times a second.
static void startServers() {
  httpd_uri_t uIndex = {"/",      HTTP_GET, indexHandler,  nullptr};
  httpd_uri_t uLevel = {"/level", HTTP_GET, levelHandler,  nullptr};
  httpd_uri_t uSet   = {"/set",   HTTP_GET, setHandler,    nullptr};
  httpd_uri_t uSnd   = {"/audio", HTTP_GET, audioHandler,  nullptr};
  httpd_uri_t uVid   = {"/stream",HTTP_GET, streamHandler, nullptr};

  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80; c.ctrl_port = 32768; c.max_uri_handlers = 4;
  c.lru_purge_enable = true; c.max_open_sockets = 7;
  if (httpd_start(&s_ui, &c) == ESP_OK) {
    httpd_register_uri_handler(s_ui, &uIndex);
    httpd_register_uri_handler(s_ui, &uLevel);
    httpd_register_uri_handler(s_ui, &uSet);
  }

  c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 81; c.ctrl_port = 32769;
  c.lru_purge_enable = true; c.max_open_sockets = 3;
  if (httpd_start(&s_video, &c) == ESP_OK) httpd_register_uri_handler(s_video, &uVid);

  c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 82; c.ctrl_port = 32770;
  c.lru_purge_enable = true; c.max_open_sockets = 3;
  if (httpd_start(&s_sound, &c) == ESP_OK) httpd_register_uri_handler(s_sound, &uSnd);
}

// ----------------------------------------------------------------- setup
static bool cameraInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;   c.pin_pclk  = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href  = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;   c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = CAM_FRAME_SIZE;
  c.jpeg_quality = CAM_JPEG_QUALITY;
  c.fb_count     = psramFound() ? 2 : 1;
  c.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  // Always hand out the newest frame; a queue of stale frames only adds lag.
  c.grab_mode    = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

void setup() {
  // The 5 V regulator on these boards sags when the camera and WiFi start
  // together, and the brownout detector reboots a board that would otherwise
  // run fine. Standard practice on ESP32-CAM.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n[babycam] boot");

  lampInit();

  if (!cameraInit()) Serial.println("[babycam] camera init FAILED");
  else               Serial.println("[babycam] camera ok");

  s_audio = xStreamBufferCreate(AUDIO_RING_BYTES, sizeof(int16_t) * MIC_BLOCK);
  if (!micInit(MIC_DEFAULT_CHAN)) Serial.println("[babycam] mic init FAILED");
  else                 Serial.println("[babycam] mic ok");

  // Microphone on core 1, away from the WiFi/LwIP stack on core 0.
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 5, nullptr, 1);

  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);   // power saving adds seconds of latency to a stream

  Serial.print("[babycam] wifi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print('.'); }
  Serial.printf("\n[babycam] http://%s/\n", WiFi.localIP().toString().c_str());

  startServers();
}

void loop() {
  // Everything runs in tasks. Reconnect if the access point drops.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[babycam] wifi lost, reconnecting");
    WiFi.reconnect();
    delay(2000);
  }
  delay(1000);
}
