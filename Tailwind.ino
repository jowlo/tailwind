#include "ConfigManager.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include <BLEUtils.h>
#include <BLE2902.h>

const char *settingsHTML = (char *)"/settings.html";
const char *stylesCSS = (char *)"/styles.css";
const char *mainJS = (char *)"/main.js";
const char *fanSVG = (char *)"/fan.svg";


// The remote service we wish to connect to.
static BLEUUID serviceUUID("180D");
// The characteristic of the remote service we are interested in.
static BLEUUID charUUID("2A37");

static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* ble_device;


static BLEServer* ble_server;
static BLEService* ble_local_service;
static BLECharacteristic* ble_local_characteristic;


#define numFanStates 3
static uint8_t levelPins[numFanStates] = {12, 14, 27};

static uint8_t hr_rec[2] = {0,0};


struct Config {
  char device_name[32];
  uint8_t HR_threshold_1 = 80;
  uint8_t HR_threshold_2 = 120;
  uint8_t HR_threshold_3 = 140;
  bool enabled = true;

  bool ble_scan = true;
  bool ble_hr_found = false;
  bool ble_connected = false;

  uint8_t hr = 0;
  uint8_t fan_level = 0;
 
  int8_t led;
} config;

static void setFanLevel(uint8_t level){
  // 0: off
  // 1: some
  // 2: more
  // 3: full

  level = max(((uint8_t)0), min(((uint8_t)3), level));

  for(size_t i = 0; i < numFanStates; i++){
    digitalWrite(levelPins[i], HIGH);
  }

  if(level-1 < numFanStates){
    digitalWrite(levelPins[level-1], LOW);
  }
 
}


static void BLE_notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
    
    ble_local_characteristic->setValue(pData, length);
    ble_local_characteristic->notify();
    DebugPrintln("[BLE Server] Notify sent"); 

    
    if(pData[1] == config.hr) return;


    
    config.hr = pData[1];
    
    uint8_t fanLevel = 0;
    
    if(pData[1] >= config.HR_threshold_1) fanLevel++;
    if(pData[1] >= config.HR_threshold_2) fanLevel++;
    if(pData[1] >= config.HR_threshold_3) fanLevel++;

    config.fan_level = fanLevel;
    DebugPrint("Setting fan level: ");
    DebugPrintln(config.fan_level);

    DebugPrint("HR: ");
    DebugPrintln(pData[1]);
    
}


class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    config.ble_connected = true;
    setFanLevel(0);
  }

  void onDisconnect(BLEClient* pclient) {
    config.ble_connected = false;
    config.ble_hr_found = false;
    BLEDevice::getAdvertising()->stop();
    DebugPrintln("[BLE] Disconnect");
    setFanLevel(0);
  }
};


ConfigManager configManager;

void APCallback(WebServer *server) {
    server->on("/styles.css", HTTPMethod::HTTP_GET, [server](){
        configManager.streamFile(stylesCSS, mimeCSS);
    });
    
    server->on("/fan.svg", HTTPMethod::HTTP_GET, [server](){
      configManager.streamFile(fanSVG, mimeSVG);
    });
    
}


void APICallback(WebServer *server) {

  configManager.setAPFilename("/index_api.html");
  
  server->on("/ble_connect", HTTPMethod::HTTP_GET, [server](){
    BLE_scan();
    server->send(202);
  });


  server->on("/ble_disconnect", HTTPMethod::HTTP_GET, [server](){
    config.ble_connected = false;
    config.ble_hr_found = false;
    setFanLevel(0);
    BLEDevice::deinit();
    server->send(202);
  });
  
  server->on("/disconnect", HTTPMethod::HTTP_GET, [server](){
    configManager.clearWifiSettings(true);
  });

  server->on("/manifest.json", HTTPMethod::HTTP_GET, [server](){
    configManager.streamFile("/manifest.json", mimeJSON);
  });
  
  server->on("/settings.html", HTTPMethod::HTTP_GET, [server](){
    configManager.streamFile(settingsHTML, mimeHTML);
  });
  
  // NOTE: css/js can be embedded in a single page HTML 
  server->on("/styles.css", HTTPMethod::HTTP_GET, [server](){
    configManager.streamFile(stylesCSS, mimeCSS);
  });
  
  server->on("/main.js", HTTPMethod::HTTP_GET, [server](){
    configManager.streamFile(mainJS, mimeJS);
  });

  server->on("/fan.svg", HTTPMethod::HTTP_GET, [server](){
    configManager.streamFile(fanSVG, mimeSVG);
  });

}


class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      DebugPrintln("[BLE Server] Client connected");
    };

    void onDisconnect(BLEServer* pServer) {
      DebugPrintln("[BLE Server] Client disconnected");
    }
};


