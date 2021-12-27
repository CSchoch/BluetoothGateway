#include <Arduino.h>
using namespace std;

#define LED_PIN 2
#define DEBUGLEVEL NONE
#define MAX_PACKET_SIZE 256 // Max data packet size

#define uS_TO_S_FACTOR 1000000 // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP 60       // Time ESP32 will go to sleep (in seconds)

#include <WiFi.h>
#include "OTA.h"
#include <PubSubClient.h> // https://github.com/knolleary/pubsubclient/blob/master/examples/mqtt_esp8266/mqtt_esp8266.ino
#include <HardwareSerial.h>
#include <DebugUtils.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>

//#include "Config.h" // make your own config file or remove this line and use the following lines
const char *clientId = "Temperatures/Corridor";
const char *mqtt_server = "192.168.2.64";
#include "WifiCredentials.h"       // const char* ssid = "MySSID"; const char* WifiPassword = "MyPw";
IPAddress ip(192, 168, 2, 8);      // Static IP
IPAddress dns(192, 168, 2, 1);     // most likely your router
IPAddress gateway(192, 168, 2, 1); // most likely your router
IPAddress subnet(255, 255, 255, 0);

unsigned long lastUpdated;
unsigned long lastLed;
const char *nameprefix = "Tempratures";
uint8_t stateLed = HIGH;
boolean updateActive;
RTC_DATA_ATTR unsigned long bootCount;
RTC_DATA_ATTR boolean enableUpdate;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

struct Data
{
  float Temp = -1;
  float Humidity = -1;
  int VBat = -1;
  int Bat = -1;
  int Flag = -1;
  int Count = -1;
  int CountOld = -1;
  int Rssi = -1;
  string Name = "";
  char *Topic = "";
};

int scanTime = 30; // seconds
BLEScan *pBLEScan;
bool newData;
Data tempData;
Data sensorData[8];

void setup_wifi()
{
  const int maxlen = 40;
  char fullhostname[maxlen];
  uint8_t mac[6];
  delay(10);
  DEBUGPRINTNONE("Connecting to ");
  DEBUGPRINTLNNONE(ssid);

  WiFi.macAddress(mac);
  snprintf(fullhostname, maxlen, "%s-%02x%02x%02x", nameprefix, mac[3], mac[4], mac[5]);
  WiFi.setHostname(fullhostname);
  WiFi.config(ip, dns, gateway, subnet);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, WifiPassword);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    DEBUGPRINTNONE(".");
    counter++;
    unsigned long timer = millis();
    while (millis() - timer <= 500)
    {
    }
    //delay(500);
    if (counter >= 20)
    {
      counter = 0;
      DEBUGPRINTLNNONE("Retry");
      WiFi.disconnect();
      while (WiFi.status() == WL_CONNECTED)
      {
        DEBUGPRINTNONE(".");

        delay(10);
      }
      WiFi.begin(ssid, WifiPassword);
    }
  }
  DEBUGPRINTLNNONE("WiFi connected");
  DEBUGPRINTNONE("IP address: ");
  DEBUGPRINTLNNONE(WiFi.localIP());

  if (enableUpdate)
  {
    setupOTA(fullhostname);
  }
}

void wifiReconnect()
{
  WiFi.disconnect();
  while (WiFi.status() == WL_CONNECTED)
  {
    DEBUGPRINTNONE(".");
    delay(10);
  }
  DEBUGPRINTNONE("Reconnecting to ");
  DEBUGPRINTLNNONE(ssid);
  int counter = 0;
  WiFi.begin(ssid, WifiPassword);
  while (WiFi.status() != WL_CONNECTED and counter < 20)
  {
    DEBUGPRINTNONE(".");
    counter++;
    unsigned long timer = millis();
    while (millis() - timer <= 500)
    {
    }
    //delay(500);
  }
  DEBUGPRINTLNNONE("WiFi connected");
  DEBUGPRINTNONE("IP address: ");
  DEBUGPRINTLNNONE(WiFi.localIP());
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  char *topic1 = "/enableUpdate";
  char *path = (char *)malloc(1 + strlen(clientId) + strlen(topic1));
  strcpy(path, clientId);
  strcat(path, topic1);
  //if (topic == path) {
  DEBUGPRINTLNNONE("NewMessage");
  DEBUGPRINTLNNONE(topic);
  DEBUGPRINTLNNONE(length);
  free(path);
  enableUpdate = true;
  topic = "/enableUpdateAck";
  path = (char *)malloc(1 + strlen(clientId) + strlen(topic));
  strcpy(path, clientId);
  strcat(path, topic);
  mqttClient.publish(path, "1");
  free(path);
  delay(100);
  //}
}

