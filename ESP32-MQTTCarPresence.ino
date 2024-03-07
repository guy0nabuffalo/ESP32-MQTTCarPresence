#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>

const char *wifiSSID = "wifissid";                  // Your WiFi network name
const char *wifiPassword = "wifipassword";          // Your WiFi network password
const char *otaPassword = "";                       // OTA update password
const char *mqttServer = "hassio.local";            // Your MQTT server IP address
const char *mqttUser = "";                          // MQTT username, set to "" for no user
const char *mqttPassword = "";                      // MQTT password, set to "" for no password
const String mqttNode = "CarPresence";              // Your unique hostname for this device
const String mqttDiscoveryPrefix = "homeassistant"; // Home Assistant MQTT Discovery, see https://www.home-assistant.io/integrations/mqtt/#discovery

const String mqttDiscoBinaryStateTopic = mqttDiscoveryPrefix + "/binary_sensor/" + mqttNode + "/state";
const String mqttDiscoBinaryConfigTopic = mqttDiscoveryPrefix + "/binary_sensor/" + mqttNode + "/config";
const String mqttDiscoSignalStateTopic = mqttDiscoveryPrefix + "/sensor/" + mqttNode + "-signal/state";
const String mqttDiscoSignalConfigTopic = mqttDiscoveryPrefix + "/sensor/" + mqttNode + "-signal/config";
const String mqttDiscoUptimeStateTopic = mqttDiscoveryPrefix + "/sensor/" + mqttNode + "-uptime/state";
const String mqttDiscoUptimeConfigTopic = mqttDiscoveryPrefix + "/sensor/" + mqttNode + "-uptime/config";

const String mqttDiscoBinaryConfigPayload = "{\"name\": \"" + mqttNode + "\", \"device_class\": \"connectivity\", \"state_topic\": \"" + mqttDiscoBinaryStateTopic + "\"}";
const String mqttDiscoSignalConfigPayload = "{\"name\": \"" + mqttNode + "-signal\", \"state_topic\": \"" + mqttDiscoSignalStateTopic + "\", \"unit_of_measurement\": \"dBm\", \"value_template\": \"{{ value }}\"}";
const String mqttDiscoUptimeConfigPayload = "{\"name\": \"" + mqttNode + "-uptime\", \"state_topic\": \"" + mqttDiscoUptimeStateTopic + "\", \"unit_of_measurement\": \"msec\", \"value_template\": \"{{ value }}\"}";

const unsigned long reportInterval = 5000;
unsigned long reportTimer = millis();

const unsigned long twinkleInterval = 50;
unsigned long twinkleTimer = millis();

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

#define WIFI_STATUS_PIN 2 // Assuming onboard LED is connected to pin 2. Change if necessary.

#define WIFI_TIMEOUT_MS 30000 // Adjust timeout according to your needs

void keepWifiAlive(void *parameters)
{
  for (;;)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("WiFi still connected: ");
      Serial.println(WiFi.localIP().toString().c_str());
      digitalWrite(WIFI_STATUS_PIN, HIGH);
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      continue;
    }

    Serial.println("WiFi Connecting");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPassword);
    unsigned long startAttemptTime = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_TIMEOUT_MS)
    {
      digitalWrite(WIFI_STATUS_PIN, HIGH);
      Serial.print(".");
      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(WIFI_STATUS_PIN, LOW);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("[WIFI] Connected: ");
      Serial.println(WiFi.localIP().toString().c_str());
      digitalWrite(WIFI_STATUS_PIN, HIGH);
    }
    else
    {
      Serial.println("[WIFI] Failed to Connect before timeout");
      digitalWrite(WIFI_STATUS_PIN, LOW);
    }
  }
}

void setupWifi()
{
  Serial.print("Connecting to WiFi network: " + String(wifiSSID));
  WiFi.setHostname(mqttNode.c_str());
  WiFi.begin(wifiSSID, wifiPassword);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1);
  }
  Serial.println("\nWiFi connected successfully and assigned IP: " + WiFi.localIP().toString());
}

