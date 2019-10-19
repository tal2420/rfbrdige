#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ELECHOUSE_CC1101_RCS_DRV.h>
#include <RCSwitch.h>
#include <ArduinoJson.h>
#include <config.h>

#ifndef STASSID
#define STASSID WIFI_SSID
#define STAPSK  WIFI_PASSWORD
#define STAHOSTNAME "RF_Wifi"
#endif


const char* ssid = STASSID;
const char* password = STAPSK;
const char* wifihostname = STAHOSTNAME;

String uniqueId = String(random(0xffff), HEX);

//MQTT
const char* mqtt_server = MQTT_SERVER;
const int mqtt_rate_limit_time = 500;
const int mqtt_send_wait_interval = 150;
int mqtt_send_wait_time = mqtt_send_wait_interval;

const char* mqtt_topic_discover_action = "rfbridge/action/discover";
const char* mqtt_topic_discover = "rfbridge/discover";
const char* mqtt_topic_list_devices_action = "rfbridge/action/listdevices";
const char* mqtt_topic_list_devices = "rfbridge/listdevices";

WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;
//
String curIpAddr ="";
String curMacAddr ="";

int pin; // int for Receive pin.

int lastCodeVal = -1;
long lastCodeTime = -1;
RCSwitch mySwitch = RCSwitch();

long wifiDisconnectedMillis = 0;
int wifiLedPin = 2;
int mqttLedPin = 14;

long modeDiscoveryRequest = -1;

struct rfdevice {
  String macAddr;
  String ipAddr;
  int waitTime;
};

rfdevice lstRfDevices[10];
int rfDeviceCount = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("Booting");

#ifdef ESP32
pin = 4;  // for esp32! Receiver on GPIO pin 4. 
#elif ESP8266
pin = 4;  // for esp8266! Receiver on pin 4 = D2.
#else
pin = 0;  // for Arduino! Receiver on interrupt 0 => that is pin #2
#endif    

//CC1101 Settings:                (Settings with "//" are optional!)
//ELECHOUSE_cc1101.setRxBW(16);     // set Receive filter bandwidth (default = 812khz) 1 = 58khz, 2 = 67khz, 3 = 81khz, 4 = 101khz, 5 = 116khz, 6 = 135khz, 7 = 162khz, 8 = 203khz, 9 = 232khz, 10 = 270khz, 11 = 325khz, 12 = 406khz, 13 = 464khz, 14 = 541khz, 15 = 650khz, 16 = 812khz.
  ELECHOUSE_cc1101.setMHZ(433.92); // Here you can set your basic frequency. The lib calculates the frequency automatically (default = 433.92).The cc1101 can: 300-348 MHZ, 387-464MHZ and 779-928MHZ. Read More info from datasheet.
  ELECHOUSE_cc1101.Init(PA10);    // must be set to initialize the cc1101! set TxPower  PA10, PA7, PA5, PA0, PA_10, PA_15, PA_20, PA_30.

  mySwitch.enableReceive(pin);  // Receiver on

  ELECHOUSE_cc1101.SetRx();  // set Recive on
  
  lastCodeTime = millis();

  pinMode(wifiLedPin, OUTPUT);
  digitalWrite(wifiLedPin, LOW);

  pinMode(mqttLedPin, OUTPUT);
  digitalWrite(mqttLedPin, LOW);
  WIFI_Connect();
  //OTA

  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  String clientId = "rf_wifi-";
  clientId += uniqueId;
  ArduinoOTA.setHostname(clientId.c_str());

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  curIpAddr = WiFi.localIP().toString();
  curMacAddr = WiFi.macAddress();
  Serial.println(curIpAddr);

  //MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

void WIFI_Connect()
{
  digitalWrite(wifiLedPin, LOW);
  WiFi.disconnect();
  Serial.println("Reconnecting...");
  WiFi.mode(WIFI_STA);
  WiFi.hostname(wifihostname);
  WiFi.begin(ssid, password);
  
  // if not connected fo 2 minutes - reboot ESP
  long now = millis();
  if (now - wifiDisconnectedMillis > 120000) {
    Serial.println("Rebooting");
    ESP.restart();
  }  
  
  // Wait for connection
  for (int i = 0; i < 25; i++)
  {
    if ( WiFi.status() != WL_CONNECTED ) {
      delay ( 250 );
      digitalWrite(wifiLedPin, HIGH);
      Serial.print ( "." );
      delay ( 250 );
      digitalWrite(wifiLedPin, LOW);
    }
  }
  digitalWrite(wifiLedPin, HIGH);
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
  
  if(strcmp(topic, mqtt_topic_discover_action) == 0) {
    handleDiscoveryRequest(payload);
  }
  else if(strcmp(topic, mqtt_topic_discover) == 0) {
    handleDiscoveryReply(payload);
  }
  else if(strcmp(topic, mqtt_topic_list_devices_action) == 0) {
    handleListDeviceRequest(payload);
  }
}

