#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <NTPClient.h> // https://github.com/arduino-libraries/NTPClient
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

//
// WARNING!!! PSRAM IC required for UXGA resolution and high JPEG quality
//            Ensure ESP32 Wrover Module or other board with PSRAM is selected
//            Partial images will be transmitted if image exceeds buffer size
//

// Select camera model
//#define CAMERA_MODEL_WROVER_KIT
//#define CAMERA_MODEL_ESP_EYE
//#define CAMERA_MODEL_M5STACK_PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE
#define CAMERA_MODEL_AI_THINKER

#include "camera_pins.h"

#define NUM_LEDS_PER_SEGMENT 1                          // 每個節點的燈珠數
#define NUM_LEDS_HOUR (14 * NUM_LEDS_PER_SEGMENT) + 1   // 包含小時位數及冒號上點
#define NUM_LEDS_MINUTE (28 * NUM_LEDS_PER_SEGMENT) + 3 // 包含分鐘位數及冒號下點，秒數位數及冒號兩點
#define HOUR_PIN 2
#define MINUTE_PIN 14

const char *ssid = "SSID名稱";     //WiFi SSID
const char *password = "SSID密碼"; //WiFi Password

const char *host = "notify-api.line.me"; //LINE Notify API網址
String lineNotifyToken = "LINE Notify金鑰";

// LED
Adafruit_NeoPixel HourPixels = Adafruit_NeoPixel(NUM_LEDS_HOUR, HOUR_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel MinutePixels = Adafruit_NeoPixel(NUM_LEDS_MINUTE, MINUTE_PIN, NEO_GRB + NEO_KHZ800);

// NTP 國家時間與頻率標準實驗室
WiFiUDP ntpUDP;
// timeClient(ntpUDP, NTP主機位置, 時區偏移量(秒), 取得NTP時間的更新間隔(秒));
NTPClient timeClient(ntpUDP, "time.stdtime.gov.tw", 8 * 3600, 1800);

//SSL wificlient
WiFiClientSecure tcpClient;

uint32_t PixelsColor = 0xffffff;

int currentMinute = -1;
int currentHour = -1;

bool isTwelveHour = false;

void startCameraServer();

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if (psramFound())
  {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  }
  else
  {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  //initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID)
  {
    s->set_vflip(s, 1);       //flip it back
    s->set_brightness(s, 1);  //up the blightness just a bit
    s->set_saturation(s, -2); //lower the saturation
  }
  //drop down frame size for higher initial frame rate
  s->set_framesize(s, FRAMESIZE_QVGA);

#if defined(CAMERA_MODEL_M5STACK_WIDE)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  timeClient.begin();
  HourPixels.begin();
  MinutePixels.begin();
  HourPixels.setBrightness(20);
  MinutePixels.setBrightness(20);

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

void loop()
{
  timeClient.update();

  int xt = timeClient.getMinutes();
  if (xt != currentMinute)
  {
    currentMinute = xt;
    drawMinute();
    sendMessage2LineNotify(String(currentMinute));
  }

  xt = timeClient.getHours();
  if (xt != currentHour)
  {
    currentHour = xt;
    drawHour();
  }
}

void sendMessage2LineNotify(String msg)
{
  if (tcpClient.connect(host, 443))
  {
    Serial.println(msg);
    int msgLength = msg.length();
    Serial.println(msgLength);
    // POST表頭
    tcpClient.println("POST /api/notify HTTP/1.1");
    tcpClient.println(String("Host: ") + host);
    tcpClient.println("Authorization: Bearer " + lineNotifyToken);
    tcpClient.println("Content-Type: application/x-www-form-urlencoded");
    tcpClient.println("Content-Length: " + String((msgLength + 10)));
    tcpClient.println();
    tcpClient.println("message=" + msg);
    tcpClient.println();
    tcpClient.stop();
  }
  else
  {
    Serial.println("Connected fail");
  }
}

void drawHour()
{
  HourPixels.clear();

  HourPixels.setPixelColor(0, PixelsColor);

  int displayHour = isTwelveHour ? hourFormat12() : currentHour;
  if (displayHour >= 10 || !isTwelveHour)
  {
    displayHourDigit(7 * NUM_LEDS_PER_SEGMENT + 1, displayHour / 10);
  }
  displayHourDigit(1, displayHour % 10);

  HourPixels.show();
}

void drawMinute()
{
  MinutePixels.clear();

  MinutePixels.setPixelColor(0, PixelsColor);

  displayMinuteDigit(1, (currentMinute < 10) ? 0 : currentMinute / 10);
  displayMinuteDigit(7 * NUM_LEDS_PER_SEGMENT + 1, currentMinute % 10);

  MinutePixels.show();
}

void fillHourSegment(int offset, int segmentIndex)
{
  HourPixels.fill(PixelsColor, offset + segmentIndex * NUM_LEDS_PER_SEGMENT, NUM_LEDS_PER_SEGMENT);
}

void fillMinuteSegment(int offset, int segmentIndex)
{
  MinutePixels.fill(PixelsColor, offset + segmentIndex * NUM_LEDS_PER_SEGMENT, NUM_LEDS_PER_SEGMENT);
}

void displayHourDigit(int offset, int digit)
{
  switch (digit)
  {
  case 0:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 2);
    fillHourSegment(offset, 4);
    fillHourSegment(offset, 5);
    fillHourSegment(offset, 6);
    break;
  case 1:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 4);
    break;
  case 2:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 5);
    fillHourSegment(offset, 6);
    break;
  case 3:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 4);
    fillHourSegment(offset, 5);
    break;
  case 4:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 2);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 4);
    break;
  case 5:
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 2);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 4);
    fillHourSegment(offset, 5);
    break;
  case 6:
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 2);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 4);
    fillHourSegment(offset, 5);
    fillHourSegment(offset, 6);
    break;
  case 7:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 4);
    break;
  case 8:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 2);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 4);
    fillHourSegment(offset, 5);
    fillHourSegment(offset, 6);
    break;
  case 9:
    fillHourSegment(offset, 0);
    fillHourSegment(offset, 1);
    fillHourSegment(offset, 2);
    fillHourSegment(offset, 3);
    fillHourSegment(offset, 4);
    fillHourSegment(offset, 5);
    break;
  }
}

