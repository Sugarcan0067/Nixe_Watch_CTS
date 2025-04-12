#include <ArduinoBLE.h>
#include <TaskScheduler.h>

// --- Configuration ---
#define DEVICE_NAME "S&B Watch"
#define LED_PIN LED_BUILTIN // 使用內建 LED

// --- CTS UUIDs ---
const char *ctsServiceUUID = "00001805-0000-1000-8000-00805F9B34FB";
const char *currentTimeCharUUID = "00002A2B-0000-1000-8000-00805F9B34FB";
const char *localTimeInfoCharUUID = "00002A0F-0000-1000-8000-00805F9B34FB";
const char *refTimeInfoCharUUID = "00002A14-0000-1000-8000-00805F9B34FB";

// --- BLE Service and Characteristics ---
BLEService ctsService(ctsServiceUUID);
// Add BLEWrite permission to currentTimeChar
BLECharacteristic currentTimeChar(currentTimeCharUUID, BLERead | BLENotify | BLEWrite, 10); // 10 bytes for Current Time
BLECharacteristic localTimeInfoChar(localTimeInfoCharUUID, BLERead, 2);                     // 2 bytes for Local Time Information
BLECharacteristic refTimeInfoChar(refTimeInfoCharUUID, BLERead, 4);                         // 4 bytes for Reference Time Information

// --- Time Structure ---
struct DateTime
{
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t dayOfWeek; // 1 = Monday, 7 = Sunday
};

// --- Global Variables ---
DateTime currentDateTime = {2024, 1, 1, 0, 0, 0, 1}; // Initial time: 2024-01-01 00:00:00 Monday
unsigned long lastTimeUpdateMillis = 0;
bool centralConnected = false;
BLEDevice connectedCentral;
bool ledState = false;

// --- Task Scheduler ---
Scheduler ts;

// --- Task Declarations ---
void blinkLedCallback();
void updateInternalTimeCallback();
void updateBleDataCallback();
void blePollCallback();         // Task for BLE polling
void printSystemTimeCallback(); // New task declaration

// --- Task Definitions ---
Task tLedBlink(1000, TASK_FOREVER, &blinkLedCallback, &ts, true);             // Blink LED every 1000ms (slower)
Task tUpdateTime(1000, TASK_FOREVER, &updateInternalTimeCallback, &ts, true); // Update internal time every second
Task tUpdateBleData(1500, TASK_FOREVER, &updateBleDataCallback, &ts, true);   // Update BLE characteristics every 1.5 seconds if connected (slower)
Task tBlePoll(5, TASK_FOREVER, &blePollCallback, &ts, true);                  // Poll BLE events more frequently (every 5ms)
Task tPrintTime(5000, TASK_FOREVER, &printSystemTimeCallback, &ts, true);     // New task: Print system time every 5 seconds

// --- Function Implementations ---

// Calculate days in a given month and year
uint8_t daysInMonth(uint16_t year, uint8_t month)
{
  if (month == 4 || month == 6 || month == 9 || month == 11)
  {
    return 30;
  }
  else if (month == 2)
  {
    // Leap year check
    bool isLeap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    return isLeap ? 29 : 28;
  }
  else
  {
    return 31;
  }
}

// Update internal time structure
void updateInternalTime()
{
  unsigned long currentMillis = millis();
  if (currentMillis - lastTimeUpdateMillis >= 1000)
  {
    unsigned long elapsedSeconds = (currentMillis - lastTimeUpdateMillis) / 1000;
    lastTimeUpdateMillis += elapsedSeconds * 1000;

    currentDateTime.second += elapsedSeconds;

    // Handle rollovers
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
          uint8_t daysElapsed = currentDateTime.hour / 24;
          currentDateTime.day += daysElapsed;
          currentDateTime.hour %= 24;
          currentDateTime.dayOfWeek = ((currentDateTime.dayOfWeek - 1 + daysElapsed) % 7) + 1;

          uint8_t monthDays = daysInMonth(currentDateTime.year, currentDateTime.month);
          while (currentDateTime.day > monthDays)
          {
            currentDateTime.day -= monthDays;
            currentDateTime.month++;
            if (currentDateTime.month > 12)
            {
              currentDateTime.month = 1;
              currentDateTime.year++;
            }
            monthDays = daysInMonth(currentDateTime.year, currentDateTime.month);
          }
        }
      }
    }
    // Serial.print("."); // Indicate time update
  }
}

