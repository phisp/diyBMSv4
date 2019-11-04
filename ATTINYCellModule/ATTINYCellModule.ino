/*
 ____  ____  _  _  ____  __  __  ___    _  _  __
(  _ \(_  _)( \/ )(  _ \(  \/  )/ __)  ( \/ )/. |
 )(_) )_)(_  \  /  ) _ < )    ( \__ \   \  /(_  _)
(____/(____) (__) (____/(_/\/\_)(___/    \/   (_)

DIYBMS V4.0
CELL MODULE FOR ATTINY841

Tools -> Programmer: Arduino as ISP (ATTinyCore)
Tools -> Burn Bootloader first

Tools Config:
Board: ATtiny441/841 (No bootloader)
Pin Mapping: Clockwise (like Rev. D boards)
Chip: ATtiny841
Clock: 8 Mhz (internal)
LTO: Enabled
B.O.D. Level: B.O.D. Enabled (1.8v)
millis()/micros(): Enabled
B.O.D. Mode (sleep): B.O.D. Disabled
B.O.D. Mode (active): B.O.D. Disabled
Save EEPROM: EEPROM retained
Wire Modes: Master Only
tinyNeo Pixel Port: Port A

*/

//#define DIYBMS_DEBUG

#include <Arduino.h>

#if !(F_CPU == 8000000)
#error Processor speed should be 8 Mhz internal
#endif

//An Arduino Library that facilitates packet-based serial communication using COBS or SLIP encoding.
//https://github.com/bakercp/PacketSerial
#include <PacketSerial.h>
#include <FastPID.h>

//96 byte buffer
PacketSerial_ < COBS, 0, 64 > myPacketSerial;

//Our project code includes
#include "include/defines.h"
#include "lib/crc16/crc16.h"
#include "lib/crc16/crc16.cpp" // Warum wird crc16.cpp nicht automatisch geladen? Muss extra eingebunden werden
#include "lib/settings/settings.h" // --> (crc16.h) crc16.cpp
                                               // --> crc16.h
#include "lib/settings/settings.cpp" // Warum wird settings.cpp nicht automatisch geladen? Muss extra eingebunden werden

#include "include/diybms_attiny841.h"
#include "lib/Steinhart/Steinhart.h"
#include "lib/Steinhart/Steinhart.cpp" // Ich verstehe es einfach nicht, bin zu dumm für .h und Ordner
//#include "packet_processor.cpp" // --> packet_processor.h 
#include "include/packet_processor.h" // --> diybms_attiny841.h Steinhart.h (Steinhart.cpp)       defines.h settings.h (crc16.h)
                                                                                //--> Steinhart.h                                                                      
// Arduino includiert packet_processor.cpp automatisch, falls packet_processor.h includiert wird

//Default values which get overwritten by EEPROM on power up
CellModuleConfig myConfig;

DiyBMSATTiny841 hardware;

PacketProcessor PP( & hardware, & myConfig);

volatile bool wdt_triggered = false;
uint16_t bypassCountDown = 0;
uint8_t bypassHasJustFinished = 0;

void DefaultConfig() {
  myConfig.LoadResistance = 4.40;

  //About 2.2100 seems about right
  myConfig.Calibration = 2.21000;

  //2mV per ADC resolution
  myConfig.mVPerADC = 2.0; //2048.0/1024.0;

  //Stop running bypass if temperature over 70 degrees C
  myConfig.BypassOverTempShutdown = 70;

  myConfig.mybank = 0;

  //Start bypass at 4.1 volt
  myConfig.BypassThresholdmV = 4100;

  //4150 = B constant (25-50℃)
  myConfig.Internal_BCoefficient = 4150;
  //4150 = B constant (25-50℃)
  myConfig.External_BCoefficient = 4150;

  // Resistance @ 25℃ = 47k, B Constant 4150, 0.20mA max current
  //Using https://www.thinksrs.com/downloads/programs/therm%20calc/ntccalibrator/ntccalculator.html
}

void setup(){ 
  //Must be first line of code
  wdt_disable();

  //8 second between watchdogs
  hardware.SetWatchdog8sec();

  //Setup IO ports
  hardware.ConfigurePorts();

  //More power saving changes
  hardware.EnableSerial0();

  #ifdef DIYBMS_DEBUG
  Serial1.begin(38400, SERIAL_8N1);
  DEBUG_PRINT(F("\r\nDEBUG MODE"))
  #else
    hardware.DisableSerial1();
  #endif

  //Check if setup routine needs to be run
  if (!Settings::ReadConfigFromEEPROM((char*)&myConfig, sizeof(myConfig), EEPROM_CONFIG_ADDRESS)) {
    DefaultConfig();
    //Save settings
    Settings::WriteConfigToEEPROM((char*)&myConfig, sizeof(myConfig), EEPROM_CONFIG_ADDRESS);
  }

  hardware.double_tap_green_led();

  //Set up data handler
  Serial.begin(4800, SERIAL_8N1);

  myPacketSerial.setStream(&Serial);
  myPacketSerial.setPacketHandler(&onPacketReceived);

  #ifdef DIYBMS_DEBUG
  if (myPID.err()) {
    Serial1.println("There is a configuration error!");
    for (;;) {}
  }
  #endif
}