void mqttReconnect()
{
  // Loop until we're reconnected
  while (!mqttClient.connected())
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      wifiReconnect();
    }
    DEBUGPRINTNONE("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect(clientId))
    {
      DEBUGPRINTLNNONE("connected");
      // ... and resubscribe
      char *topic = "/enableUpdate";
      char *path = (char *)malloc(1 + strlen(clientId) + strlen(topic));
      strcpy(path, clientId);
      strcat(path, topic);
      DEBUGPRINTLNNONE(path);
      mqttClient.subscribe(path);
      free(path);
    }
    else
    {
      DEBUGPRINTNONE("failed, rc=");
      DEBUGPRINTNONE(mqttClient.state());
      DEBUGPRINTLNNONE(" try again in 1 seconds");
      // Wait 1 seconds before retrying
      unsigned long timer = millis();
      while (millis() - timer <= 1000)
      {
      }
      //delay(1000);
    }
  }
}

void printBuffer(uint8_t *buf, int len)
{
  for (int i = 0; i < len; i++)
  {
    DEBUGPRINTFDEBUG("%02x", buf[i]);
  }
  DEBUGPRINTFDEBUG("\r\n");
}

void parse_value(uint8_t *buf, int len)
{
  int16_t x = buf[3];
  if (buf[2] > 1)
    x |= buf[4] << 8;
  switch (buf[0])
  {
  case 0x0D:
    if (buf[2] && len > 6)
    {
      float temp = x / 10.0;
      x = buf[5] | (buf[6] << 8);
      float humidity = x / 10.0;
      //DEBUGPRINTFDEBUG("Temp: %.1f°, Humidity: %.1f %%\r\n", temp, humidity);
      tempData.Temp = temp;
      tempData.Humidity = humidity;
    }
    break;
  case 0x04:
  {
    float temp = x / 10.0;

    //DEBUGPRINTFDEBUG("Temp: %.1f°\r\n", temp);
    tempData.Temp = temp;
  }
  break;
  case 0x06:
  {
    float humidity = x / 10.0;
    //DEBUGPRINTFDEBUG("Humidity: %.1f%%\r\n", humidity);
    tempData.Humidity = humidity;
  }
  break;
  case 0x0A:
  {
    //DEBUGPRINTFDEBUG("Battery: %d%%", x);
    tempData.Bat = x;
    if (len > 5 && buf[4] == 2)
    {
      uint16_t battery_mv = buf[5] | (buf[6] << 8);
      // DEBUGPRINTFDEBUG(", %d mV", battery_mv);
      tempData.VBat = battery_mv;
    }
    // DEBUGPRINTFDEBUG("\r\n");
  }
  break;
  default:
    // DEBUGPRINTFDEBUG("Type: 0x%02x ", buf[0]);
    printBuffer(buf, len);
    break;
  }
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{

  uint8_t *findServiceData(uint8_t *data, size_t length, uint8_t *foundBlockLength)
  {
    uint8_t *rightBorder = data + length;
    while (data < rightBorder)
    {
      uint8_t blockLength = *data + 1;
      //DEBUGPRINTFDEBUG("blockLength: 0x%02x\r\n", blockLength);
      if (blockLength < 5)
      {
        data += blockLength;
        continue;
      }
      uint8_t blockType = *(data + 1);
      uint16_t serviceType = *(uint16_t *)(data + 2);
      //DEBUGPRINTFDEBUG("blockType: 0x%02x, 0x%04x\r\n", blockType, serviceType);
      if (blockType == 0x16)
      { // https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile/
        //DEBUGPRINTFDEBUG("blockType: 0x%02x, 0x%04x\r\n", blockType, serviceType);
        /* 16-bit UUID for Members 0xFE95 Xiaomi Inc. https://btprodspecificationrefs.blob.core.windows.net/assigned-values/16-bit%20UUID%20Numbers%20Document.pdf */
        if (serviceType == 0xfe95 || serviceType == 0x181a)
        { // mi or custom service
          //DEBUGPRINTFDEBUG("blockLength: 0x%02x\r\n", blockLength);
          //DEBUGPRINTFDEBUG("blockType: 0x%02x, 0x%04x\r\n", blockType, serviceType);
          *foundBlockLength = blockLength;
          return data;
        }
      }
      data += blockLength;
    }
    return nullptr;
  }

  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    int dataIndex = -1;
    uint8_t mac[6];
    uint8_t *payload = advertisedDevice.getPayload();

    size_t payloadLength = advertisedDevice.getPayloadLength();
    uint8_t serviceDataLength = 0;
    uint8_t *serviceData = findServiceData(payload, payloadLength, &serviceDataLength);
    if (serviceData == nullptr || serviceDataLength < 15)
      return;
    Data initData;
    tempData = initData;
    uint16_t serviceType = *(uint16_t *)(serviceData + 2);
    DEBUGPRINTFDEBUG("Advertised Device: %s\r\n", advertisedDevice.toString().c_str());
    DEBUGPRINTFDEBUG("Found service '%04x' data len: %d, \r\n", serviceType, serviceDataLength);
    string devName = advertisedDevice.getName();
    for (size_t i = 0; i < sizeof(sensorData) / sizeof(sensorData[0]); i++)
    {
      if (devName == sensorData[i].Name)
      {
        dataIndex = i;
        break;
      }
    }
    if (dataIndex == -1)
    {
      return;
    }

    // if (advertisedDevice.getName() == "ATC_50B64A") // Keller
    // {
    //   dataIndex = 0;
    // }
    // else if (advertisedDevice.getName() == "ATC_AB94CF") // Wohnzimmer
    // {
    //   dataIndex = 1;
    // }
    // else if (advertisedDevice.getName() == "ATC_A0FF18") // Heizraum
    // {
    //   dataIndex = 2;
    // }
    // else if (advertisedDevice.getName() == "ATC_350AB8") // Flur
    // {
    //   dataIndex = 3;
    // }
    // else if (advertisedDevice.getName() == "ATC_F4ADDB") // Bad
    // {
    //   dataIndex = 4;
    // }
    // else if (advertisedDevice.getName() == "ATC_6FE1D5") // Eltern
    // {
    //   dataIndex = 5;
    // }
    // else if (advertisedDevice.getName() == "ATC_67B4CC") // Kind
    // {
    //   dataIndex = 6;
    // }
    // else if (advertisedDevice.getName() == "ATC_D24D9F") // Dach
    // {
    //   dataIndex = 7;
    // }
    // else
    // {
    //   return;
    // }

    tempData.Rssi = advertisedDevice.getRSSI();

    // printBuffer(serviceData, serviceDataLength);
    if (serviceType == 0xfe95)
    {
      if (serviceData[5] & 0x10)
      {
        mac[5] = serviceData[9];
        mac[4] = serviceData[10];
        mac[3] = serviceData[11];
        mac[2] = serviceData[12];
        mac[1] = serviceData[13];
        mac[0] = serviceData[14];
        // DEBUGPRINTFDEBUG("MAC: ");
        // printBuffer(mac, 6);
      }
      if ((serviceData[5] & 0x08) == 0)
      { // not encrypted
        serviceDataLength -= 15;
        payload = &serviceData[15];
        while (serviceDataLength > 3)
        {
          parse_value(payload, serviceDataLength);
          serviceDataLength -= payload[2] + 3;
          payload += payload[2] + 3;
        }
        // DEBUGPRINTFDEBUG("count: %d\r\n", serviceData[8]);
        tempData.Count = serviceData[8];
        newData = true;
      }
      else
      {
        if (serviceDataLength > 19)
        { // aes-ccm  bindkey
          // https://github.com/ahpohl/xiaomi_lywsd03mmc
          // https://github.com/Magalex2x14/LYWSD03MMC-info
          // DEBUGPRINTFDEBUG("Crypted data[%d]! ", serviceDataLength - 15);
        }
        // DEBUGPRINTFDEBUG("count: %d\r\n", serviceData[8]);
      }
    }
    else
    { // serviceType == 0x181a
      if (serviceDataLength > 18)
      { // custom format
        mac[5] = serviceData[4];
        mac[4] = serviceData[5];
        mac[3] = serviceData[6];
        mac[2] = serviceData[7];
        mac[1] = serviceData[8];
        mac[0] = serviceData[9];
        // DEBUGPRINTFDEBUG("MAC: ");
        // printBuffer(mac, 6);
        float temp = *(int16_t *)(serviceData + 10) / 100.0;
        float humidity = *(uint16_t *)(serviceData + 12) / 100.0;
        uint16_t vbat = *(uint16_t *)(serviceData + 14);
        // DEBUGPRINTFDEBUG("Temp: %.2f°, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, cout: %d\r\n", temp, humidity, vbat, serviceData[16], serviceData[18], serviceData[17]);
        tempData.Temp = temp;
        tempData.Humidity = humidity;
        tempData.VBat = vbat;
        tempData.Bat = serviceData[16];
        tempData.Flag = serviceData[18];
        tempData.Count = serviceData[17];
      }
      else if (serviceDataLength == 17)
      { // format atc1441
        // DEBUGPRINTFDEBUG("MAC: ");
        // printBuffer(serviceData + 4, 6);
        int16_t x = (serviceData[10] << 8) | serviceData[11];
        float temp = x / 10.0;
        uint16_t vbat = x = (serviceData[14] << 8) | serviceData[15];
        // DEBUGPRINTFDEBUG("Temp: %.1f°, Humidity: %d%%, Vbatt: %d, Battery: %d%%, cout: %d\r\n", temp, serviceData[12], vbat, serviceData[13], serviceData[16]);
        tempData.Temp = temp;
        tempData.Humidity = serviceData[12];
        tempData.VBat = vbat;
        tempData.Bat = serviceData[13];
        tempData.Count = serviceData[16];
      }
      newData = true;
    }
    if (tempData.Temp != -1)
    {
      sensorData[dataIndex].Temp = tempData.Temp;
    }
    if (tempData.Humidity != -1)
    {
      sensorData[dataIndex].Humidity = tempData.Humidity;
    }
    if (tempData.VBat != -1)
    {
      sensorData[dataIndex].VBat = tempData.VBat;
    }
    if (tempData.Bat != -1)
    {
      sensorData[dataIndex].Bat = tempData.Bat;
    }
    if (tempData.Flag != -1)
    {
      sensorData[dataIndex].Flag = tempData.Flag;
    }
    if (tempData.Count != -1)
    {
      sensorData[dataIndex].Count = tempData.Count;
    }
    if (tempData.Rssi != -1)
    {
      sensorData[dataIndex].Rssi = tempData.Rssi;
    }
  }
};

