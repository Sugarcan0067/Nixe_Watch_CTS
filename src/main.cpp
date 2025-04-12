#include <ArduinoBLE.h>
#include <TaskScheduler.h>

// CTS 服務和特性的 UUID
const char *ctsServiceUUID = "00001805-0000-1000-8000-00805F9B34FB";
const char *currentTimeCharUUID = "00002A2B-0000-1000-8000-00805F9B34FB";
const char *localTimeInfoCharUUID = "00002A0F-0000-1000-8000-00805F9B34FB";
const char *refTimeInfoCharUUID = "00002A14-0000-1000-8000-00805F9B34FB";

// 建立服務和特性
BLEService ctsService(ctsServiceUUID);
BLECharacteristic currentTimeChar(currentTimeCharUUID,
                                  BLERead | BLENotify,
                                  10); // 時間數據長度為 10 字節
BLECharacteristic localTimeInfoChar(localTimeInfoCharUUID,
                                    BLERead,
                                    2); // 本地時間信息長度為 2 字節
BLECharacteristic refTimeInfoChar(refTimeInfoCharUUID,
                                  BLERead,
                                  4); // 參考時間信息長度為 4 字節

// 定義簡易時間結構
struct DateTime
{
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t dayOfWeek; // 1=周一, 7=周日
};

// 全局變數
DateTime currentDateTime = {2023, 10, 10, 12, 0, 0, 2}; // 初始時間設為 2023-10-10 12:00:00 周二
bool bleInitialized = false;
unsigned long lastTimeUpdate = 0;

// 定義 LED 腳位
#define LED_PIN 15 // P0.15 腳位

// 中央設備連接狀態
bool centralConnected = false;
BLEDevice connectedCentral;

// 設置 TaskScheduler
Scheduler ts;

// 任務聲明 (增加數據更新任務)
void blinkLedCallback();
void updateTimeCallback();
void bleServerCallback();
void updateServiceDataCallback();

// 任務定義 (增加數據更新任務)
Task tLedBlink(500, TASK_FOREVER, &blinkLedCallback, &ts, true);
Task tUpdateTime(1000, TASK_FOREVER, &updateTimeCallback, &ts, true);
Task tBleServer(100, TASK_FOREVER, &bleServerCallback, &ts, true);
Task tUpdateServiceData(1000, TASK_FOREVER, &updateServiceDataCallback, &ts, true);

// LED 狀態
bool ledState = false;

// 更新當前時間
void updateInternalTime()
{
  // 每秒更新一次時間
  unsigned long currentMillis = millis();
  unsigned long elapsed = currentMillis - lastTimeUpdate;

  if (elapsed >= 1000)
  {
    // 計算經過的秒數
    uint32_t secondsElapsed = elapsed / 1000;
    lastTimeUpdate = currentMillis - (elapsed % 1000); // 保留剩餘的毫秒數

    // 更新秒數
    currentDateTime.second += secondsElapsed;

    // 進位處理
    if (currentDateTime.second >= 60)
    {
      currentDateTime.minute += currentDateTime.second / 60;
      currentDateTime.second %= 60;

      if (currentDateTime.minute >= 60)
      {
        currentDateTime.hour += currentDateTime.minute / 60;
        currentDateTime.minute %= 60;

        if (currentDateTime.hour >= 24)
        {
          currentDateTime.day += currentDateTime.hour / 24;
          currentDateTime.hour %= 24;

          // 更新星期幾
          currentDateTime.dayOfWeek = ((currentDateTime.dayOfWeek - 1) + (currentDateTime.hour / 24)) % 7 + 1;

          // 簡化的月份天數處理 (不考慮閏年)
          uint8_t daysInMonth = 31; // 默認
          if (currentDateTime.month == 4 || currentDateTime.month == 6 ||
              currentDateTime.month == 9 || currentDateTime.month == 11)
          {
            daysInMonth = 30;
          }
          else if (currentDateTime.month == 2)
          {
            // 簡單的閏年處理
            daysInMonth = ((currentDateTime.year % 4 == 0 && currentDateTime.year % 100 != 0) ||
                           (currentDateTime.year % 400 == 0))
                              ? 29
                              : 28;
          }

          if (currentDateTime.day > daysInMonth)
          {
            currentDateTime.day = 1;
            currentDateTime.month++;

            if (currentDateTime.month > 12)
            {
              currentDateTime.month = 1;
              currentDateTime.year++;
            }
          }
        }
      }
    }
  }
}

// 更新 CTS 特性的時間數據
void updateCurrentTime()
{
  updateInternalTime(); // 先更新內部時間

  uint8_t timeData[10];
  // 按照 CTS 規範格式化時間數據
  // 年份 (小端儲存)
  timeData[0] = currentDateTime.year & 0xFF;
  timeData[1] = (currentDateTime.year >> 8) & 0xFF;
  // 月份 (1-12)
  timeData[2] = currentDateTime.month;
  // 日期 (1-31)
  timeData[3] = currentDateTime.day;
  // 星期幾 (1-7, 1=周一)
  timeData[4] = currentDateTime.dayOfWeek;
  // 時 (0-23)
  timeData[5] = currentDateTime.hour;
  // 分 (0-59)
  timeData[6] = currentDateTime.minute;
  // 秒 (0-59)
  timeData[7] = currentDateTime.second;
  // 精確度/原因 (一般為 0x00)
  timeData[8] = 0x00;
  // 日/時調整索引
  timeData[9] = 0x00;

  // 更新特性值
  currentTimeChar.writeValue(timeData, sizeof(timeData));
}