void mqttConnect()
{
  digitalWrite(WIFI_STATUS_PIN, HIGH);
  Serial.println("Attempting MQTT connection to broker: " + String(mqttServer));

  if (mqttClient.connect(mqttNode.c_str(), mqttUser, mqttPassword, mqttDiscoBinaryStateTopic.c_str(), 1, 1, "OFF"))
  {
    String signalStrength = String(WiFi.RSSI());
    reportTimer = millis();
    String uptimeTimer = String(millis());

    Serial.println("MQTT discovery connectivity config: [" + mqttDiscoBinaryConfigTopic + "] : [" + mqttDiscoBinaryConfigPayload + "]");
    Serial.println("MQTT discovery connectivity state: [" + mqttDiscoBinaryStateTopic + "] : [ON]");
    Serial.println("MQTT discovery signal config: [" + mqttDiscoSignalConfigTopic + "] : [" + mqttDiscoSignalConfigPayload + "]");
    Serial.println("MQTT discovery signal state: [" + mqttDiscoSignalStateTopic + "] : " + WiFi.RSSI());
    Serial.println("MQTT discovery uptime config: [" + mqttDiscoUptimeConfigTopic + "] : [" + mqttDiscoUptimeConfigPayload + "]");
    Serial.println("MQTT discovery uptime state: [" + mqttDiscoUptimeStateTopic + "] : " + uptimeTimer);

    mqttClient.publish(mqttDiscoUptimeConfigTopic.c_str(), mqttDiscoUptimeConfigPayload.c_str(), true);
    mqttClient.publish(mqttDiscoUptimeStateTopic.c_str(), uptimeTimer.c_str());
    mqttClient.publish(mqttDiscoBinaryConfigTopic.c_str(), mqttDiscoBinaryConfigPayload.c_str(), true);
    mqttClient.publish(mqttDiscoBinaryStateTopic.c_str(), "ON");
    mqttClient.publish(mqttDiscoSignalConfigTopic.c_str(), mqttDiscoSignalConfigPayload.c_str(), true);
    mqttClient.publish(mqttDiscoSignalStateTopic.c_str(), signalStrength.c_str());

    Serial.println("MQTT connected");
    digitalWrite(WIFI_STATUS_PIN, LOW);
  }
  else
  {
    Serial.println("MQTT connection failed, rc=" + String(mqttClient.state()));
  }
}

void setupOTA()
{
  ArduinoOTA.setHostname(mqttNode.c_str());
  ArduinoOTA.setPassword(otaPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("ESP OTA:  update start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("ESP OTA:  update complete");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.println("ESP OTA:  ERROR code " + String(error));
    if (error == OTA_AUTH_ERROR)
      Serial.println("ESP OTA:  ERROR - Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("ESP OTA:  ERROR - Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("ESP OTA:  ERROR - Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("ESP OTA:  ERROR - Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("ESP OTA:  ERROR - End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("ESP OTA:  Over the Air firmware update ready");
}

void mqtt_callback(char *topic, byte *payload, unsigned int payloadLength)
{
}

void setup()
{
  pinMode(WIFI_STATUS_PIN, OUTPUT);
  digitalWrite(WIFI_STATUS_PIN, HIGH);

  Serial.begin(115200);
  Serial.println("\nHardware initialized, starting program load");

  setupWifi();

  xTaskCreatePinnedToCore(
      keepWifiAlive,
      "keepWifiAlive",
      8192,
      NULL,
      1,
      NULL,
      0);

  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(mqtt_callback);

  mqttClient.setBufferSize(512);

  mqttConnect();

  if (otaPassword[0])
  {
    setupOTA();
  }

  Serial.println("Initialization complete\n");
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    setupWifi();
  }

  if (!mqttClient.connected())
  {
    mqttConnect();
  }

  if (mqttClient.connected())
  {
    mqttClient.loop();
  }

  if (mqttClient.connected() && ((millis() - twinkleTimer) >= twinkleInterval))
  {
    digitalWrite(WIFI_STATUS_PIN, !digitalRead(WIFI_STATUS_PIN));
    twinkleTimer = millis();
  }

  if (mqttClient.connected() && ((millis() - reportTimer) >= reportInterval))
  {
    String signalStrength = String(WiFi.RSSI());
    String uptimeTimer = String(millis());
    mqttClient.publish(mqttDiscoSignalStateTopic.c_str(), signalStrength.c_str());
    mqttClient.publish(mqttDiscoUptimeStateTopic.c_str(), uptimeTimer.c_str());
    reportTimer = millis();
  }

  if (otaPassword[0])
  {
    ArduinoOTA.handle();
  }
}