void handleListDeviceRequest(byte* payload) {
  const size_t capacity = JSON_OBJECT_SIZE(1) + 30;
  DynamicJsonDocument doc(capacity);
  //const char* json = "{\"action\":\"QueryDeviceList\"}";
  deserializeJson(doc, payload);
  const char* action = doc["action"]; // "QueryDeviceList"

  if (doc["action"].isNull()==1 || strcmp(action,"QueryDeviceList") !=0) {
     Serial.println("Received a bad request!");
  }
  else {
    sendDeviceListMsg();
  }
}

void sendDeviceListMsg() {
  //const size_t capacity = JSON_ARRAY_SIZE(rfDeviceCount+1) + (rfDeviceCount+1)*JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(4);
  //const size_t capacity = JSON_ARRAY_SIZE(2) + 2*JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(4);
  //DynamicJsonDocument doc(capacity);
  StaticJsonDocument<1200> doc;
  
  doc["action"] = "ListDevices";
  doc["mac"] = curMacAddr;
  doc["count"] = rfDeviceCount;
    
  JsonArray devices = doc.createNestedArray("devices");

  for (int i=0;i<rfDeviceCount;i++) {
      JsonObject devices_0 = devices.createNestedObject();
      if (rfDeviceCount<4) {
        devices_0["mac"] = lstRfDevices[i].macAddr;
      }
      else {
        devices_0["mac"] = "";
      }
      if (rfDeviceCount<3) {
        devices_0["ip"] = lstRfDevices[i].ipAddr;
      }
      else {
        devices_0["ip"] = "";
      }
      devices_0["delay"] = lstRfDevices[i].waitTime;
    }
  
  serializeJson(doc, Serial);

    String strOutput="";
    serializeJson(doc, strOutput);
    client.publish(mqtt_topic_list_devices, (char*) strOutput.c_str());
}

void handleDiscoveryRequest(byte* payload) {
  const size_t capacity = JSON_OBJECT_SIZE(2) + 60;
  DynamicJsonDocument doc(capacity);
  //const char* json = payload;"{\"action\":\"Request\",\"requestor_mac\":\"4C:3A:E8:0A:7E:67\"}";
  deserializeJson(doc, payload);
  const char* action = doc["action"]; // "Request"
  const char* requestor_mac = doc["requestor_mac"]; // "4C:3A:E8:0A:7E:67"

  
  if (doc["action"].isNull()==1 || doc["requestor_mac"].isNull()==1) {
     Serial.println("Received a bad request!");
  }
  else {
    const char* action = doc["action"]; // "Response"
    const char* requestor_mac = doc["requestor_mac"]; // "EC:FA:BC:A8:64:28"
  
    
    //if recieved a discovery request from another device
    if (strcmp(action,"Request")==0 && strcmp(requestor_mac,WiFi.macAddress().c_str())!=0) {
      Serial.println("Answering discovery request");
      sendDiscoveryMsg(requestor_mac);
    }
  }
}

void sendDiscoveryMsg(String requestorMac) {
  // dbg
  //Serial.println("dbg: sendDiscoveryMsg()");

  //const size_t capacity = 2*JSON_OBJECT_SIZE(3);
  StaticJsonDocument<300> doc;
  
  doc["action"] = "Response";
  doc["requestor_mac"] = requestorMac;
  
  JsonObject data = doc.createNestedObject("data");
  data["mac"] = curMacAddr;
  data["ip"] = curIpAddr;
  data["delay"] = mqtt_send_wait_time;

  char output[128];
  serializeJson(doc, output);
  
  client.publish(mqtt_topic_discover, output);
  serializeJson(doc, Serial);
  Serial.println(output);
}

void handleDiscoveryReply(byte* payload) {
  const size_t capacity = 2*JSON_OBJECT_SIZE(3) + 110;
  DynamicJsonDocument doc(capacity);
  
  deserializeJson(doc, payload);
  
  const char* action = doc["action"]; // "Response"
  const char* requestor_mac = doc["requestor_mac"]; // "4C:3A:E8:0A:7E:67"
  
  JsonObject data = doc["data"];
  const char* data_mac = data["mac"]; // "EC:FA:BC:A8:64:28"
  const char* data_ip = data["ip"]; // "192.168.1.142"
  int data_delay = data["delay"]; // 150
  
  if (doc["action"].isNull()==1 || doc["requestor_mac"].isNull()==1 || doc["data"].isNull()==1 || data["mac"].isNull()==1 || data["ip"].isNull()==1 || data["delay"].isNull()==1 ) {
      Serial.println("Received a bad reply!");
  }
  else {
   if (strcmp(action,"Response")==0 && strcmp(requestor_mac,WiFi.macAddress().c_str())==0) {
    //Check if device already in list
    int intExists=0;
    int intDelayChanged=0;
    
    for (int i=0;i<rfDeviceCount;i++) {
      if (strcmp(lstRfDevices[i].macAddr.c_str(),data_mac)==0) {
        intExists=1;
        lstRfDevices[i].ipAddr = data_ip;
        
        if (lstRfDevices[i].waitTime != data_delay) {
          intDelayChanged=1;
          lstRfDevices[i].waitTime = data_delay;
        }
      }
    }
     
      if (intExists==0) {
        Serial.println("Adding new device");
  
        lstRfDevices[rfDeviceCount].macAddr = data_mac;
        lstRfDevices[rfDeviceCount].ipAddr = data_ip;
        lstRfDevices[rfDeviceCount].waitTime = data_delay;
        rfDeviceCount++;
      }
      if (intExists==0 || intDelayChanged ==1) {
        updateDelayTime();
      }
    }
  }
}

