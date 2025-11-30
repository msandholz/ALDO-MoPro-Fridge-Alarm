#include <Arduino.h>
#include "FS.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPClient.h>
#include <UrlEncode.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <OneButton.h>
 

#define PIN_ALARM_OUTPUT 32                             // IO32 for LED green
#define PIN_CONFIG_MODE_INPUT  25                       // IO25 for Alarm input
#define ONE_WIRE_BUS 4                                  // GPIO 4

#define VERSION "0.9.5"                                 // Software Version 
#define OTA_PASSWORD "aldo"                             // OTA Update Password

#define DEBUG_SERIAL true                               // Enable debbuging over serial interface
#if DEBUG_SERIAL
    #define debug(x) Serial.print(x)
    #define debugf(x, ...) Serial.printf((x), ##__VA_ARGS__)
    #define debugln(x) Serial.println(x)
    #define debug_speed(x) Serial.begin(x)
#else
    #define debug(x)
    #define debugf(x, ...)
    #define debugln(x)
    #define debug_speed(x) 
#endif

// ======================================================================
// Setting parameters with default values
// ======================================================================

// JSON Dcuments to hold configuration and system parameters
JsonDocument config;                                    // Create a JSON document to hold configuration parameters
JsonDocument sys;                                       // Create a JSON document to hold system parameters
String configString;                                    // Create a String to hold JSON configuration parameters  
String sysString;                                       // Create a String to hold JSON system parameters

// Flags and States
volatile bool led_state = false;                        // State of the LED
volatile bool alarmtriggered = false;                   // Flag for alarmtriggered
volatile bool tempTrigger = false;                      // Flag for new Temp available
volatile bool enableNotifications = false;              // Enable WhatsApp Notifications     


// ======================================================================
// Initialize Objects
// ======================================================================
static TimerHandle_t Normal_Mode_timer = nullptr;       // Handle for LED blinking task, when in NORMAL Mode
static TimerHandle_t No_WiFi_timer = nullptr;           // Handle for LED blinking task, when no WiFi connection
static TimerHandle_t Temp_timer = nullptr;              // Handle for getting the Temp
static TimerHandle_t Reminder_timer = nullptr;          // Handle for resetting the alarm

AsyncWebServer server(80);                              // Initialize WebServer
HTTPClient http;                                        // Initialize HTTP Client  
OneWire oneWire(ONE_WIRE_BUS);                          // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
DallasTemperature sensors(&oneWire);                    // Pass our oneWire reference to Dallas Temperature.
OneButton button(PIN_CONFIG_MODE_INPUT, true, true);    // Initialize OneButton for CONFIG mode input pin


// ======================================================================
// Interrupts
// ======================================================================


// ======================================================================
// Forward Declarations
// ======================================================================
void initESP(bool montFS = true);                           // Initialize ESP + gather system parameters

void readConfig(JsonDocument &config);                      // Read configuration from filesystem
void saveConfig(JsonDocument &config);                      // Save configuration to filesystem

bool startWiFi_STA (bool staMode);                          // Start WiFi wether STA or in AP Mode
void enableOTAUpdates();                                    // Enable OTA Updates
void startWebServer();                                      // Start WebServer

TimerHandle_t createPeriodicTimer(const char* TimerName, uint32_t PeriodMS, TimerCallbackFunction_t CallbackFunction); // Create a periodic timer
void blinkLED(TimerHandle_t xTimer);                        // Blink LED timer
void getTemp(TimerHandle_t xTimer);                         // Get Temp timer
void notificationReminder(TimerHandle_t xTimer);            // Sent remind notifications

void switchMode(String mode);                               // Switch the mode <CONFIG>, <CONFIG> or <DEEP_SLEEP>
void switchToConfigMode();                                  // Switch to <CONFIG> mode

void sendWhatsAppNotifications(String Notification);        // Function to send notifications via WhatsApp to all configured numbers
void sendNotification(String Phone_Number, String API_Key, String Notification); // Function to send notifications via WhatsApp to a specific number

// ======================================================================
// Setup
// ======================================================================