ISR(WDT_vect) {
  //This is the watchdog timer - something went wrong and no activity recieved in a while
  wdt_triggered = true;
  PP.IncrementWatchdogCounter();
}

ISR(ADC_vect) {
  // when ADC completed, take an interrupt and process result
  PP.ADCReading(hardware.ReadADC());
}

void onPacketReceived(const uint8_t * receivebuffer, size_t len) {

  if (len > 0) {

    //A data packet has just arrived, process it and forward the results to the next module
    if (PP.onPacketReceived(receivebuffer, len)) {
      //Only light green if packet is good
      hardware.GreenLedOn();
    }

    hardware.EnableSerial0TX();

    //Wake up the connected cell module from sleep
    Serial.write((byte) 0);
    delay(10);

    //Send the packet (even if it was invalid so controller can count crc errors)
    myPacketSerial.send(PP.GetBufferPointer(), PP.GetBufferSize());

    hardware.FlushSerial0();
    hardware.DisableSerial0TX();
  }

  hardware.GreenLedOff();
}


ISR(USART0_START_vect) {
  //Needs to be here!
  //asm("NOP");

  //hardware.GreenLedOn();
}

//Kp: Determines how aggressively the PID reacts to the current amount of error (Proportional)
//Ki: Determines how aggressively the PID reacts to error over time (Integral)
//Kd: Determines how aggressively the PID reacts to the change in error (Derivative)

//3Hz rate - number of times we call this code in Loop
FastPID myPID(150.0, 2.5, 5, 3, 16, false);

bool hztiming=false;

void loop(){
  //This loop runs around 3 times per second when the module is in bypass

  wdt_reset();

  //if (hztiming) {  hardware.SparePinOn();} else {  hardware.SparePinOff();}hztiming=!hztiming;

  if (PP.identifyModule > 0) {
    hardware.GreenLedOn();
    PP.identifyModule--;

    if (PP.identifyModule == 0) {
      hardware.GreenLedOff();
    }
  }

  if (!PP.WeAreInBypass && bypassHasJustFinished == 0) {
    //We don't sleep if we are in bypass mode or just after completing bypass
    hardware.EnableStartFrameDetection();

    //Program stops here until woken by watchdog or pin change interrupt
    hardware.Sleep();
  }

  //We are awake....

  if (wdt_triggered) {
    //Flash green LED twice after a watchdog wake up
    hardware.double_tap_green_led();
  }

  //We always take a voltage and temperature reading on every loop cycle to check if we need to go into bypass
  //this is also triggered by the watchdog should comms fail or the module is running standalone
  //Probably over kill to do it this frequently
  hardware.ReferenceVoltageOn();

  //allow 2.048V to stabalize
  delay(10);

  PP.TakeAnAnalogueReading(ADC_CELL_VOLTAGE);
  //Internal temperature
  PP.TakeAnAnalogueReading(ADC_INTERNAL_TEMP);
  //External temperature
  PP.TakeAnAnalogueReading(ADC_EXTERNAL_TEMP);

  hardware.ReferenceVoltageOff();

  if (PP.BypassCheck()) {
    //Our cell voltage is OVER the setpoint limit, start draining cell using load bypass resistor

    if (!PP.WeAreInBypass) {
      //We have just entered the bypass code

      //The TIMER2 can vary between 0 and 10,000
      myPID.setOutputRange(0, 10000);

      //Start timer2 with zero value
      hardware.StartTimer2();

      PP.WeAreInBypass = true;

      //This controls how many cycles of loop() we make before re-checking the situation
      bypassCountDown = 200;
    }
  }

  if (bypassCountDown > 0) {
    //Compare the real temperature against max setpoint
    //We want the PID to keep at this temperature
    //int setpoint = ;
    //int feedback =
    uint16_t output = myPID.step(myConfig.BypassOverTempShutdown, PP.InternalTemperature());

    hardware.SetTimer2Value(output);

    bypassCountDown--;

    if (bypassCountDown == 0) {
      //Switch everything off for this cycle

      PP.WeAreInBypass = false;

      //myPID.clear();
      hardware.StopTimer2();

      //switch off
      hardware.DumpLoadOff();

      //On the next iteration of loop, don't sleep so we are forced to take another
      //cell voltage reading without the bypass being enabled, and we can then
      //evaludate if we need to stay in bypass mode, we do this a few times
      //as the cell has a tendancy to float back up in voltage once load resistor is removed
      bypassHasJustFinished = 200;
    }
  }

  if (wdt_triggered) {
    //We got here because the watchdog (after 8 seconds) went off - we didnt receive a packet of data
    wdt_triggered = false;
  } else {
    //Loop here processing any packets then go back to sleep

    //NOTE this loop size is dependant on the size of the packet buffer (34 bytes)
    //     too small a loop will prevent anything being processed as we go back to Sleep
    //     before packet is received correctly
    for (size_t i = 0; i < 15; i++) {
      //Allow data to be received in buffer
      delay(10);

      // Call update to receive, decode and process incoming packets.
      myPacketSerial.update();
    }
  }

  if (bypassHasJustFinished > 0) {
    bypassHasJustFinished--;
  }
}