// Format and write Current Time characteristic data
void writeCurrentTime()
{
  uint8_t timeData[10];
  timeData[0] = currentDateTime.year & 0xFF;
  timeData[1] = (currentDateTime.year >> 8) & 0xFF;
  timeData[2] = currentDateTime.month;
  timeData[3] = currentDateTime.day;
  timeData[4] = currentDateTime.hour;
  timeData[5] = currentDateTime.minute;
  timeData[6] = currentDateTime.second;
  timeData[7] = currentDateTime.dayOfWeek;
  timeData[8] = 0; // Fractions256 - We don't support setting this via write
  timeData[9] = 1; // Adjust Reason: Manual time update

  // Check if writeValue was successful (optional, but good for debugging)
  if (!currentTimeChar.writeValue(timeData, sizeof(timeData)))
  {
    Serial.println("Error writing Current Time characteristic!");
  }
  // Serial.println("Current Time Characteristic Updated");
}

// Write Local Time Information characteristic data (Example: UTC+8, no DST)
void writeLocalTimeInfo()
{
  uint8_t localTimeData[2];
  int8_t timeZone = 32;  // UTC+8 (8 * 4 quarters)
  uint8_t dstOffset = 0; // Standard Time

  localTimeData[0] = timeZone;
  localTimeData[1] = dstOffset;
  localTimeInfoChar.writeValue(localTimeData, sizeof(localTimeData));
  // Serial.println("Local Time Info Characteristic Updated");
}

// Write Reference Time Information characteristic data (Example: Manual source)
void writeRefTimeInfo()
{
  uint8_t refTimeData[4];
  refTimeData[0] = 4;   // Source: Manual
  refTimeData[1] = 254; // Accuracy: Inaccurate (within 5s) or use a specific value if known
  refTimeData[2] = 0;   // Days since update (unknown)
  refTimeData[3] = 0;   // Fractions256 since update (unknown)
  refTimeInfoChar.writeValue(refTimeData, sizeof(refTimeData));
  // Serial.println("Reference Time Info Characteristic Updated");
}

// --- Task Callbacks ---

void blinkLedCallback()
{
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState);
}

void updateInternalTimeCallback()
{
  updateInternalTime();
  // Optional: Print time to Serial for debugging
  // Serial.printf("%04d-%02d-%02d %02d:%02d:%02d DOW:%d\n",
  //               currentDateTime.year, currentDateTime.month, currentDateTime.day,
  //               currentDateTime.hour, currentDateTime.minute, currentDateTime.second,
  //               currentDateTime.dayOfWeek);
}

void updateBleDataCallback()
{
  if (centralConnected)
  {
    // Update internal time first to ensure latest value is sent
    updateInternalTime();
    // Write the current time characteristic
    writeCurrentTime();
  }
}

void blePollCallback()
{
  BLE.poll(); // Process BLE events
}

// New Task Callback: Print current system time every 5 seconds
void printSystemTimeCallback()
{
  // Ensure internal time is updated before printing
  updateInternalTime();

  // Format the time string
  char timeBuffer[50];
  snprintf(timeBuffer, sizeof(timeBuffer), "System Time: %04d-%02d-%02d %02d:%02d:%02d DOW:%d",
           currentDateTime.year, currentDateTime.month, currentDateTime.day,
           currentDateTime.hour, currentDateTime.minute, currentDateTime.second,
           currentDateTime.dayOfWeek);
  // Print the formatted time
  Serial.println(timeBuffer);
}