void setup() {

    debug_speed(115200);
    debugln("===SETUP_BEGIN===");

    initESP(true);                                      // Initialize ESP + gather system parameters, true = mount filesystem
    readConfig(config);                                 // Read configuration from filesystem

    sensors.begin();                                    // Start up the library
    
    pinMode(LED_BUILTIN, OUTPUT);                       // Initialize the BUILTIN_LED pin as an output
    pinMode(PIN_ALARM_OUTPUT, OUTPUT);                  // Set PIN_ALARM_OUTPUT as Output
    pinMode(PIN_CONFIG_MODE_INPUT, INPUT_PULLUP);       // Set PIN_ALARM_INPUT as Input
   
    // Creating Timers
    Normal_Mode_timer = createPeriodicTimer("Normal Mode LED Timer", 500, blinkLED);     // Create LED timer, when in NORMAL mode
    No_WiFi_timer = createPeriodicTimer("No WiFi LED Timer", 200, blinkLED);             // Create LED timer, when no WiFi connection
    Temp_timer = createPeriodicTimer("Temp Timer", 10000, getTemp);                      // Create timer for reading the temperature every 10 seconds
   
    if(String(config["MODE"]) == "NORMAL") {
        debugln("✅ Starting in <NORMAL> mode");
        button.attachLongPressStart(switchToConfigMode);    // Attach function to check CONFIG mode on long press
        button.setPressMs(5000);                            // Set long press time to 5 seconds

        xTimerStart(Temp_timer, 0);                         // Start Temp timer

        if(startWiFi_STA(true)){
            xTimerStart(Normal_Mode_timer, 0);              // Start Normal Mode LED blinking timer
            enableOTAUpdates();                             // Enable OTA Updates
            startWebServer();                               // Start WebServer
        } else {
            xTimerStart(No_WiFi_timer, 0);                  // Start No WiFi LED blinking timer
        }

    } else if(String(config["MODE"]) == "DEEP_SLEEP") {
        int sleepInterval = config["DEEP_SLEEP_INTERVAL"].as<int>(); // Get DEEP SLEEP interval from config in microseconds
        debugf("💤 Starting in <DEEP_SLEEP> mode - [Interval: %i minutes]\n", sleepInterval);
        digitalWrite(LED_BUILTIN, LOW);                             // Turn the LED off to show <DEEP_SLEEP> mode

        esp_sleep_enable_timer_wakeup(sleepInterval * 60 * 1000000);
        
        getTemp(nullptr);                                           // Get temperature before going to sleep

        float fridge_temp = config["FRIDGE_TEMP"].as<float>();      // Get the temperature in °C
        int target_temp = config["TARGET_TEMP"].as<int>();          // Get the temperature in °C
        
        if(fridge_temp > target_temp) {                             // If Fridge Temp is above Target Temp and alarm not yet triggered
            config["MODE"] = "NORMAL";                              // Set MODE to NORMAL      
            debugf("⚠️ MODE changed form <DEEP_SLEEP> to <NORMAL>!");
            saveConfig(config);                                     // Save configuration to filesystem
            ESP.restart(); 
        } else {
            esp_deep_sleep_start();
        }

    } else {
        debugln("⚙️  Starting in <CONFIG> mode!");
        
        if(startWiFi_STA(false)){
            digitalWrite(LED_BUILTIN, HIGH);                        // Turn the LED on to show <CONFIG> mode
            startWebServer();                                       // Start WebServer
        } else {
            xTimerStart(No_WiFi_timer, 0);                          // Start No WiFi LED blinking timer
            debugln("❌ Failed to start WiFi in AP mode!");
        }   
    }

    debugf("✅ MODE: %s\n", String(config["MODE"])); 

    debugln("===SETUP_END===");
}

// ======================================================================
// Loop
// ======================================================================

