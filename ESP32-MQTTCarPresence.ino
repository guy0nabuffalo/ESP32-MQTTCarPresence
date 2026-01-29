#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>

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

#define WIFI_TIMEOUT_MS 20000 // WiFi connection timeout (20 seconds)
#define WDT_TIMEOUT 60 // Watchdog timeout in seconds
#define WIFI_CHECK_INTERVAL_MS 1000 // How often to check WiFi status when connected (1 second for faster reconnection)

void keepWifiAlive(void *parameters)
{
  // Add this task to watchdog monitoring
  esp_task_wdt_add(NULL);

  for (;;)
  {
    // Feed watchdog from this task
    esp_task_wdt_reset();

    if (WiFi.status() == WL_CONNECTED)
    {
      // Check frequently (1s) for faster disconnect detection when driving away
      vTaskDelay(WIFI_CHECK_INTERVAL_MS / portTICK_PERIOD_MS);
      continue;
    }

    // WiFi not connected, attempt to connect
    Serial.println("[WIFI] Not connected, attempting connection...");
    Serial.print("[WIFI] Connecting to SSID: ");
    Serial.println(wifiSSID);

    // Disconnect and clear any existing configuration
    WiFi.disconnect(true);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Set WiFi mode and disable auto-reconnect (we control reconnection)
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);

    // Begin connection
    WiFi.begin(wifiSSID, wifiPassword);
    unsigned long startAttemptTime = millis();

    // Wait for connection with timeout
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_TIMEOUT_MS)
    {
      digitalWrite(WIFI_STATUS_PIN, HIGH);
      Serial.print(".");
      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(WIFI_STATUS_PIN, LOW);
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println();
      Serial.print("[WIFI] Connected successfully: ");
      Serial.println(WiFi.localIP().toString().c_str());
      Serial.print("[WIFI] Signal strength: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");

      // Re-apply WiFi sleep disable after reconnection for constant scanning
      WiFi.setSleep(false);

      digitalWrite(WIFI_STATUS_PIN, HIGH);
    }
    else
    {
      Serial.println();
      Serial.println("[WIFI] Connection failed - will retry");
      Serial.print("[WIFI] Status code: ");
      Serial.println(WiFi.status());
      digitalWrite(WIFI_STATUS_PIN, LOW);
      // Short delay before retry to allow for scanning
      vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
  }
}

void setupWifi()
{
  Serial.println("[WIFI] Initial WiFi setup...");

  // Set hostname before connecting
  WiFi.setHostname(mqttNode.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // We control reconnection via keepWifiAlive task

  // Disable WiFi sleep mode for constant scanning
  WiFi.setSleep(false);

  Serial.print("[WIFI] Attempting to connect to: ");
  Serial.println(wifiSSID);

  WiFi.begin(wifiSSID, wifiPassword);

  // Wait for connection with timeout (not infinite loop)
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS)
  {
    delay(100);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.print("[WIFI] Connected successfully! IP: ");
    Serial.println(WiFi.localIP().toString());
  }
  else
  {
    Serial.println();
    Serial.println("[WIFI] Initial connection failed, keepWifiAlive task will handle reconnection");
  }
}

void mqttConnect()
{
  // Only attempt MQTT connection if WiFi is connected
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[MQTT] Cannot connect - WiFi not connected");
    return;
  }

  digitalWrite(WIFI_STATUS_PIN, HIGH);
  Serial.print("[MQTT] Attempting connection to broker: ");
  Serial.println(mqttServer);

  // Use NULL for empty username/password to avoid authentication issues
  const char* user = (mqttUser[0] != '\0') ? mqttUser : NULL;
  const char* pass = (mqttPassword[0] != '\0') ? mqttPassword : NULL;

  if (mqttClient.connect(mqttNode.c_str(), user, pass, mqttDiscoBinaryStateTopic.c_str(), 1, 1, "OFF"))
  {
    String signalStrength = String(WiFi.RSSI());
    reportTimer = millis();
    String uptimeTimer = String(millis());

    Serial.println("[MQTT] Connected successfully!");
    Serial.println("[MQTT] Publishing discovery configs...");

    mqttClient.publish(mqttDiscoUptimeConfigTopic.c_str(), mqttDiscoUptimeConfigPayload.c_str(), true);
    mqttClient.publish(mqttDiscoUptimeStateTopic.c_str(), uptimeTimer.c_str());
    mqttClient.publish(mqttDiscoBinaryConfigTopic.c_str(), mqttDiscoBinaryConfigPayload.c_str(), true);
    mqttClient.publish(mqttDiscoBinaryStateTopic.c_str(), "ON");
    mqttClient.publish(mqttDiscoSignalConfigTopic.c_str(), mqttDiscoSignalConfigPayload.c_str(), true);
    mqttClient.publish(mqttDiscoSignalStateTopic.c_str(), signalStrength.c_str());

    Serial.println("[MQTT] Discovery messages published");
    digitalWrite(WIFI_STATUS_PIN, LOW);
  }
  else
  {
    Serial.print("[MQTT] Connection failed, rc=");
    Serial.println(mqttClient.state());
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

void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(2, 0, 0)
  // New event names for ESP32 Arduino Core 2.0.0+
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.println("[WIFI EVENT] Connected to WiFi, IP: " + WiFi.localIP().toString());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.println("[WIFI EVENT] Disconnected from WiFi");
    break;
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println("[WIFI EVENT] WiFi started");
    break;
  case ARDUINO_EVENT_WIFI_STA_STOP:
    Serial.println("[WIFI EVENT] WiFi stopped");
    break;
#else
  // Legacy event names for older ESP32 Arduino Core
  case SYSTEM_EVENT_STA_GOT_IP:
    Serial.println("[WIFI EVENT] Connected to WiFi, IP: " + WiFi.localIP().toString());
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    Serial.println("[WIFI EVENT] Disconnected from WiFi");
    break;
  case SYSTEM_EVENT_STA_START:
    Serial.println("[WIFI EVENT] WiFi started");
    break;
  case SYSTEM_EVENT_STA_STOP:
    Serial.println("[WIFI EVENT] WiFi stopped");
    break;
#endif
  default:
    break;
  }
}