bool connectToDevice() {
    DebugPrint("Forming a connection to ");
    DebugPrintln(ble_device->getAddress().toString().c_str());
    DebugPrintln(ble_device->getName().c_str());
    
    DebugPrintln("Creating client");
    BLEClient*  ble_client = BLEDevice::createClient();

    DebugPrintln("Setting callbacks");
    ble_client->setClientCallbacks(new MyClientCallback());
  
    // Connect to the remove BLE Server.
    DebugPrintln("Connecting to server");
    ble_client->connect(ble_device);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)

    // Obtain a reference to the service we are after in the remote BLE server.
    DebugPrintln("Connecting to service");    
    BLERemoteService* pRemoteService = ble_client->getService(serviceUUID);
    if (pRemoteService == nullptr) {
      DebugPrint("Failed to find our service UUID: ");
      DebugPrintln(serviceUUID.toString().c_str());
      ble_client->disconnect();
      return false;
    }

    DebugPrintln("Searching characteristic");    
    // Obtain a reference to the characteristic in the service of the remote BLE server.
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) {
      DebugPrint("Failed to find our characteristic UUID: ");
      DebugPrintln(charUUID.toString().c_str());
      ble_client->disconnect();
      return false;
    }
    
    DebugPrintln("Registering Notification");    
    if(pRemoteCharacteristic->canNotify()){
      pRemoteCharacteristic->registerForNotify(BLE_notifyCallback);
    } else {
      DebugPrint("Notify subscription failed");      
    }

    ble_server = BLEDevice::createServer();
    DebugPrintln("[BLE Server] created"); 
    ble_server->setCallbacks(new MyServerCallbacks());     
    DebugPrintln("[BLE Server] Callbacks registered"); 


    ble_local_service = ble_server->createService(serviceUUID);
    DebugPrintln("[BLE Server] Service created");      
    ble_local_characteristic = ble_local_service->createCharacteristic(charUUID, 
      BLECharacteristic::PROPERTY_NOTIFY);
    ble_local_characteristic->addDescriptor(new BLE2902());
    DebugPrintln("[BLE Server] Characteristic created");

    ble_local_service->start();
    DebugPrintln("[BLE Server] Service started");

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(serviceUUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    DebugPrintln("[BLE Server] Advertising started");
    
    return true;

    
}






/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
 /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {

      BLEDevice::getScan()->stop();
      ble_device = new BLEAdvertisedDevice(advertisedDevice);
      DebugPrintln("New BLEAdvertisedDevice");
      config.ble_hr_found = true;
    }
  }
};

void fan_setup(){
  for(size_t i = 0; i < numFanStates; i++){
    pinMode(levelPins[i], OUTPUT);
    digitalWrite(levelPins[i], HIGH);
  }

  config.fan_level = 0;
  DebugPrint("Setting fan level: ");
  DebugPrintln(config.fan_level);
}

void BLE_setup(){
  BLEDevice::init("Tailwind Fan");
  config.ble_scan = false;
  config.ble_hr_found = false;
  config.ble_connected = false;
}

  
  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
void BLE_scan(){
  if(!BLEDevice::getInitialized()) {
    BLEDevice::init("Tailwind Fan");
  }
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(2, false);
  config.ble_scan = false;
}

void setup() {
  DEBUG_MODE = true;
  Serial.begin(115200);
  DebugPrintln(F(""));

  fan_setup();
  
  // Setup config manager
  configManager.setAPName("Tailwind");
  configManager.setAPFilename("/index.html");

  //Set defaults
  strncpy(config.device_name, "Tailwind Fan", 13);
  config.HR_threshold_1 = 80;
  config.HR_threshold_2 = 120;
  config.HR_threshold_3 = 140;

  // Settings variables 
  configManager.addParameter("device_name", config.device_name, 32);
  configManager.addParameter("HR_threshold_1", &config.HR_threshold_1);
  configManager.addParameter("HR_threshold_2", &config.HR_threshold_2);
  configManager.addParameter("HR_threshold_3", &config.HR_threshold_3);
  configManager.addParameter("enabled", &config.enabled);
  configManager.addParameter("ble_scan", &config.ble_scan);
  configManager.addParameter("ble_connected", &config.ble_connected, get);


  configManager.addParameter("hr", &config.hr, get);
  configManager.addParameter("fan_level", &config.fan_level);

  // Init Callbacks
  configManager.setAPCallback(APCallback);
  configManager.setAPICallback(APICallback);
  configManager.setWifiConnectRetries(100);
  configManager.setWifiConnectInterval(1000);
  
  configManager.begin(config);

  BLE_setup();
}

void loop() {
  configManager.loop();
  if(!config.ble_connected && config.ble_hr_found) connectToDevice();
  setFanLevel(config.fan_level);
}