void loop() {

    ArduinoOTA.handle();                                                        // Handle OTA updates
    button.tick();                                                              // Check if CONFIG mode should be started

    sys["RSSI"] = WiFi.RSSI();

    if(tempTrigger) {                                                           // If new Temp available
        tempTrigger = false;                                                    // Set newTemp to false

        float fridge_temp = config["FRIDGE_TEMP"].as<float>();                  // Get the temperature in °C
        int target_temp = config["TARGET_TEMP"].as<int>();                      // Get the temperature in °C
        int hysteresis = config["HYSTERESIS"].as<int>();                        // Get the temperature in °C

        if(fridge_temp > target_temp && !config["ALARM"]) { // If Fridge Temp is above Target Temp and alarm not yet triggered
            config["ALARM"] = true;                                             // Set ALARM to true       
            digitalWrite(PIN_ALARM_OUTPUT, HIGH);
            
            debugf("🌡️ Fridge Temp %02.1f °C > Target Temp %i °C (ALARM: %s)\n", fridge_temp, target_temp, String(config["ALARM"] ? "true" : "false"));
            sendWhatsAppNotifications(String("🌡️ ALARM: Aldo MoPro-Kühltheke - Temperatur: " + String(fridge_temp) + "°C (Schwellwert: " + String(target_temp) + "°C)!!")); // Send WhatsApp Notification to all configured numbers    

            if(Reminder_timer == nullptr) {
                int timerReminderMinutes = int(config["REMINDER"])*60000;       // Get Reminder time from config
                Reminder_timer = createPeriodicTimer("Reminder Timer", timerReminderMinutes, notificationReminder); // Create timer for re-sending notifications           
                xTimerStart(Reminder_timer, 0);                                 // Start Reminder timer
            }  

        } else if(fridge_temp < (target_temp - hysteresis) && config["ALARM"]) {                   // If Fridge Temp is below Target Temp - Hysteresis and alarm is triggered
            config["ALARM"] = false;                                            // Set ALARM to false
            digitalWrite(PIN_ALARM_OUTPUT, LOW);                                // Set PIN_ALARM_OUTPUT to LOW

            debugf("🌡️ Fridge Temp %02.1f °C < Target Temp %i °C - Hysteresis %i °C (ALARM: %s)!\n", fridge_temp, target_temp, hysteresis, String(config["ALARM"] ? "true" : "false"));

            if(Reminder_timer != nullptr) {
                xTimerStop(Reminder_timer, 0);                                  // Stop Reminder timer
                xTimerDelete(Reminder_timer, 0);                                // Delete Reminder timer
                Reminder_timer = nullptr;
            }
        } else {
            debugf("🌡️ Fridge Temp %02.1f °C | Target Temp %i °C (ALARM: %s)\n", fridge_temp, target_temp, String(config["ALARM"] ? "true" : "false"));
        }
    }     
}

// ======================================================================
// Declarations
// ======================================================================