void updateDelayTime() {
    // dbg
  Serial.println("dbg: updateDelayTime()");
  //mqtt_send_wait_time
  int intLoopCnt =1;
  int intCurValue = mqtt_send_wait_interval;
  int intCurValueFound =-1;

  Serial.print("old wait value: ");
  Serial.print(mqtt_send_wait_time);
  Serial.println("- optimizing..");
  
  while (intCurValueFound !=0) {
      intCurValueFound =0;
      
      for (int i=0;i<rfDeviceCount;i++) {
         if ((int)lstRfDevices[i].waitTime == intCurValue) {
          intCurValueFound =1;
          Serial.print("found another device with ");
          Serial.print(intCurValue);
          Serial.println(" wait value..");
         }
      }
      
      if (intCurValueFound ==1) {
        intLoopCnt++;
        intCurValue = mqtt_send_wait_interval *intLoopCnt;
      }
  }
  mqtt_send_wait_time = intCurValue;
  Serial.print("new wait value: ");
  Serial.println(intCurValue);
}

void sendKeepAliveMsg() {
  snprintf (msg, 50, "KA #%ld", value);
  Serial.print("Publish KA message: ");
  Serial.println(msg);
  //client.publish("rfbridge/status", msg);
    
  const int capacity = JSON_OBJECT_SIZE(5);
  StaticJsonDocument<200> doc;

  doc["mac"].set(WiFi.macAddress());
  doc["ip"].set(curIpAddr);
  doc["freeHeap"].set(ESP.getFreeHeap());
  doc["alive"].set(true);
  doc["counter"].set(value);

  char output[128];
  serializeJson(doc, output);
  client.publish("rfbridge/status", output);
  serializeJson(doc, Serial);
  //Serial.println(output);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += uniqueId;
    // Attempt to connect
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish("rfbridge/status", "Connected to MQTT");
      // ... and resubscribe
      client.subscribe(mqtt_topic_discover);
      client.subscribe(mqtt_topic_discover_action);
      client.subscribe(mqtt_topic_list_devices_action);
      client.subscribe(mqtt_topic_list_devices_action);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void loop() {
  ArduinoOTA.handle();
  
  if (mySwitch.available()){
    int curCodeVal = mySwitch.getReceivedValue();
    Serial.print("Received ");
    Serial.print( curCodeVal );
    Serial.print(" / ");
    Serial.print( mySwitch.getReceivedBitlength() );
    Serial.print("bit ");
    Serial.print("Protocol: ");
    Serial.println( mySwitch.getReceivedProtocol() );

    long timeDiff = millis()-lastCodeTime;
    if (lastCodeVal == curCodeVal && timeDiff < mqtt_rate_limit_time) {
      //Serial.println("Skipping sending msg.");
    }
    else {
      Serial.println("Sending msg.");

      char cstr[16];
      itoa(curCodeVal, cstr, 10);
      
      client.publish("rfbridge/action", cstr);
      //Serial.println(timeDiff);
      lastCodeTime = millis();
      lastCodeVal = curCodeVal;
    }
    mySwitch.resetAvailable();
  }
  //WIFI watchdog
   if (WiFi.status() != WL_CONNECTED)
    {
      if (wifiDisconnectedMillis == 0) {
        wifiDisconnectedMillis = millis();
      }
      digitalWrite(wifiLedPin, LOW);
      WIFI_Connect();
    } else {
        if (wifiDisconnectedMillis != 0) {
        wifiDisconnectedMillis = 0;
      }
      digitalWrite(wifiLedPin, HIGH);
    }
    
  //MQTT
    if (!client.connected()) {
    reconnect();
  }
  client.loop();

  //handleDiscovery();
  
  //Timers
  long now = millis();
  
  //Keep Alive Msg
  if (now - lastMsg > 300000) {
    lastMsg = now;
    ++value;
    sendKeepAliveMsg();
  }
  
    //Discovery Mode - **not implemented**
    if (modeDiscoveryRequest !=0 && (now - modeDiscoveryRequest > 10000)) {
    modeDiscoveryRequest = 0;
    
  }
}