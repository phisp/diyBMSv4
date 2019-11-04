/*

 ____  ____  _  _  ____  __  __  ___    _  _  __
(  _ \(_  _)( \/ )(  _ \(  \/  )/ __)  ( \/ )/. |
 )(_) )_)(_  \  /  ) _ < )    ( \__ \   \  /(_  _)
(____/(____) (__) (____/(_/\/\_)(___/    \/   (_)

  (c) 2017/18/19 Stuart Pittaway

  This is the code for the controller - it talks to the V4 cell modules over isolated serial bus

  This code runs on ESP-8266-12E (NODE MCU 1.0) and compiles with Arduino 1.8.5 environment

  Arduino settings
  NodeMCU 1.0 (ESP-12E module), Flash 4M (3MSPIFF), CPU 80MHZ

  Setting up ESP-8266-12E (NODE MCU 1.0) on Arduino
  http://www.instructables.com/id/Programming-a-HTTP-Server-on-ESP-8266-12E/

  Installation Instruction:
  Tools -> Manage Libraries... -> Search: packetserial -> Packetserial by Christopher Baker (https://github.com/bakercp/PacketSerial)
  Libraries:
  Packetserial by Christopher Baker (https://github.com/bakercp/PacketSerial)
  ArduinoJSON by Blenoit Blanchon (https://github.com/bblanchon/ArduinoJson)
  
  Download 
  https://github.com/me-no-dev/ESPAsyncWebServer,
  https://github.com/me-no-dev/ESPAsyncTCP,
  https://github.com/me-no-dev/ESPAsyncUDP,
  https://github.com/marvinroger/async-mqtt-client,
  https://github.com/SMFSW/Queue,
  https://github.com/WereCatf/PCF8574_ESP,
  https://github.com/PaulStoffregen/Time,
  https://github.com/gmag11/NtpClient,
  https://github.com/marvinroger/ESP8266TrueRandom
  select the “Download ZIP” option
  and the file should be downloaded to your computer.
  Just open the .zip file and extract the folder to your Arduino libraries folder.
  Usually, the libraries folder for the Arduino installation is located on the C:\Users\UserName\Documents\Arduino\libraries folder.
  You can remove the -master from the name if you want.
  This applies to install both libraries. 
*/

/*
   PINS
   D0 = GREEN_LED
   D1 = i2c SDA
   D2 = i2c SCL
   D3 = switch to ground (reset WIFI configuration on power up)
   D4 = GPIO2 = TXD1 = TRANSMIT DEBUG SERIAL (and blue led on esp8266)
   D5 = GPIO14 = Interrupt in from PCF8574
   D7 = GPIO13 = RECEIVE SERIAL
   D8 = GPIO15 = TRANSMIT SERIAL


   DIAGRAM
   https://www.hackster.io/Aritro/getting-started-with-esp-nodemcu-using-arduinoide-aa7267
*/


#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Hash.h>
#include <ESPAsyncWebServer.h>
#include <AsyncMqttClient.h>
#include <PacketSerial.h>
#include <cppQueue.h>
#include <Ticker.h>
#include <pcf8574_esp.h>
#include <Wire.h>

//Debug flags for ntpclientlib
#define DBG_PORT Serial1
#define DEBUG_NTPCLIENT

#include <TimeLib.h>
#include <NtpClientLib.h>

#include "include/defines.h"

bool PCF8574Enabled;

uint16_t ConfigHasChanged=0;
diybms_eeprom_settings mysettings;

AsyncWebServer server(80);

bool server_running=false;
bool wifiFirstConnected = false;
boolean NTPsyncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent; // Last triggered event

uint8_t packetType=0;

//PCF8574P has an i2c address of 0x38 instead of the normal 0x20
PCF857x pcf8574(0x38, &Wire);

volatile bool PCFInterruptFlag = false;

void ICACHE_RAM_ATTR PCFInterrupt() {
  PCFInterruptFlag = true;
}

//This large array holds all the information about the modules
//up to 4x16
CellModuleInfo cmi[4][maximum_cell_modules];
//Size of array
uint8_t numberOfModules[4];