static void scanCompleteCB(BLEScanResults scanResults)
{
  // DEBUGPRINTFDEBUG("Scan complete!\r\n");
  // DEBUGPRINTFDEBUG("We found %d devices\r\n", scanResults.getCount());
  scanResults.dump();
  pBLEScan->start(scanTime, scanCompleteCB);
} // scanCompleteCB

void setup()
{
  Serial.begin(115200);
  DEBUGPRINTLNNONE("\nHardware serial started");
  bootCount++;
  updateActive = enableUpdate;
  lastUpdated = millis() - (TIME_TO_SLEEP * 1000);
  lastLed = millis();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  DEBUGPRINTLNNONE(bootCount);

  if (!enableUpdate)
  {
    sensorData[0].Name = "ATC_50B64A";
    sensorData[0].Topic = "Basement";
    sensorData[1].Name = "ATC_AB94CF";
    sensorData[1].Topic = "LivingRoom";
    sensorData[2].Name = "ATC_A0FF18";
    sensorData[2].Topic = "HeatingRoom";
    sensorData[3].Name = "ATC_350AB8";
    sensorData[3].Topic = "Corridor";
    sensorData[4].Name = "ATC_F4ADDB";
    sensorData[4].Topic = "Bath";
    sensorData[5].Name = "ATC_6FE1D5";
    sensorData[5].Topic = "Parents";
    sensorData[6].Name = "ATC_67B4CC";
    sensorData[6].Topic = "Child";
    sensorData[7].Name = "ATC_D24D9F";
    sensorData[7].Topic = "Atic";

    DEBUGPRINTLNNONE("Scanning...");
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
    pBLEScan->setInterval(500); // default 100
    pBLEScan->setWindow(100);   // default 100, less or equal setInterval value
    pBLEScan->setActiveScan(true);
    pBLEScan->start(scanTime, scanCompleteCB);
    DEBUGPRINTLNNONE("Now scanning in the background ... scanCompleteCB() will be called when done.");
  }
  setup_wifi();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(MAX_PACKET_SIZE);
}