// --- BLE Event Handlers ---

// Handler for when the Current Time characteristic is written by a client
void currentTimeWrittenHandler(BLEDevice central, BLECharacteristic characteristic)
{
  Serial.print("Current Time characteristic written by: ");
  Serial.println(central.address());

  if (characteristic.valueLength() == 10)
  {
    const uint8_t *data = characteristic.value();

    // Print the raw received data
    Serial.print("  Raw Data Received: [");
    for (int i = 0; i < 10; i++)
    {
      Serial.print("0x");
      if (data[i] < 0x10)
        Serial.print("0"); // Add leading zero for single digit hex
      Serial.print(data[i], HEX);
      if (i < 9)
        Serial.print(", ");
    }
    Serial.println("]");

    // Parse the received data according to CTS Current Time format
    uint16_t year = data[0] | (data[1] << 8);
    uint8_t month = data[2];
    uint8_t day = data[3];
    uint8_t hour = data[4];
    uint8_t minute = data[5];
    uint8_t second = data[6];
    uint8_t dayOfWeek = data[7];
    // uint8_t fractions256 = data[8]; // We ignore fractions and adjust reason on write
    // uint8_t adjustReason = data[9];

    // Basic validation (optional but recommended)
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
        hour <= 23 && minute <= 59 && second <= 59 &&
        dayOfWeek >= 1 && dayOfWeek <= 7)
    {
      // Update the internal time structure
      currentDateTime.year = year;
      currentDateTime.month = month;
      currentDateTime.day = day;
      currentDateTime.hour = hour;
      currentDateTime.minute = minute;
      currentDateTime.second = second;
      currentDateTime.dayOfWeek = dayOfWeek;

      // Reset the internal time update mechanism to sync with the new time
      lastTimeUpdateMillis = millis();

      Serial.println("Internal time updated by client:");
      // Use snprintf to format the string into a buffer, then print the buffer
      char timeBuffer[50]; // Create a buffer to hold the formatted string
      snprintf(timeBuffer, sizeof(timeBuffer), "  New Time: %04d-%02d-%02d %02d:%02d:%02d DOW:%d",
               currentDateTime.year, currentDateTime.month, currentDateTime.day,
               currentDateTime.hour, currentDateTime.minute, currentDateTime.second,
               currentDateTime.dayOfWeek);
      Serial.println(timeBuffer); // Print the buffer content

      // Optional: Immediately write the value back to confirm/notify (if needed)
      // writeCurrentTime(); // Already handled by the periodic update task
    }
    else
    {
      Serial.println("Received invalid time data format.");
    }
  }
  else
  {
    Serial.print("Received data with incorrect length: ");
    Serial.println(characteristic.valueLength());
    // Optionally print the incorrect data as well
    const uint8_t *data = characteristic.value();
    int len = characteristic.valueLength();
    Serial.print("  Raw Data Received: [");
    for (int i = 0; i < len; i++)
    {
      Serial.print("0x");
      if (data[i] < 0x10)
        Serial.print("0");
      Serial.print(data[i], HEX);
      if (i < len - 1)
        Serial.print(", ");
    }
    Serial.println("]");
  }
}

void blePeripheralConnectHandler(BLEDevice central)
{
  Serial.print("Connected event for: ");
  Serial.println(central.address());
  digitalWrite(LED_PIN, HIGH); // Turn LED on when connected
  ledState = HIGH;
  tLedBlink.disable(); // Stop blinking when connected

  // Check if already connected to avoid race conditions
  if (!centralConnected)
  {
    centralConnected = true;
    connectedCentral = central; // Store the connected device
    Serial.println("Connection established.");

    // Update characteristics immediately on connection
    // Delay slightly before writing to allow connection stabilization
    delay(50); // Small delay
    writeCurrentTime();
    delay(10);
    writeLocalTimeInfo();
    delay(10);
    writeRefTimeInfo();
    Serial.println("Initial characteristics sent.");
  }
  else
  {
    Serial.println("Already connected, ignoring duplicate connect event.");
  }
}