#include "lib/crc16/crc16.h"
#include "lib/crc16/crc16.cpp" // <-- muss hier includiert werden, sonst wird die Class nicht gefunden

#include "lib/settings/settings.h"
#include "lib/settings/settings.cpp" // <-- muss hier includiert werden, sonst wird die Class nicht gefunden
#include "include/SoftAP.h"
#include "include/DIYBMSServer.h"
#include "include/PacketRequestGenerator.h"
#include "include/PacketReceiveProcessor.h"


// Instantiate queue to hold packets ready for transmission
Queue	requestQueue(sizeof(packet), 16, FIFO);

PacketRequestGenerator prg=PacketRequestGenerator(&requestQueue);

PacketReceiveProcessor receiveProc=PacketReceiveProcessor();

PacketSerial_<COBS, 0, 128> myPacketSerial;

volatile bool waitingForReply=false;

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;

Ticker myTimerRelay;

Ticker myTimer;
Ticker myTransmitTimer;
Ticker wifiReconnectTimer;
Ticker mqttReconnectTimer;
Ticker myTimerSendMqttPacket;
Ticker myTimerSendInfluxdbPacket;

uint16_t sequence=0;

AsyncMqttClient mqttClient;

void dumpPacketToDebug(packet *buffer) {
  Serial1.print(buffer->address,HEX);
  Serial1.print('/');
  Serial1.print(buffer->command,HEX);
  Serial1.print('/');
  Serial1.print(buffer->sequence,HEX);
  Serial1.print('=');
  for (size_t i = 0; i < maximum_cell_modules; i++)
  {
    Serial1.print(buffer->moduledata[i],HEX);
    Serial1.print(" ");
  }
  Serial1.print(" =");
  Serial1.print(buffer->crc,HEX);
}

uint16_t minutesSinceMidnight() {
  return (hour() * 60) + minute();
}

void processSyncEvent (NTPSyncEvent_t ntpEvent) {
    if (ntpEvent < 0) {
        Serial1.printf ("Time Sync error: %d\n", ntpEvent);
        if (ntpEvent == noResponse)
            Serial1.println ("NTP server not reachable");
        else if (ntpEvent == invalidAddress)
            Serial1.println ("Invalid NTP server address");
        else if (ntpEvent == errorSending)
            Serial1.println ("Error sending request");
        else if (ntpEvent == responseError)
            Serial1.println ("NTP response error");
    } else {
        if (ntpEvent == timeSyncd) {
            Serial1.print ("Got NTP time: ");
            Serial1.println (NTP.getTimeDateString (NTP.getLastNTPSync()));
        }
    }
}


void onPacketReceived(const uint8_t* receivebuffer, size_t len)
{
  //Note that this function gets called frequently with zero length packets
  //due to the way the modules operate

  if (len==sizeof(packet)) {
    // Process decoded incoming packet
    Serial1.print("Recv:");
    dumpPacketToDebug((packet*)receivebuffer);

    if (!receiveProc.ProcessReply(receivebuffer,sequence)) {
      Serial1.println("** FAILED PROCESS REPLY **");
    }

    //We received a packet (although may have been an error)
    waitingForReply=false;

    Serial1.println();
  }
}


void timerTransmitCallback() {
  //Don't send another message until we have received reply from the last one
  //this slows the transmit process down a lot so potentially need to look at a better
  //way to do this and also keep track of missing messages replies for error tracking
  if (waitingForReply) {
    //Serial1.print('E');Serial1.print(receiveProc.commsError);

    //Increment the counter to watch for complete comms failures
    receiveProc.commsError++;

    //After 5 attempts give up and send another packet
    if (receiveProc.commsError>10) {
      waitingForReply=false;
      receiveProc.totalMissedPacketCount++;
    } else {
        return;
    }
  }

  //Called every second to transmit anything that remains in the transmit queue
  if (!requestQueue.isEmpty()) {
    packet transmitBuffer;

    GREEN_LED_ON;

    //Wake up the connected cell module from sleep
    Serial.write(0x00);
    delay(10);

    requestQueue.pop(&transmitBuffer);

    sequence++;

    transmitBuffer.sequence=sequence;
    //transmitBuffer.crc = uCRC16Lib::calculate((char*)&transmitBuffer, sizeof(packet) - 2);
    transmitBuffer.crc =CRC16::CalculateArray((uint8_t*)&transmitBuffer, sizeof(packet) - 2);

    myPacketSerial.send((byte*)&transmitBuffer, sizeof(transmitBuffer));

    waitingForReply=true;

    Serial1.print("Send:");
    dumpPacketToDebug(&transmitBuffer);

    Serial1.print("/Q:");
    Serial1.print(requestQueue.getCount());
    Serial1.print(" # ");

    GREEN_LED_OFF;
  }
}