void displayMinuteDigit(int offset, int digit)
{
  switch (digit)
  {
  case 0:
    fillMinuteSegment(offset, 0);
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 4);
    fillMinuteSegment(offset, 5);
    fillMinuteSegment(offset, 6);
    break;
  case 1:
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 6);
    break;
  case 2:
    fillMinuteSegment(offset, 0);
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 5);
    fillMinuteSegment(offset, 6);
    break;
  case 3:
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 5);
    fillMinuteSegment(offset, 6);
    break;
  case 4:
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 4);
    fillMinuteSegment(offset, 6);
    break;
  case 5:
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 4);
    fillMinuteSegment(offset, 5);
    break;
  case 6:
    fillMinuteSegment(offset, 0);
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 4);
    fillMinuteSegment(offset, 5);
    break;
  case 7:
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 5);
    fillMinuteSegment(offset, 6);
    break;
  case 8:
    fillMinuteSegment(offset, 0);
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 4);
    fillMinuteSegment(offset, 5);
    fillMinuteSegment(offset, 6);
    break;
  case 9:
    fillMinuteSegment(offset, 1);
    fillMinuteSegment(offset, 2);
    fillMinuteSegment(offset, 3);
    fillMinuteSegment(offset, 4);
    fillMinuteSegment(offset, 5);
    fillMinuteSegment(offset, 6);
    break;
  }
}

int hourFormat12()
{
  int x = timeClient.getHours();
  if (x > 12)
  {
    x = x - 12;
  }
  return x;
}

void printTime()
{
  // Serial.print(isTwelveHour ? hourFormat12() : timeClient.getHours());
  Serial.print(isTwelveHour ? hourFormat12() : currentHour);
  Serial.print(":");
  printDigits(currentMinute);
  Serial.println("");
}

void printDigits(int digits)
{
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}