void blePeripheralDisconnectHandler(BLEDevice central)
{
  Serial.print("Disconnected event for: ");
  Serial.println(central.address());
  digitalWrite(LED_PIN, LOW); // Turn LED off when disconnected
  ledState = LOW;

  // Only restart blinking and advertising if we were actually connected
  if (centralConnected)
  {
    centralConnected = false;
    tLedBlink.enable(); // Start blinking again
    Serial.println("Connection terminated.");

    // Explicitly stop advertising before restarting
    BLE.stopAdvertise();
    Serial.println("Stopped advertising.");
    delay(100); // Short delay before restarting

    // Restart advertising
    if (BLE.advertise())
    {
      Serial.println("Restarted advertising.");
    }
    else
    {
      Serial.println("Failed to restart advertising!");
      // Consider a more robust recovery like resetting BLE stack or device
    }
  }
  else
  {
    Serial.println("Ignoring disconnect event, was not connected.");
  }
}

// --- Setup ---
void setup()
{
  Serial.begin(9600);
  // while (!Serial); // Wait for serial port to connect - Needed for some boards
  delay(1000); // Short delay for stability
  Serial.println("Starting BLE CTS Server ver 1 : " DEVICE_NAME);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // Start with LED off

  // Initialize BLE
  if (!BLE.begin())
  {
    Serial.println("Starting BLE failed!");
    while (1)
    { // Halt execution
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(200);
    }
  }

  // Set BLE device name
  BLE.setLocalName(DEVICE_NAME);
  BLE.setDeviceName(DEVICE_NAME);

  // Add characteristics to the service
  ctsService.addCharacteristic(currentTimeChar);
  ctsService.addCharacteristic(localTimeInfoChar);
  ctsService.addCharacteristic(refTimeInfoChar);

  // Add the service
  BLE.addService(ctsService);

  // Set advertised service UUID
  BLE.setAdvertisedService(ctsService); // Advertise the service itself

  // Set initial characteristic values
  lastTimeUpdateMillis = millis(); // Initialize time tracking
  updateInternalTime();            // Set initial time struct values
  writeCurrentTime();
  writeLocalTimeInfo();
  writeRefTimeInfo();

  // Assign event handlers
  BLE.setEventHandler(BLEConnected, blePeripheralConnectHandler);
  BLE.setEventHandler(BLEDisconnected, blePeripheralDisconnectHandler);

  // Assign the written handler specifically for the currentTimeChar
  currentTimeChar.setEventHandler(BLEWritten, currentTimeWrittenHandler);

  // Set advertising parameters (optional, use defaults or customize)
  BLE.setAdvertisingInterval(320); // Slower advertising interval: 200ms (320 * 0.625ms)
  // Set connection parameters for stability (longer intervals)
  // Min 30ms, Max 60ms. Supervision Timeout 4 seconds.
  BLE.setConnectionInterval(0x0018, 0x0030); // 24 * 1.25ms = 30ms, 48 * 1.25ms = 60ms
  BLE.setSupervisionTimeout(400);            // 400 * 10ms = 4000ms = 4s

  // Start advertising
  if (BLE.advertise())
  {
    Serial.println("Advertising started");
    Serial.print("MAC Address: ");
    Serial.println(BLE.address());
  }
  else
  {
    Serial.println("Advertising failed to start!");
    // Handle error, maybe retry or halt
  }

  // Initialize Task Scheduler runner (already done by task creation)
  Serial.println("Setup complete. Running tasks...");
}

// --- Loop ---
void loop()
{
  // Execute scheduled tasks
  ts.execute();

  // Add a small delay if loop runs too fast, can sometimes help stability
  // delay(1); // Uncomment if needed, but tBlePoll should handle polling sufficiently
}