bool rule_outcome[RELAY_RULES];

void timerProcessRules() {

  uint32_t packvoltage[4];

  packvoltage[0]=0;
  packvoltage[1]=0;
  packvoltage[2]=0;
  packvoltage[3]=0;

  for (int8_t r = 0; r < RELAY_RULES; r++)
  {
    rule_outcome[r]=false;
  }


  //If we have a communications error
  if (receiveProc.commsError > 2) {
    rule_outcome[0]=true;
  }




  //Loop through cells
  for (int8_t bank = 0; bank < mysettings.totalNumberOfBanks; bank++)
  {
    for (int8_t i = 0; i < numberOfModules[bank]; i++) {

      packvoltage[bank]+=cmi[bank][i].voltagemV;

      if (cmi[bank][i].voltagemV > mysettings.rulevalue[1]) {
          //Rule 1 - Individual cell over voltage
          rule_outcome[1]=true;
      }

      if ((cmi[bank][i].externalTemp!=-40) && (cmi[bank][i].externalTemp > mysettings.rulevalue[2])) {
          //Rule 2 - Individual cell over temperature (external probe)
          rule_outcome[2]=true;
      }
    }
  }

  //Combine the voltages if we need to
  if (mysettings.combinationParallel==false) {
    packvoltage[0]+=packvoltage[1]+packvoltage[2]+packvoltage[3];
    packvoltage[1]=0;
    packvoltage[2]=0;
    packvoltage[3]=0;
  }

  for (int8_t bank = 0; bank < mysettings.totalNumberOfBanks; bank++)
  {
    if (packvoltage[bank] > mysettings.rulevalue[3]) {
      //Rule 3 - Pack over voltage (mV)
      rule_outcome[3]=true;
    }

    if (packvoltage[bank] < mysettings.rulevalue[4]) {
      //Rule 4 - Pack under voltage (mV)
      rule_outcome[4]=true;
    }
  }

  //Time based rules
  if (minutesSinceMidnight() >= mysettings.rulevalue[5]) {
    rule_outcome[5]=true;
  }
  if (minutesSinceMidnight() >= mysettings.rulevalue[6]) {
    rule_outcome[6]=true;
  }

  // DO NOTE: When you write LOW to a pin on a PCF8574 it becomes an OUTPUT.
  // It wouldn't generate an interrupt if you were to connect a button to it that pulls it HIGH when you press the button.
  // Any pin you wish to use as input must be written HIGH and be pulled LOW to generate an interrupt.

  Serial1.print("Rules:");
  for (int8_t r = 0; r < RELAY_RULES; r++)
  {
    Serial1.print(rule_outcome[r]);
    Serial1.print(" ");
  }
  Serial1.print("=");

  uint8_t relay[RELAY_TOTAL];

  //Set defaults based on configuration
  for (int8_t y = 0; y<RELAY_TOTAL; y++)
  {
    relay[y]=  mysettings.rulerelaydefault[y]==RELAY_ON ? LOW:HIGH;
  }

  //Test the rules (in reverse order)
  for (int8_t n = RELAY_RULES-1; n>=0; n--)
  {
    if (rule_outcome[n]==true) {

      for (int8_t y = 0; y<RELAY_TOTAL; y++)
      {
        //Dont change relay if its set to ignore/X
        if (mysettings.rulerelaystate[n][y]!=RELAY_X) {
            //Logic is inverted on the PCF chip
            if (mysettings.rulerelaystate[n][y]==RELAY_ON) {
              relay[y]=LOW;
            } else {
              relay[y]=HIGH;
            }
        }
      }
      }
  }

  if (PCF8574Enabled) {
    //Perhaps we should publish the relay settings over MQTT and INFLUX/website?
    for (int8_t n = 0; n<RELAY_TOTAL; n++)
    {
      //Would be better here to use the WRITE8 to lower i2c traffic
      pcf8574.write(n, relay[n]);
      Serial1.print(' ');
      Serial1.print(relay[n]);
    }
    Serial1.println("");
  } else {
    Serial1.println("N/F");
  }

  //pcf8574.write8(relayState);
}