// initialize ESP + gather system parameters
void initESP(bool mountFS) {
    sys.clear();                                                                // Clear system JSON document
    sys["chip_model"] = String(ESP.getChipModel());
    sys["chip_revision"] = ESP.getChipRevision();
    sys["chip_cores"] = ESP.getChipCores();
    sys["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
    sys["sdk_version"] = String(ESP.getSdkVersion());
    sys["flash_used"] = ESP.getSketchSize();
    sys["flash_total"] = ESP.getFlashChipSize();
    sys["heap_size"] = ESP.getHeapSize();
    sys["heap_free"] = ESP.getFreeHeap();
 

    debugln("+--------------------------------------------------------------------------");
    debugf("| ChipModel:   %s (Rev.%d) with %d Core(s) and %d MHz\n", sys["chip_model"].as<String>(), sys["chip_revision"].as<int>(), sys["chip_cores"].as<int>(), sys["cpu_freq_mhz"].as<int>());
    debugf("| SDK Version: %s\n", sys["sdk_version"].as<String>().c_str());

    // calculate RAM
    int heap_used = sys["heap_size"].as<int>() - sys["heap_free"].as<int>();
    int heap_total = sys["heap_size"].as<int>();
    float heap_percentage = (float)heap_used / (float)heap_total  * 100;
    debugf("| RAM:         %02.1f%% (used %i bytes from %i bytes)\n", heap_percentage, heap_used, heap_total); 

    // calculate FLASH
    int flash_used = sys["flash_used"].as<int>();
    int flash_total = sys["flash_total"].as<int>();
    float flash_percentage = (float)flash_used / (float)flash_total * 100;
    debugf("| FLASH:       %02.1f%% (used %i bytes from %i bytes)\n", flash_percentage, flash_used, flash_total);          
   
    if(mountFS) { // Mounting LittleFS
        if (!LittleFS.begin(true)) { 
            debugln("| LittlsFS:   An error occurred during LittleFS mounting!");
            return; 
        } else {
            sys["filesystem_used"] = LittleFS.usedBytes();
            sys["filesystem_total"] = LittleFS.totalBytes();

            int filesystem_used = sys["filesystem_used"].as<int>();
            int filesystem_total = sys["filesystem_total"].as<int>();
            
            float filesystem_percentage = (float)filesystem_used / (float)filesystem_total * 100;
            debugf("| LittlsFS:    %02.1f%% (used %i bytes from %i bytes)\n", filesystem_percentage, sys["filesystem_used"], sys["filesystem_total"]);
        } 
    } 
    debugln("+--------------------------------------------------------------------------");
}

// Read Configuration from filesystem
void readConfig(JsonDocument &config) {
    config.clear();                                                              // Clear configuration JSON document
    configString = "";                                                           // Clear configuration String

    if(LittleFS.exists("/config.json")) {
        File configFile = LittleFS.open("/config.json", "r");
        if (configFile) {

            DeserializationError error = deserializeJson(config, configFile);   // Deserialize the JSON document

            config["VERSION"] = VERSION;                                        // Add version to config

            serializeJson(config, configString);                                // convert JSON document to String  
            debugf("✅ Configuration loaded -> %s\n", configString.c_str())  ;

            if (error) { 
                debugf("❌ DeserializeJson failed! -> %s\n", error.f_str());
                return; 
            } 
        }
        configFile.close();

    } else {
        debugln("❌ No configuration file found");
    }
}

// Save Configuration to filesystem
void saveConfig(JsonDocument &config) {
    if(LittleFS.exists("/config.json")) {
        File configFile = LittleFS.open("/config.json","w");
        if(configFile) { 
            if(serializeJsonPretty(config, configFile) == 0) {                  // Serialize the JSON document
                debugln("❌ Failed to write to JSON file");
            }else{
                serializeJson(config, configString);                            // convert JSON document to String    
                debugf("💾 Configuration saved -> %s\n", configString.c_str())  ;
            }
             
        configFile.close();

        } else {
             debugln("❌ No configuration file found");
        }
    }
} 

// Start WiFi STA Mode
bool startWiFi_STA (bool staMode) {
    
    String hostname = String(config["HOSTNAME"]);
    WiFi.setHostname(hostname.c_str());

    if (staMode){
        String ssid_sta = String(config["WIFI_STA_SSID"]);
        String sta_pw = String(config["WIFI_STA_PW"]);
        sys["SSID"] = ssid_sta;
        sys["WiFi_Mode"] = WiFi.getMode();

        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid_sta, sta_pw);

        debugf("📶 Connecting WiFi STA with %s ", ssid_sta);

        unsigned long startAttemptTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
            delay(500);
            debug(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            debugln("✅ WiFi connected!");
            debugf("✅ IP: %s\n", WiFi.localIP().toString().c_str());
            debugf("✅ Hostname: %s\n", hostname.c_str());
            sys["RSSI"] = WiFi.RSSI();
            sys["channel"] = WiFi.channel();


            if (MDNS.begin(hostname.c_str())) {
                debugf("✅ mDNS: http://%s.local\n", hostname.c_str());
            } else {
                debugln("❌ Error setting up MDNS responder!");
            } 

            return true;
        } else {
            debugln("\n❌ WiFi connection failed!");
            return false;
        }

    } else {
        String ssid_ap = String(config["WIFI_AP_SSID"]);
        sys["SSID"] = ssid_ap;
        sys["WiFi_Mode"] = WiFi.getMode();
        WiFi.mode(WIFI_AP);
        debug("📶 Starting WiFi AP...");

        if (WiFi.softAP(ssid_ap)) {
            debugf("✅ WiFi AP: %s startet!\n", WiFi.softAPSSID().c_str());
            debugf("✅ IP: %s\n", WiFi.softAPIP().toString().c_str());
            debugf("✅ Hostname: %s\n", hostname.c_str());
            sys["RSSI"] = WiFi.RSSI();
            sys["channel"] = WiFi.channel();

            if (MDNS.begin(hostname.c_str())) {
                debugf("✅ mDNS: http://%s.local\n", hostname.c_str());
            } else {
                debugln("❌ Error setting up MDNS responder!");
            } 

            return true;
        } else {
            debugln("❌ Failed to start AP!");
            return false;
        }      
    }
}      