void loop()
{
  if (enableUpdate && !updateActive)
  {
    esp_deep_sleep(10 * uS_TO_S_FACTOR);
  }
  else if (enableUpdate)
  {
    ArduinoOTA.handle();
    digitalWrite(LED_PIN, HIGH);
  }
  else
  {
    char Data[MAX_PACKET_SIZE];
    if (!mqttClient.connected())
    {
      mqttReconnect();
    }
    mqttClient.loop();

    // if (newData)
    // {
    //   uint8_t i = 0;
    //   DEBUGPRINTFNONE("Keller: Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Wohnzimmer Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Heizraum Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Flur Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Bad Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Eltern Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Katja Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTFNONE("Dachboden Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
    //   i++;
    //   DEBUGPRINTLNNONE("");
    //   newData = false;
    // }

    if (newData)
    {
      for (size_t i = 0; i < sizeof(sensorData) / sizeof(sensorData[0]); i++)
      {
        DEBUGPRINTFNONE("%s: Temp: %.2f°C, Humidity: %.2f%%, Vbatt: %d, Battery: %d%%, flg: 0x%02x, count: %d\r\n", sensorData[i].Topic, sensorData[i].Temp, sensorData[i].Humidity, sensorData[i].VBat, sensorData[i].Bat, sensorData[i].Flag, sensorData[i].Count);
      }
      DEBUGPRINTLNNONE("");
      newData = false;
    }

    for (size_t i = 0; i < sizeof(sensorData) / sizeof(sensorData[0]); i++)
    {
      if (sensorData[i].Count != sensorData[i].CountOld && sensorData[i].Temp != -1 && sensorData[i].Humidity != -1 && sensorData[i].VBat != -1 && sensorData[i].Bat != -1)
      {
        sensorData[i].CountOld = sensorData[i].Count;
        lastUpdated = millis();
        const size_t capacity = JSON_OBJECT_SIZE(6);
        DynamicJsonDocument doc(capacity);

        //JsonObject Room = doc.createNestedObject("LivingRoom");
        doc["Temperature"] = sensorData[i].Temp;
        doc["Humidity"] = sensorData[i].Humidity;
        doc["VBattery"] = sensorData[i].VBat;
        doc["Battery"] = sensorData[i].Bat;
        doc["Count"] = sensorData[i].Count;
        doc["Lqi"] = 100 + sensorData[i].Rssi;

        DEBUGPRINTNONE("MemUsage.........: ");
        DEBUGPRINTLNNONE(doc.memoryUsage());

        serializeJson(doc, Data, sizeof(Data));

        char *topic;
        switch (i)
        {
        case 0:
          topic = "/Basement";
          break;

        case 1:
          topic = "/LivingRoom";
          break;

        case 2:
          topic = "/HeatingRoom";
          break;

        case 3:
          topic = "/Corridor";
          break;

        case 4:
          topic = "/Bath";
          break;

        case 5:
          topic = "/Parents";
          break;

        case 6:
          topic = "/Child";
          break;

        case 7:
          topic = "/Atic";
          break;

        default:
          topic = "/Unknown";
          break;
        };
        char *path = (char *)malloc(1 + strlen(clientId) + strlen(topic));
        strcpy(path, clientId);
        strcat(path, topic);
        if (!mqttClient.publish(path, Data, false))
        {
          lastUpdated = millis() - (TIME_TO_SLEEP * 1000);
          sensorData[i].CountOld = -1;
          DEBUGPRINTLNNONE("MQTT publish failed");
        }
        free(path);

        DEBUGPRINTLNDEBUG(Data);
      }
    }
  }
}