void timerEnqueueCallback() {
  //this is called regularly on a timer, it determines what request to make to the modules (via the request queue)
  for (uint8_t b = 0; b < mysettings.totalNumberOfBanks; b++)
  {
    prg.sendCellVoltageRequest(b);
    prg.sendCellTemperatureRequest(b);
  }

}


void connectToWifi() {
  Serial1.println("Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(DIYBMSSoftAP::WifiSSID(), DIYBMSSoftAP::WifiPassword());
}


void connectToMqtt() {
  Serial1.println("Connecting to MQTT...");
  mqttClient.connect();
}

static AsyncClient * aClient = NULL;

void setupInfluxClient()
{

    if (aClient) //client already exists
        return;

    aClient = new AsyncClient();
    if (!aClient) //could not allocate client
        return;

    aClient->onError([](void* arg, AsyncClient* client, err_t error) {
        Serial1.println("Connect Error");
        aClient = NULL;
        delete client;
    }, NULL);

    aClient->onConnect([](void* arg, AsyncClient* client) {
        Serial1.println("Connected");

        //Send the packet here

        aClient->onError(NULL, NULL);

        client->onDisconnect([](void* arg, AsyncClient* c) {
            Serial1.println("Disconnected");
            aClient = NULL;
            delete c;
        }, NULL);

        client->onData([](void* arg, AsyncClient* c, void* data, size_t len) {
            //Data received
            Serial1.print("\r\nData: ");Serial1.println(len);
            //uint8_t* d = (uint8_t*)data;
            //for (size_t i = 0; i < len; i++) {Serial1.write(d[i]);}
        }, NULL);

        //send the request


        //Construct URL for the influxdb
        //See API at https://docs.influxdata.com/influxdb/v1.7/tools/api/#write-http-endpoint

        String poststring;

        for (uint8_t bank = 0; bank < 4; bank++) {
          for (uint8_t i = 0; i < numberOfModules[bank]; i++) {

            //Data in LINE PROTOCOL format https://docs.influxdata.com/influxdb/v1.7/write_protocols/line_protocol_tutorial/
            poststring = poststring
                + "cells,"
                +"cell=" + String(bank+1)+"_"+String(i+1)
                + " v=" + String((float)cmi[bank][i].voltagemV/1000.0)
                + ",inttemp=" + String(cmi[bank][i].internalTemp)+"i"
                + ",bypass=" + (cmi[bank][i].inBypass ? String("true"):String("false"));

                if (cmi[bank][i].externalTemp!=-40) {
                  //Ensure its valid
                  poststring = poststring + ",exttemp=" + String(cmi[bank][i].externalTemp)+"i";
                }
                poststring = poststring + "\n";
          }
        }

        //TODO: Need to URLEncode these values
        //+ String(mysettings.influxdb_host) + ":" + String(mysettings.influxdb_httpPort)
        String url = "/write?db=" + String(mysettings.influxdb_database)
            +"&u=" + String(mysettings.influxdb_user)
            +"&p=" + String(mysettings.influxdb_password);

        String header="POST "+url+" HTTP/1.1\r\n"
        +"Host: "+String(mysettings.influxdb_host)+"\r\n"
        +"Connection: close\r\n"
        +"Content-Length: "+poststring.length()+"\r\n"
        +"Content-Type: text/plain\r\n"

        +"\r\n";

        Serial1.println(header.c_str());
        Serial1.println(poststring.c_str());

        client->write(header.c_str());
        client->write(poststring.c_str());

    }, NULL);
}

void SendInfluxdbPacket() {
  if (!mysettings.influxdb_enabled) return;

  Serial1.println("SendInfluxdbPacket");

  setupInfluxClient();

  if(!aClient->connect(mysettings.influxdb_host, mysettings.influxdb_httpPort )){
    Serial1.println("Influxdb connect fail");
    AsyncClient * client = aClient;
    aClient = NULL;
    delete client;
  }
}


void startTimerToInfluxdb() {
  myTimerSendInfluxdbPacket.attach(30, SendInfluxdbPacket);
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {

  wifiFirstConnected = true;

  Serial1.println("Connected to Wi-Fi.");
  Serial1.print( WiFi.status() );
  Serial1.print(F(". Connected IP:"));
  Serial1.println(WiFi.localIP());

  /*
  TODO: CHECK ERROR CODES BETTER!
  0 : WL_IDLE_STATUS when Wi-Fi is in process of changing between statuses
  1 : WL_NO_SSID_AVAIL in case configured SSID cannot be reached
  3 : WL_CONNECTED after successful connection is established
  4 : WL_CONNECT_FAILED if password is incorrect
  6 : WL_DISCONNECTED if module is not configured in station mode
  */
  if (!server_running)
  {
    DIYBMSServer::StartServer(&server);
    server_running=true;
  }

  if (mysettings.mqtt_enabled) {
    connectToMqtt();
  }

  if (mysettings.influxdb_enabled) {
    startTimerToInfluxdb();
  }
}


void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial1.println("Disconnected from Wi-Fi.");

  // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  mqttReconnectTimer.detach();
  myTimerSendMqttPacket.detach();
  myTimerSendInfluxdbPacket.detach();

  wifiReconnectTimer.once(2, connectToWifi);

  //DIYBMSServer::StopServer(&server);
  //server_running=false;
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial1.println("Disconnected from MQTT.");

  myTimerSendMqttPacket.detach();

  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}


void sendMqttPacket() {
  if (!mysettings.mqtt_enabled) return;

  Serial1.println("Sending MQTT");

  char buffer[50];
  char value[50];

  for (uint8_t bank = 0; bank < 4; bank++) {
    for (uint8_t i = 0; i < numberOfModules[bank]; i++) {
      sprintf(buffer, "diybms/%d/%d/voltage", bank,i);
      float v=(float)cmi[bank][i].voltagemV/1000.0;
      dtostrf(v,7, 3, value);
      mqttClient.publish(buffer, 0, true, value);

      sprintf(buffer, "diybms/%d/%d/inttemp", bank,i);
      sprintf(value, "%d", cmi[bank][i].internalTemp);
      mqttClient.publish(buffer, 0, true, value);

      if (cmi[bank][i].externalTemp!=-40) {
        sprintf(buffer, "diybms/%d/%d/exttemp", bank,i);
        sprintf(value, "%d", cmi[bank][i].externalTemp);
        mqttClient.publish(buffer, 0, true, value);
      }

      sprintf(buffer, "diybms/%d/%d/bypass", bank,i);
      sprintf(value, "%d", cmi[bank][i].inBypass ? 1:0);
      mqttClient.publish(buffer, 0, true, value);
    }
  }
}

void onMqttConnect(bool sessionPresent) {
  Serial1.println("Connected to MQTT.");
  myTimerSendMqttPacket.attach(30, sendMqttPacket);
}

void LoadConfiguration() {

  if (Settings::ReadConfigFromEEPROM((char*)&mysettings, sizeof(mysettings), EEPROM_SETTINGS_START_ADDRESS)) return;

    Serial1.println("Apply default config");

    mysettings.totalNumberOfBanks=1;
    mysettings.combinationParallel=true;

    //EEPROM settings are invalid so default configuration
    mysettings.mqtt_enabled=false;
    mysettings.mqtt_port=1883;

    //Default to EMONPI default MQTT settings
    strcpy(mysettings.mqtt_server,"192.168.0.26");
    strcpy(mysettings.mqtt_username,"emonpi");
    strcpy(mysettings.mqtt_password,"emonpimqtt2016");

    mysettings.influxdb_enabled=false;
    mysettings.influxdb_httpPort=8086;

    strcpy(mysettings.influxdb_host,"myinfluxserver");
    strcpy(mysettings.influxdb_database,"database");
    strcpy(mysettings.influxdb_user,"user");
    strcpy(mysettings.influxdb_password,"");

    mysettings.timeZone= 0;
    mysettings.minutesTimeZone= 0;
    mysettings.daylight=false;
    strcpy(mysettings.ntpServer,"time.google.com");

    for (size_t x = 0; x < RELAY_TOTAL; x++) {
      mysettings.rulerelaydefault[x]=RELAY_OFF;
    }

    mysettings.rulevalue[0]=0;
    mysettings.rulevalue[1]=4150;
    mysettings.rulevalue[2]=55;
    mysettings.rulevalue[3]=16000;
    mysettings.rulevalue[4]=12000;
    mysettings.rulevalue[5]=60*9; //9am
    mysettings.rulevalue[6]=60*17;  //5pm

    for (size_t i = 0; i < RELAY_RULES; i++) {
      for (size_t x = 0; x < RELAY_TOTAL; x++) {
        mysettings.rulerelaystate[i][x]=RELAY_X;
        mysettings.rulerelaystate[i][x]=RELAY_X;
        mysettings.rulerelaystate[i][x]=RELAY_X;
      }
    }
}

void setup() {
  //Serial is used for communication to modules, Serial1 is for debug output
  pinMode(GREEN_LED, OUTPUT);
  //D3 is used to reset access point WIFI details on boot up
  pinMode(D3,INPUT_PULLUP);
  //D5 is interrupt pin from PCF8574
  pinMode(D5,INPUT_PULLUP);

  GREEN_LED_OFF;

  //We generate a unique number which is used in all following JSON requests
  //we use this as a simple method to avoid cross site scripting attacks
  DIYBMSServer::generateUUID();

  numberOfModules[0]=0;
  numberOfModules[1]=0;
  numberOfModules[2]=0;
  numberOfModules[3]=0;

  //Pre configure the array
  for (size_t i = 0; i < maximum_cell_modules; i++)
  {
    cmi[0][i].voltagemVMax=0;
    cmi[0][i].voltagemVMin=6000;
    cmi[1][i].voltagemVMax=0;
    cmi[1][i].voltagemVMin=6000;
    cmi[2][i].voltagemVMax=0;
    cmi[2][i].voltagemVMin=6000;
    cmi[3][i].voltagemVMax=0;
    cmi[3][i].voltagemVMin=6000;
  }

  Serial.begin(4800, SERIAL_8N1);           // Serial for comms to modules

  //Use alternative GPIO pins of D7/D8
  //D7 = GPIO13 = RECEIVE SERIAL
  //D8 = GPIO15 = TRANSMIT SERIAL
  Serial.swap();

  myPacketSerial.setStream(&Serial);           // start serial for output
  myPacketSerial.setPacketHandler(&onPacketReceived);

  //Debug serial output
  Serial1.begin(115200, SERIAL_8N1);
  Serial1.setDebugOutput(true);

  LoadConfiguration();

  //SDA / SCL
  //I'm sure this should be 4,5 !
  Wire.begin(5,4);
  Wire.setClock(100000L);

  //Make PINs 4-7 INPUTs - the interrupt fires when triggered
  pcf8574.begin();

  //We test to see if the i2c expander is actually fitted
  pcf8574.read8();

  if (pcf8574.lastError()==0) {
    Serial1.println("Found pcf8574");
    pcf8574.write(4, HIGH);
    pcf8574.write(5, HIGH);
    pcf8574.write(6, HIGH);
    pcf8574.write(7, HIGH);

    //Set relay defaults
    for (int8_t y = 0; y<RELAY_TOTAL; y++)
    {
         pcf8574.write(y,mysettings.rulerelaydefault[y]==RELAY_ON ? LOW:HIGH);
    }
    PCF8574Enabled=true;
  } else {
    //Not fitted
    Serial1.println("pcf8574 not fitted");
    PCF8574Enabled=false;
  }

  //internal pullup-resistor on the interrupt line via ESP8266
  pcf8574.resetInterruptPin();
  attachInterrupt(digitalPinToInterrupt(D5), PCFInterrupt, FALLING);

  //Ensure we service the cell modules every 5 seconds
  myTimer.attach(5, timerEnqueueCallback);

  //Process rules every 10 seconds (this prevents the relays from clattering on and off)
  myTimerRelay.attach(10, timerProcessRules);

  //We process the transmit queue every 0.5 seconds (this needs to be lower delay than the queue fills)
  myTransmitTimer.attach(0.5, timerTransmitCallback);

  //This is normally pulled high, D3 is used to reset WIFI details
  uint8_t clearAPSettings=digitalRead(D3);

  //Temporarly force WIFI settings
  //wifi_eeprom_settings xxxx;
  //strcpy(xxxx.wifi_ssid,"XXXXXXXXXXXXXXXXX");
  //strcpy(xxxx.wifi_passphrase,"XXXXXXXXXXXXXX");
  //Settings::WriteConfigToEEPROM((char*)&xxxx, sizeof(xxxx), EEPROM_WIFI_START_ADDRESS);

  if (!DIYBMSSoftAP::LoadConfigFromEEPROM() || clearAPSettings==0) {
      Serial1.print("Clear AP settings");
      Serial1.println(clearAPSettings);
      Serial1.println("Setup Access Point");
      //We are in initial power on mode (factory reset)
      DIYBMSSoftAP::SetupAccessPoint(&server);
  } else {

    //Config NTP
      NTP.onNTPSyncEvent ([](NTPSyncEvent_t event) {
         ntpEvent = event;
         NTPsyncEventTriggered = true;
     });

      Serial1.println("Connecting to WIFI");

    /* Explicitly set the ESP8266 to be a WiFi-client, otherwise by default,
      would try to act as both a client and an access-point */

      wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
      wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

      mqttClient.onConnect(onMqttConnect);
      mqttClient.onDisconnect(onMqttDisconnect);

      if (mysettings.mqtt_enabled) {
        Serial1.println("MQTT Enabled");
        mqttClient.setServer(mysettings.mqtt_server, mysettings.mqtt_port);
        mqttClient.setCredentials(mysettings.mqtt_username,mysettings.mqtt_password);
      }

      connectToWifi();
  }
}

void loop() {

  //static int last = 0;

  // Call update to receive, decode and process incoming packets.
  if (Serial.available()) {
    myPacketSerial.update();
  }


  if (ConfigHasChanged>0) {
      //Auto reboot if needed (after changing MQTT or INFLUX settings)
      //Ideally we wouldn't need to reboot if the code could sort itself out!
      ConfigHasChanged--;
      if (ConfigHasChanged==0) {
        Serial1.println("RESTART AFTER CONFIG CHANGE");
        //Stop networking
        if (mqttClient.connected()) {
          mqttClient.disconnect(true);
        }
        WiFi.disconnect();
        ESP.restart();
      }
      delay(1);
  }

  if (PCFInterruptFlag) {
    //Value is ZERO when PIN is grounded
    Serial1.print("PCFInterruptFlag=");
    Serial1.println(pcf8574.read8() & B11110000);
    PCFInterruptFlag=false;
  }

  if (wifiFirstConnected) {
      Serial1.print("Requesting NTP from ");
      Serial1.println(mysettings.ntpServer);
      wifiFirstConnected = false;
      //Update time every 10 minutes
      NTP.setInterval (600);
      NTP.setNTPTimeout (NTP_TIMEOUT);
      // String ntpServerName, int8_t timeZone, bool daylight, int8_t minutes, AsyncUDP* udp_conn
      NTP.begin (mysettings.ntpServer, mysettings.timeZone, mysettings.daylight, mysettings.minutesTimeZone);
  }

  if (NTPsyncEventTriggered) {
      processSyncEvent (ntpEvent);
      NTPsyncEventTriggered = false;
  }
}