// Start WebServer
void startWebServer() { 

    // Make test available
    server.on("/test", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Hello, world");
    });

    // Make style.css available
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/style.css","text/css");
    });

    // Make favicon.ico available
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
        debugln("✅ Requested /favicon.ico");
        request->send(LittleFS, "/cold-32.png","image/png");
    });

    // Make icons.svg available
    server.on("/icons.svg", HTTP_GET, [](AsyncWebServerRequest *request){
        debugln("✅ Requested /icons.svg");
        request->send(LittleFS, "/icons.svg","image/svg+xml");
    });

    // Make index.html available
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        debugln("🌐 Requested index.html");
        request->send(LittleFS, "/index.html", "text/html");
    });

    // Make config.html available
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
        debugln("🌐 Requested config.html");
        request->send(LittleFS, "/config.html", "text/html");
    });


    // Make system.html available
    server.on("/system", HTTP_GET, [](AsyncWebServerRequest *request){
         debugln("🌐 Requested system.html");
        request->send(LittleFS, "/system.html", "text/html");
    });

    // 
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        debugln("🆙 Update Done, restarting...");
        request->send(200, "text/plain", "Update Done, restarting...");
        ESP.restart();
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index){
            debugf("🆙 Update Start: %s\n", filename.c_str());
            if(!Update.begin(UPDATE_SIZE_UNKNOWN)){ //start with max available size
                Update.printError(Serial);
            }
        }
        if(!Update.hasError()){
            if(Update.write(data, len) != len){
                Update.printError(Serial);
            }
        }
        if(final){
            if(Update.end(true)){ //true to set the size to the current progress
                debugf("🆙 Update Success: %u bytes\n", index+len);
            } else {
                Update.printError(Serial);
            }
        }
    });
 
    // Make configutation data available
    server.on("/getdata", HTTP_GET, [](AsyncWebServerRequest *request){      
                              
        int paramsNr = request->params();
        debugf("📬 /getdata with %i parameters: ", paramsNr);
        for(int i=0;i<paramsNr;i++){
            const AsyncWebParameter* p = request->getParam(i);   
            const String name = p->name();
            const String value = p->value();

            debugf("%s:%s, ", name.c_str(), value.c_str());

            if(name == "MODE" && value != String(config["MODE"])) {                                 // If MODE changed, set new MODE
                switchMode(value); 
                continue;
            }

            if (config[name].is<bool>()) { config[name] = value == "true" ? true : false; }         // Convert "true"/"false" to boolean
                else if (config[name].is<int>()) { config[name] = value.toInt(); }                  // Convert to integer
                else if (config[name].is<float>()) { config[name] = value.toFloat(); }              // Convert to float
                else { config[name] = p->value(); }                                                 // Else keep as string  
        }

        debugln("");
        if (paramsNr > 0) {
            saveConfig(config);                 // Save configuration to filesystem  
        }
            
        serializeJson(config, configString);    // convert JSON document to String    

        request->send(200, "application/json", configString);       
    });

    // Make system data available
    server.on("/getsys", HTTP_GET, [](AsyncWebServerRequest *request){
        serializeJson(sys, sysString); 
        request->send(200, "application/json", sysString);

    });

    server.begin();
    debugln("🌐 WebServer started");

}

// Enable OTA Updates
void enableOTAUpdates() {

    ArduinoOTA.setHostname(String(config["HOSTNAME"]).c_str());
    ArduinoOTA.setMdnsEnabled(true);
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        debugln("🆙 Starting OTA Update...");
    });

    ArduinoOTA.onEnd([]() {
        debugln("\n✅ OTA Update successfull!");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        debugf("🔄 OTA update progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
    debugf("\n❌ ERROR [%u] while OTA update: ", error);

    if (error == OTA_AUTH_ERROR) debugln("Authentication failed!");
    else if (error == OTA_BEGIN_ERROR) debugln("Failed to start OTA update!");
    else if (error == OTA_CONNECT_ERROR) debugln("Failed to connect to OTA server!");
    else if (error == OTA_RECEIVE_ERROR) debugln("Error receiving OTA data!");
    else if (error == OTA_END_ERROR) debugln("Error receiving OTA data!");
  });

  ArduinoOTA.begin();
}