void setup()
{
  pinMode(WIFI_STATUS_PIN, OUTPUT);
  digitalWrite(WIFI_STATUS_PIN, HIGH);

  Serial.begin(115200);
  delay(500); // Give serial time to initialize
  Serial.println("\n\n========================================");
  Serial.println("[SYSTEM] ESP32 MQTT Car Presence");
  Serial.println("[SYSTEM] Starting initialization...");
  Serial.println("========================================");

  // Configure and enable hardware watchdog timer
  Serial.print("[SYSTEM] Configuring watchdog timer (");
  Serial.print(WDT_TIMEOUT);
  Serial.println(" seconds)...");
  esp_task_wdt_init(WDT_TIMEOUT, true); // Enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);                // Add current thread to WDT watch
  Serial.println("[SYSTEM] Watchdog timer enabled");

  // Register WiFi event handler
  WiFi.onEvent(WiFiEvent);
  Serial.println("[SYSTEM] WiFi event handler registered");

  // Initial WiFi setup
  setupWifi();

  // Create WiFi keep-alive task on core 0
  Serial.println("[SYSTEM] Creating WiFi keep-alive task...");
  xTaskCreatePinnedToCore(
      keepWifiAlive,
      "keepWifiAlive",
      8192,
      NULL,
      1,
      NULL,
      0);
  Serial.println("[SYSTEM] WiFi keep-alive task started on core 0");

  // Setup MQTT
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(mqtt_callback);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(10);  // 10 second socket timeout to prevent long blocking
  mqttClient.setKeepAlive(30);      // 30 second keepalive for faster disconnect detection
  Serial.println("[SYSTEM] MQTT client configured");

  mqttConnect();

  // Setup OTA if password is set
  if (otaPassword[0])
  {
    setupOTA();
  }
  else
  {
    Serial.println("[SYSTEM] OTA updates disabled (no password set)");
  }

  Serial.println("========================================");
  Serial.println("[SYSTEM] Initialization complete!");
  Serial.println("========================================\n");
}

void loop()
{
  // Feed the watchdog timer to prevent reset
  esp_task_wdt_reset();

  // WiFi reconnection is handled by keepWifiAlive task
  // Only attempt MQTT connection if WiFi is connected
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected())
  {
    mqttConnect();
  }

  // Process MQTT messages if connected
  if (mqttClient.connected())
  {
    mqttClient.loop();
  }

  // Twinkle LED when MQTT is connected (visual indicator)
  if (mqttClient.connected() && ((millis() - twinkleTimer) >= twinkleInterval))
  {
    digitalWrite(WIFI_STATUS_PIN, !digitalRead(WIFI_STATUS_PIN));
    twinkleTimer = millis();
  }

  // Publish periodic updates when MQTT is connected
  if (mqttClient.connected() && ((millis() - reportTimer) >= reportInterval))
  {
    String signalStrength = String(WiFi.RSSI());
    String uptimeTimer = String(millis());

    if (mqttClient.publish(mqttDiscoSignalStateTopic.c_str(), signalStrength.c_str()) &&
        mqttClient.publish(mqttDiscoUptimeStateTopic.c_str(), uptimeTimer.c_str()))
    {
      // Successfully published
      reportTimer = millis();
    }
    else
    {
      Serial.println("[MQTT] Failed to publish update");
    }
  }

  // Handle OTA updates if enabled
  if (otaPassword[0])
  {
    ArduinoOTA.handle();
  }

  // Small delay to prevent tight loop
  delay(10);
}