// 更新時區和DST信息
void updateLocalTimeInfo()
{
  uint8_t localTimeData[2];

  // 時區 (UTC+8 = +8*4 = +32 (15分鐘為單位))
  int8_t timeZone = 32; // +8:00

  // DST偏移量 (0 = 標準時間, 1 = +1小時, etc.)
  uint8_t dstOffset = 0; // 不使用夏令時

  localTimeData[0] = timeZone;
  localTimeData[1] = dstOffset;

  localTimeInfoChar.writeValue(localTimeData, sizeof(localTimeData));
}

// 更新參考時間信息
void updateRefTimeInfo()
{
  uint8_t refTimeData[4];

  // 參考時間源 (0x01 = 網絡時間協議, 0x02 = GPS, 0x03 = 射頻, 0x04 = 手動, 0x05 = 原子鐘, 0x06 = 蜂窩網)
  refTimeData[0] = 0x04; // 手動設置

  // 準確度 (低4位: 準確度, 高4位: 估計誤差)
  refTimeData[1] = 0x04; // 100 ppm

  // 每天源的偏移天數 (0x00 表示未知)
  refTimeData[2] = 0x00;

  // 每天源的偏移時間 (24小時制，以1/256秒為單位的偏移)
  refTimeData[3] = 0x00;

  refTimeInfoChar.writeValue(refTimeData, sizeof(refTimeData));
}

// LED 閃爍任務回調
void blinkLedCallback()
{
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  Serial.println(ledState ? "LED ON" : "LED OFF");
}

// 時間更新任務回調
void updateTimeCallback()
{
  updateInternalTime();
  Serial.print("Current Time: ");
  Serial.print(currentDateTime.year);
  Serial.print("-");
  Serial.print(currentDateTime.month);
  Serial.print("-");
  Serial.print(currentDateTime.day);
  Serial.print(" ");
  Serial.print(currentDateTime.hour);
  Serial.print(":");
  Serial.print(currentDateTime.minute);
  Serial.print(":");
  Serial.println(currentDateTime.second);
}

// BLE 服務器任務回調
void bleServerCallback()
{
  // 如果 BLE 尚未初始化
  if (!bleInitialized)
  {
    if (BLE.begin())
    {
      bleInitialized = true;

      // 設置 BLE 名稱
      BLE.setLocalName("Arduino CTS Server");
      BLE.setDeviceName("Arduino CTS Server");

      // 添加服務
      BLE.setAdvertisedService(ctsService);
      ctsService.addCharacteristic(currentTimeChar);
      ctsService.addCharacteristic(localTimeInfoChar);
      ctsService.addCharacteristic(refTimeInfoChar);
      BLE.addService(ctsService);

      // 初始更新時間和相關信息
      updateCurrentTime();
      updateLocalTimeInfo();
      updateRefTimeInfo();

      // 開始廣播
      BLE.advertise();
      Serial.println("BLE CTS Server is advertising");
      Serial.print("Local MAC address: ");
      Serial.println(BLE.address());
    }
    else
    {
      Serial.println("Starting BLE failed!");
      return;
    }
  }

  // 檢查是否有已連接的中央設備
  if (centralConnected)
  {
    // 檢查連接是否仍然存在
    if (!connectedCentral.connected())
    {
      Serial.print("Disconnected from central: ");
      Serial.println(connectedCentral.address());
      centralConnected = false;
      // 重新開始廣播
      BLE.advertise();
    }
    else
    {
      // 定期更新時間特性 (不使用delay，改由TaskScheduler控制)
      updateCurrentTime();
    }
  }
  else
  {
    // 檢查是否有新的中央設備連接
    BLEDevice central = BLE.central();
    if (central)
    {
      connectedCentral = central;
      centralConnected = true;
      Serial.print("Connected to central: ");
      Serial.println(central.address());
      Serial.println("Sending initial time information...");

      // 連接後立即發送時間資訊
      updateCurrentTime();
      updateLocalTimeInfo();
      updateRefTimeInfo();
    }
  }
}

// 添加一個新的任務專門處理數據更新
void updateServiceDataCallback()
{
  if (centralConnected)
  {
    // 更新時間特性
    updateCurrentTime();
  }
}

void setup()
{
  Serial.begin(9600);
  while (!Serial)
    ;

  Serial.println("Starting CTS Server Example");

  // 初始化 P0.14 腳位
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // 起始狀態為關閉

  // 初始化時間更新變數
  lastTimeUpdate = millis();

  // 初始化連接狀態
  centralConnected = false;
}

void loop()
{
  // 執行任務調度器
  ts.execute();
}