// Create a periodic timer
TimerHandle_t createPeriodicTimer(const char* TimerName, uint32_t PeriodMS, TimerCallbackFunction_t CallbackFunction) {
    TimerHandle_t timerHandle = xTimerCreate(                 // Define timer
        TimerName,                                            // Name of timer
        PeriodMS/portTICK_PERIOD_MS,                          // Period of timer (in ticks)
        pdTRUE,                                               // Auto-reload
        (void *)1,                                            // Timer ID
        CallbackFunction);                                    // Callback function

    if (timerHandle == nullptr) {
        debugf("❌ Failed to create timer: %s\n", TimerName);
    } else {
        debugf("✅ Timer created: %s (Period: %i ms)\n", TimerName, PeriodMS);
    }

    return timerHandle;
}

// blinking LED task
void blinkLED(TimerHandle_t xTimer) {
    led_state = !led_state;
    digitalWrite(LED_BUILTIN, led_state);
}

// get Temp task
void getTemp(TimerHandle_t xTimer) {
    
    sensors.requestTemperatures();                          // Temperaturmessung anstoßen
    float tempC = sensors.getTempCByIndex(0);               // Temperatur des ersten Sensors lesen
    if(tempC == DEVICE_DISCONNECTED_C) {
        debugln("❌ Error: Could not read temperature data");
        return;
    }

    config["FRIDGE_TEMP"] = round(tempC * 10) / 10.0;        // Set TEMP_C to current temperature
    if (config["FRIDGE_TEMP"] < config["MIN_TEMP"]) {        // Set MIN_TEMP to current temperature
        config["MIN_TEMP"] = config["FRIDGE_TEMP"];
        saveConfig(config);                                   
    }
    if (config["FRIDGE_TEMP"] > config["MAX_TEMP"]) {        // Set MAX_TEMP to current temperature
        config["MAX_TEMP"] = config["FRIDGE_TEMP"];          
        saveConfig(config);
    }
    tempTrigger = true;                                      // Set newTemp to true

}

// function to send notifications via WhatsApp to all configured numbers
void sendWhatsAppNotifications(String Notification) {
    if (config["NOTIFICATION"] == true) {
        if(String(config["PHONE_NUMBER_1"]) != "" && String(config["API_KEY_1"]) != "") {
            sendNotification(String(config["PHONE_NUMBER_1"]), String(config["API_KEY_1"]), Notification);
        }
        if(String(config["PHONE_NUMBER_2"]) != "" && String(config["API_KEY_2"]) != "") {
            sendNotification(String(config["PHONE_NUMBER_2"]), String(config["API_KEY_2"]), Notification);
        }
        if(String(config["PHONE_NUMBER_3"]) != "" && String(config["API_KEY_3"]) != "") {
            sendNotification(String(config["PHONE_NUMBER_3"]), String(config["API_KEY_3"]), Notification);
        }
    }           
}

// function to send a notification via WhatsApp
void sendNotification(String Phone_Number, String API_Key, String Notification) {

    String url = "https://api.callmebot.com/whatsapp.php?phone=" + Phone_Number + "&apikey=" + API_Key + "&text="+urlEncode(Notification);    

    http.begin(url);                                                        // Data to send with HTTP POST  
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");    // Specify content-type header
          
    int httpResponseCode = http.POST("");                                  // Send HTTP POST request
    if (httpResponseCode == 200){
        debugf("📦 WHATSAPP Notification sent to <+%s>\n", Phone_Number);
    } else {
        debugf("❌ WHATSAPP Notification -> ERROR: HTTP response code: %i\n",httpResponseCode);
    }

    http.end();                                                             // Free resources
  }

// function for automatically resetting the alarm after a defined time
void notificationReminder(TimerHandle_t xTimer) {

    if (config["NOTIFICATION"] == true) {
        debugln("📦 Notification reminder!");
        sendWhatsAppNotifications(String("🌡️ Erinnerung: AlDo MoPro-Kühltheke immer noch zu warm!! (Temperatur: " + config["FRIDGE_TEMP"].as<String>() + "°C)")); // Send WhatsApp Notification to all configured numbers    
    }   
}

// Function to switch between the modes <CONFIG>, <CONFIG> or <DEEP_SLEEP>
void switchMode(String mode) {
    debugf("⚠️ MODE changed to <%s>. A restart is required to apply the new mode!", mode.c_str());
    config["MODE"] = mode;
    saveConfig(config);                               // Save configuration to filesystem
    ESP.restart();  
}

// Function to switch to <CONFIG> mode
void switchToConfigMode() {
    switchMode("CONFIG");
}