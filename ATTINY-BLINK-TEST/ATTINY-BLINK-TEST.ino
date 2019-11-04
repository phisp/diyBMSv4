/*
 ____  ____  _  _  ____  __  __  ___    _  _  __
(  _ \(_  _)( \/ )(  _ \(  \/  )/ __)  ( \/ )/. |
 )(_) )_)(_  \  /  ) _ < )    ( \__ \   \  /(_  _)
(____/(____) (__) (____/(_/\/\_)(___/    \/   (_)

  based on Stuard Pittaway BMSv4 design
  
  (c) 2019 Philipp Forsbach

  ATTINY841 LED Blink TEST

  ATTINY841-SSU data sheet
  https://www.mouser.de/datasheet/2/268/Atmel-8495-8-bit-AVR-Microcontrollers-ATtiny441-AT-1315526.pdf

    PA1 = PIN 12 SERIAL TRANSMIT (TXD0)
    PA2 = PIN 11 SERIAL RECEIVE (RXD0)
  
    PA3 = DUMP LOAD ENABLE / PIN 10 /  ARDUINO PIN 7/A3
    PA4 = ADC4 PIN 9 ARDUINO PIN 6/A4 = ON BOARD TEMP sensor
    PA5 = RED_LED / PIN 8 / ARDUINO PIN 5/A5  (SERIAL PORT 1 TXD1)
    PA6 = GREEN_LED / PIN 7 / ARDUINO PIN 4/A6
    PA7 = ADC7 = PIN 6 = ARDUINO PIN 3/A7 = 2.048V REFERENCE ENABLE
  
    PB2 = ADC8 PIN 5 ARDUINO PIN 2/A8 = VOLTAGE reading
    PB0 = ADC11 PIN 2 ARDUINO PIN 0/A11 = REMOTE TEMP sensor
    PB1 = ADC10 PIN 3 ARDUINO PIN 1/A10 = SPARE INPUT/OUTPUT
*/


void setup() {
  Serial.begin(9600);
  pinMode(PA3, OUTPUT);
  pinMode(PA5, OUTPUT);
  pinMode(PA6, OUTPUT);
  pinMode(PA7, OUTPUT);
  
  digitalWrite(PA7, HIGH);
}

void loop() {
  Serial.println(PA6);
  // put your main code here, to run repeatedly:
  GreenLedOn();
  delay(50);
  GreenLedOff();
  delay(50);
  BlueLedOn();
  delay(50);
  BlueLedOff();
  delay(50);
  //RedLedOn();
  //delay(50);
  //RedLedOff();
  //delay(50);
}

void RedLedOn() {
  digitalWrite(PA3, HIGH);
}

void RedLedOff() {
  digitalWrite(PA3, LOW);
}

void BlueLedOn() {
  digitalWrite(PA5, HIGH);
}

void BlueLedOff() {
  digitalWrite(PA5, LOW);
}

void GreenLedOn() {
  //#define GREEN_LED_ON PORTA |= _BV(PORTA6);
  digitalWrite(PA6, HIGH);
}

void GreenLedOff() {
  //#define GREEN_LED_OFF PORTA &= (~_BV(PORTA6));
  digitalWrite(PA6, LOW);
}
