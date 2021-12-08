/*
  This module and those attached with it have been modified for the Helium Network by Fizzy. The following has been changed from the original modifications for Helium, by longfi-arduino:
  - Added Helium Startup Logo
  - Changed App Name and Version of device to reflect more of a device name and number scheme.
  - Enabled long press middle button to Discard Prefs by default for future troubleshooting on device.
  - Changed Text output to reflect Helium, and not TTL (Code referances ttn, just to prevent brakes in this awesome code)
  - Changed credentials file to use OTAA by default.
  - Changed GPS metric output text "Error", to "Accuracy/HDOP".
*/
/*
  Modifications by Max-Plastix
  - DevEUI is auto-generated per-device
  - Print all three ID values to console at boot, in a format that pastes into Helium Console. 
  - Change Payload to #2 and match CubeCell Mapper format, including battery voltage. (Shared decoder with CubeCell)
*/
/*

  Main module

  # Modified by Kyle T. Gabriel to fix issue with incorrect GPS data for TTNMapper

  Copyright (C) 2018 by Xose Pérez <xose dot perez at gmail dot com>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <Arduino.h>
#include "configuration.h"
#include <TinyGPS++.h>
#include <Wire.h>
#include <axp20x.h>

// Defined in ttn.ino
void ttn_register(void (*callback)(uint8_t message));

bool          justSendNow             = true; // Start by sending
unsigned long int  last_send_millis   = 0;
unsigned long int  last_moved_millis  = 0;
float         last_send_lat           = 0;
float         last_send_lon           = 0;
float         dist_moved              = 0;

/* Defaults that can be overwritten by downlink messages */
unsigned long int tx_interval_ms = STATIONARY_TX_INTERVAL * 1000;
boolean freeze_tx_interval_ms = false;
float battery_low_voltage = BATTERY_LOW_VOLTAGE;
float min_dist_moved = MIN_DIST;

AXP20X_Class axp;
bool pmu_irq = false;
const char *baChStatus = "unknown";

bool ssd1306_found = false;
bool axp192_found = false;

bool packetSent, packetQueued;
bool isJoined = false;

#if defined(PAYLOAD_USE_FULL)
// includes number of satellites and accuracy
static uint8_t txBuffer[10];
#elif defined(PAYLOAD_USE_CAYENNE)
// CAYENNE DF
static uint8_t txBuffer[11] = {0x03, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#elif defined (PAYLOAD_USE_MAPPER)
  static uint8_t txBuffer[11];
#endif

// deep sleep support
RTC_DATA_ATTR int bootCount = 0;
esp_sleep_source_t wakeCause; // the reason we booted this time

char buffer[40]; // Screen buffer

// -----------------------------------------------------------------------------
// Application
// -----------------------------------------------------------------------------

void buildPacket(uint8_t txBuffer[]); // needed for platformio

/**
   If we have a valid position send it to the server.
   @return true if we decided to send.
*/

bool trySend()
{
  if (gps_hdop() <= 0 || gps_hdop() > 50 
      || gps_latitude() == 0.0                 // Not fair to the whole equator
      || gps_latitude() > 90 || gps_latitude() < -90 
      || gps_longitude() == 0.0    // Not fair to King George
      || gps_longitude() < -180 || gps_longitude() > 180 
      || gps_altitude() == 0.0 // Not fair to the ocean
  )
    return false; // Rejected as bogus GPS reading.

  if (!isJoined)
    return false;

  // distance from last transmitted location
  float dist_moved = gps_distanceBetween(last_send_lat, last_send_lon, gps_latitude(), gps_longitude());
  #if 0
  snprintf(buffer, sizeof(buffer), "Lat: %10.6f\n", gps_latitude());
  screen_print(buffer);
  snprintf(buffer, sizeof(buffer), "Long: %10.6f\n", gps_longitude());
  screen_print(buffer);
  snprintf(buffer, sizeof(buffer), "HDOP: %4.2fm\n", gps_hdop());
  screen_print(buffer);
  #endif

  // check if we should transmit based on distance covered since last TX or are there other reasons:
  // - TX when distance traveled above required threshold
  // - TX when we are not tracking distance yet - happens on first trySend() after boot-up/reset - default is distance to (0,0) so more than usual 50m
  // - TX when stationary (not met movement requirement) but waited enough TX cycles (report I'm still alive)
  // - TX when USR button short presset to force send right now
  // in all other cases sleep and return false

  boolean send_now;
  // rudimentary debug to serial port
  if (justSendNow)
  {
    justSendNow = false;
    Serial.println("** JUST_SEND_NOW");
    send_now = true;
  }
  else if (dist_moved > min_dist_moved)
  {
    Serial.println("** MOVING");
    last_moved_millis = millis();
    send_now = true;
  }
  else if (millis() - last_send_millis > tx_interval_ms)
  {
    Serial.println("** STATIONARY_TX");
//    Serial.printf("last = %lu, interval = %lu\n", last_send_millis, tx_interval_ms);
    send_now = true;
  }
  else
  {
    send_now = false;
  }

  if (send_now && LMIC_queryTxReady())
  {
    //snprintf(buffer, sizeof(buffer), "Moved %4.1fm\n", dist_moved);
    // The first distance is crazy.. don't put it on screen.
    if (dist_moved < 1000000) {
      snprintf(buffer, sizeof(buffer), "%lus %.0fm ", (millis()-last_send_millis)/1000, dist_moved);
      screen_print(buffer);
    }

    // prepare LoRa frame
    buildPacket(txBuffer);

    bool confirmed = (LORAWAN_CONFIRMED_EVERY > 0) && (ttn_get_count() % LORAWAN_CONFIRMED_EVERY == 0);
    if (confirmed)
      Serial.println("ACK requested");

    // send it!
    packetQueued = true;
    ttn_send(txBuffer, sizeof(txBuffer), LORAWAN_PORT, confirmed);
    packetSent = true;
    last_send_millis = millis();
    last_send_lat = gps_latitude();
    last_send_lon = gps_longitude();
    return true;
  } else {
    // snprintf(buffer, sizeof(buffer), "Still: %4.1fm\n", dist_moved);
    // screen_print(buffer);
    return false;
  }
}

void doDeepSleep(uint64_t msecToWake)
{
  Serial.printf("Entering deep sleep for %llu seconds\n", msecToWake / 1000);

  // not using wifi yet, but once we are this is needed to shutoff the radio hw
  // esp_wifi_stop();

  screen_off(); // datasheet says this will draw only 10ua
  LMIC_shutdown(); // cleanly shutdown the radio

  if (axp192_found) {
    // turn on after initial testing with real hardware
    axp.setPowerOutPut(AXP192_LDO2, AXP202_OFF); // LORA radio
    axp.setPowerOutPut(AXP192_LDO3, AXP202_OFF); // GPS main power
  }

  // FIXME - use an external 10k pulldown so we can leave the RTC peripherals powered off
  // until then we need the following lines
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // Only GPIOs which are have RTC functionality can be used in this bit map: 0,2,4,12-15,25-27,32-39.
  uint64_t gpioMask = (1ULL << MIDDLE_BUTTON_PIN);

  // FIXME change polarity so we can wake on ANY_HIGH instead - that would allow us to use all three buttons (instead of just the first)
  gpio_pullup_en((gpio_num_t) MIDDLE_BUTTON_PIN);

  esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ALL_LOW);

  esp_sleep_enable_timer_wakeup(msecToWake * 1000ULL); // call expects usecs
  esp_deep_sleep_start();                              // TBD mA sleep current (battery)
}


void update_status() {
  float_t batt_volts = axp.getBattVoltage() / 1000.0;
  float_t charge_ma = axp.getBattChargeCurrent();
  float_t discharge_ma = axp.getBattDischargeCurrent();
  // static boolean screen_sleep = false;

  if (0)
  {
    snprintf(buffer, sizeof(buffer), "%.2fv %.1fmA\n", batt_volts, charge_ma - discharge_ma);
    Serial.println(buffer);
  }

#if 0
  if ((batt_volts > SLEEP_VOLTAGE) || (charge_ma - discharge_ma > 1.0)) {
    if (screen_sleep) {
      screen_sleep = false;
      screen_on();
    }
  } else {
    if (!screen_sleep) {
      screen_sleep = true;
      screen_off();
    }
  }
#endif

#if 0  
  // Set the user button to wake the board
  sleep_interrupt(MIDDLE_BUTTON_PIN, LOW);

  doDeepSleep(SEND_INTERVAL);
#endif
}


void callback(uint8_t message) 
{
  static boolean seen_joined = false, seen_joining = false;
#ifdef DEBUG_LORA_MESSAGES
  {
    snprintf(buffer, sizeof(buffer), "## MSG %d\n", message);
    screen_print(buffer);
  }
  if (EV_JOIN_TXCOMPLETE == message) Serial.println("# JOIN_TXCOMPLETE");
  if (EV_TXCOMPLETE == message) Serial.println("# TXCOMPLETE");
  if (EV_RXCOMPLETE == message) Serial.println("# RXCOMPLETE");
  if (EV_RXSTART == message) Serial.println("# RXSTART");
  if (EV_TXCANCELED == message) Serial.println("# TXCANCELED");
  if (EV_TXSTART == message) Serial.println("# TXSTART");
  if (EV_JOINING == message) Serial.println("# JOINING");
  if (EV_JOINED == message) Serial.println("# JOINED");
  if (EV_JOIN_FAILED == message) Serial.println("# JOIN_FAILED");
  if (EV_REJOIN_FAILED == message) Serial.println("# REJOIN_FAILED");
  if (EV_RESET == message) Serial.println("# RESET");
  if (EV_LINK_DEAD == message) Serial.println("# LINK_DEAD");
  if (EV_ACK == message) Serial.println("# ACK");
  if (EV_PENDING == message) Serial.println("# PENDING");
  if (EV_QUEUED == message) Serial.println("# QUEUED");
#endif

  /* This is confusing because JOINED is sometimes spoofed and comes early */
  if (EV_JOINED == message)
    seen_joined = true;
  if (EV_JOINING == message)
    seen_joining = true;
  if (!isJoined && seen_joined && seen_joining)
  {
    isJoined = true;
    screen_print("Joined Helium!\n");
  }

  if (EV_TXSTART == message) {
    screen_print("TX\n");
  }
  // We only want to say 'packetSent' for our packets (not packets needed for joining)
  if (EV_TXCOMPLETE == message && packetQueued) {
//    screen_print("sent.\n");
    packetQueued = false;
    packetSent = true;
  }

  if (EV_RXCOMPLETE == message || EV_RESPONSE == message) {

    size_t len = ttn_response_len();
    uint8_t data[len];
    uint8_t port;
    ttn_response(&port, data, len);

    snprintf(buffer, sizeof(buffer), "Rx: %d on P%d\n", len, port);
    screen_print(buffer);

    Serial.print("Downlink on port:");
    printHex2(port);
    Serial.print(" = ");
    for (int i = 0; i < len; i++)
        printHex2(data[i]);
    Serial.println();

    /*
     * Downlink format: FPort 1
     * 2 Bytes: Minimum Distance (1 to 65535) meters, or 0 no-change
     * 2 Bytes: Minimum Time (1 to 65535) seconds (18.2 hours) between pings, or 0 no-change, or 0xFFFF to use default
     * 1 Byte:  Battery voltage (2.0 to 4.5) for auto-shutoff, or 0 no-change
     */ 
    if (port == 1 && len == 5) {
      float new_distance = (float)(data[0] << 8 | data[1]);
      if (new_distance > 0.0) {
        min_dist_moved = new_distance;
        snprintf(buffer, sizeof(buffer), "New Dist: %.0fm\n", new_distance);
        screen_print(buffer);
      }

      unsigned long int new_interval = data[2] << 8 | data[3];
      if (new_interval) {
        if (new_interval == 0xFFFF) {
          freeze_tx_interval_ms = false;
          tx_interval_ms = STATIONARY_TX_INTERVAL;
        } else {
          tx_interval_ms = new_interval * 1000;
          freeze_tx_interval_ms = true;
        }
        snprintf(buffer, sizeof(buffer), "New Time: %.0lus\n", new_interval);
        screen_print(buffer);
      }

      if (data[4]) {
        float new_low_voltage = data[4] / 100.00 + 2.00;
        battery_low_voltage = new_low_voltage;
        snprintf(buffer, sizeof(buffer), "New LowBat: %.2fv\n", new_low_voltage);
        screen_print(buffer);
      }
    }
  }
}

void scanI2Cdevice(void)
{
  byte err, addr;
  int nDevices = 0;
  for (addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("I2C device found at address 0x");
      if (addr < 16)
        Serial.print("0");
      Serial.print(addr, HEX);
      Serial.println(" !");
      nDevices++;

      if (addr == SSD1306_ADDRESS) {
        ssd1306_found = true;
        Serial.println("ssd1306 display found");
      }
      if (addr == AXP192_SLAVE_ADDRESS) {
        axp192_found = true;
        Serial.println("axp192 PMU found");
      }
    } else if (err == 4) {
      Serial.print("Unknow error at address 0x");
      if (addr < 16)
        Serial.print("0");
      Serial.println(addr, HEX);
    }
  }
  if (nDevices == 0)
    Serial.println("No I2C devices found\n");
  else
    Serial.println("done\n");
}

/**
   Init the power manager chip

   axp192 power
    DCDC1 0.7-3.5V @ 1200mA max -> OLED // If you turn this off you'll lose comms to the axp192 because the OLED and the axp192 share the same i2c bus, instead use ssd1306 sleep mode
    DCDC2 -> unused
    DCDC3 0.7-3.5V @ 700mA max -> ESP32 (keep this on!)
    LDO1 30mA -> charges GPS backup battery // charges the tiny J13 battery by the GPS to power the GPS ram (for a couple of days), can not be turned off
    LDO2 200mA -> LORA
    LDO3 200mA -> GPS
*/

void axp192Init() {
  if (axp192_found) {
    if (!axp.begin(Wire, AXP192_SLAVE_ADDRESS)) {
      Serial.println("AXP192 Begin PASS");
    } else {
      Serial.println("AXP192 Begin FAIL");
    }
    // axp.setChgLEDMode(LED_BLINK_4HZ);
  #if 0
    Serial.printf("DCDC1: %s\n", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("DCDC2: %s\n", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("LDO2: %s\n", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("LDO3: %s\n", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("DCDC3: %s\n", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("Exten: %s\n", axp.isExtenEnable() ? "ENABLE" : "DISABLE");
    Serial.println("----------------------------------------");
#endif

    axp.setPowerOutPut(AXP192_LDO2, AXP202_ON); // LORA radio
    axp.setPowerOutPut(AXP192_LDO3, AXP202_ON); // GPS main power
    axp.setPowerOutPut(AXP192_DCDC2, AXP202_OFF);
    axp.setPowerOutPut(AXP192_EXTEN, AXP202_OFF);
    axp.setPowerOutPut(AXP192_DCDC1, AXP202_ON); // OLED & AXP192 power
    axp.setDCDC1Voltage(3300); // for the OLED power

    Serial.printf("DCDC1: %s\n", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("DCDC2: %s\n", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("DCDC3: %s\n", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
    //Serial.printf("LDO1: %s\n", axp.isLDO1Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("LDO2: %s\n", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("LDO3: %s\n", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
    Serial.printf("Exten: %s\n", axp.isExtenEnable() ? "ENABLE" : "DISABLE");

    pinMode(PMU_IRQ, INPUT_PULLUP);
    attachInterrupt(PMU_IRQ, [] {
      pmu_irq = true;
    }, FALLING);

    axp.adc1Enable(AXP202_BATT_CUR_ADC1, 1);
    axp.enableIRQ(AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_BATT_REMOVED_IRQ | AXP202_BATT_CONNECT_IRQ, 1);
    axp.clearIRQ();

    if (axp.isChargeing()) {
      baChStatus = "Charging: Yes";
    } else {
      baChStatus = "Charging: No";
    }
  } else {
    Serial.println("AXP192 not found");
  }
}


// Perform power on init that we do on each wake from deep sleep
void initDeepSleep() {
  bootCount++;
  wakeCause = esp_sleep_get_wakeup_cause();
  /*
    Not using yet because we are using wake on all buttons being low

    wakeButtons = esp_sleep_get_ext1_wakeup_status();       // If one of these buttons is set it was the reason we woke
    if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 && !wakeButtons) // we must have been using the 'all buttons rule for waking' to support busted boards, assume button one was pressed
      wakeButtons = ((uint64_t)1) << buttons.gpios[0];
  */

  Serial.printf("booted, wake cause %d (boot count %d)\n", wakeCause, bootCount);
}


void setup() {
  // Debug
#ifdef DEBUG_PORT
  DEBUG_PORT.begin(SERIAL_BAUD);
#endif

  initDeepSleep();

  Wire.begin(I2C_SDA, I2C_SCL);
  scanI2Cdevice();

  axp192Init();

  // Buttons & LED
  pinMode(MIDDLE_BUTTON_PIN, INPUT_PULLUP);

#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
#endif

  // Hello
  DEBUG_MSG(APP_NAME " " APP_VERSION "\n");

  // Don't init display if we don't have one or we are waking headless due to a timer event
  if (0 && wakeCause == ESP_SLEEP_WAKEUP_TIMER)
    ssd1306_found = false; // forget we even have the hardware

  if (ssd1306_found) {
    screen_setup();
  }

  // Init GPS
  gps_setup();

  // Show logo on first boot after removing battery
#ifndef ALWAYS_SHOW_LOGO
  if (bootCount == 0) {
#endif
    screen_print(APP_NAME " " APP_VERSION, 0, 0);
    screen_show_logo();
    screen_update();
    delay(LOGO_DELAY);
#ifndef ALWAYS_SHOW_LOGO
  }
#endif

  // Helium setup
  if (!ttn_setup()) {
    screen_print("[ERR] Radio module not found!\n");

    if (REQUIRE_RADIO) {
//      delay(MESSAGE_TO_SLEEP_DELAY);
      screen_off();
      sleep_forever();
    }
  }
  else {
    ttn_register(callback);
    // ttn_erase_prefs();
    ttn_join();
    ttn_adr(LORAWAN_ADR);
  }
}

void update_activity()
{
  float bat_volts = axp.getBattVoltage() / 1000;
  float charge_ma = axp.getBattChargeCurrent();
  // float discharge_ma = axp.getBatChargeCurrent();

  if (axp.isBatteryConnect() && bat_volts < battery_low_voltage && charge_ma < 99.0)
  {
    Serial.println("Low Battery OFF");
    screen_print("Low Battery OFF\n");
    ttn_write_prefs();
    delay(4999); // Give some time to read the screen
    axp.shutdown(); // PMIC power off
    // Does not return
  }

/*
  if (bat_volts > BATTERY_HI_VOLTAGE)
    tx_interval_ms = STATIONARY_TX_INTERVAL * 1000;
  else 
  */
  if (!freeze_tx_interval_ms) 
  {
    unsigned long int now_interval;
    if (millis() - last_moved_millis > REST_WAIT * 1000) 
      now_interval = REST_TX_INTERVAL * 1000;
    else
      now_interval = STATIONARY_TX_INTERVAL * 1000;
    if (now_interval != tx_interval_ms)
      tx_interval_ms = now_interval;
  }
}

/* I must know what that interrupt was for! */
const char *find_irq_name (void)
{
  const char *irq_name = "MysteryIRQ";

  if (axp.isAcinOverVoltageIRQ()) irq_name = "AcinOverVoltage";
  else if (axp.isAcinPlugInIRQ()) irq_name = "AcinPlugIn";
  else if (axp.isAcinRemoveIRQ()) irq_name = "AcinRemove";
  else if (axp.isVbusOverVoltageIRQ()) irq_name = "VbusOverVoltage";
  else if (axp.isVbusPlugInIRQ()) irq_name = "VbusPlugIn";
  else if (axp.isVbusRemoveIRQ()) irq_name = "VbusRemove";
  else if (axp.isVbusLowVHOLDIRQ()) irq_name = "VbusLowVHOLD";
  else if (axp.isBattPlugInIRQ()) irq_name = "BattPlugIn";
  else if (axp.isBattRemoveIRQ()) irq_name = "BattRemove";
  else if (axp.isBattEnterActivateIRQ()) irq_name = "BattEnterActivate";
  else if (axp.isBattExitActivateIRQ()) irq_name = "BattExitActivate";
  else if (axp.isChargingIRQ()) irq_name = "Charging";
  else if (axp.isChargingDoneIRQ()) irq_name = "ChargingDone";
  else if (axp.isBattTempLowIRQ()) irq_name = "BattTempLow";
  else if (axp.isBattTempHighIRQ()) irq_name = "BattTempHigh";
  else if (axp.isChipOvertemperatureIRQ()) irq_name = "ChipOvertemperature";
  else if (axp.isChargingCurrentLessIRQ()) irq_name = "ChargingCurrentLess";
  else if (axp.isDC2VoltageLessIRQ()) irq_name = "DC2VoltageLess";
  else if (axp.isDC3VoltageLessIRQ()) irq_name = "DC3VoltageLess";
  else if (axp.isLDO3VoltageLessIRQ()) irq_name = "LDO3VoltageLess";
  else if (axp.isPEKShortPressIRQ()) irq_name = "PEKShortPress";
  else if (axp.isPEKLongtPressIRQ()) irq_name = "PEKLongtPress";
  else if (axp.isNOEPowerOnIRQ()) irq_name = "NOEPowerOn";
  else if (axp.isNOEPowerDownIRQ()) irq_name = "NOEPowerDown";
  else if (axp.isVBUSEffectiveIRQ()) irq_name = "VBUSEffective";
  else if (axp.isVBUSInvalidIRQ()) irq_name = "VBUSInvalid";
  else if (axp.isVUBSSessionIRQ()) irq_name = "VUBSSession";
  else if (axp.isVUBSSessionEndIRQ()) irq_name = "VUBSSessionEnd";
  else if (axp.isLowVoltageLevel1IRQ()) irq_name = "LowVoltageLevel1";
  else if (axp.isLowVoltageLevel2IRQ()) irq_name = "LowVoltageLevel2";
  else if (axp.isTimerTimeoutIRQ()) irq_name = "TimerTimeout";
  else if (axp.isPEKRisingEdgeIRQ()) irq_name = "PEKRisingEdge";
  else if (axp.isPEKFallingEdgeIRQ()) irq_name = "PEKFallingEdge";
  else if (axp.isGPIO3InputEdgeTriggerIRQ()) irq_name = "GPIO3InputEdgeTrigger";
  else if (axp.isGPIO2InputEdgeTriggerIRQ()) irq_name = "GPIO2InputEdgeTrigger";
  else if (axp.isGPIO1InputEdgeTriggerIRQ()) irq_name = "GPIO1InputEdgeTrigger";
  else if (axp.isGPIO0InputEdgeTriggerIRQ()) irq_name = "GPIO0InputEdgeTrigger";

  return irq_name;
}

void loop() {
  gps_loop();
  ttn_loop();
  screen_loop();
  update_activity();

  if (packetSent) {
    packetSent = false;
  }

  // Short press on power button (near USB) also causes PMIC IRQ
  if (axp192_found && pmu_irq) {
    const char *irq_name;
    pmu_irq = false;
    axp.readIRQ();
    irq_name = find_irq_name();
    axp.clearIRQ();

    snprintf(buffer, sizeof(buffer), "%s\n", irq_name);
    screen_print(buffer);
  }

  static uint32_t pressTime = 0; // what tick should we call this press long enough
  if (!digitalRead(MIDDLE_BUTTON_PIN)) {
    // Pressure is on
    if (!pressTime) { // just started a new press
      pressTime = millis();
    }
  } else if (pressTime) {
    // we just did a release
    if (millis() - pressTime > 1000) {
      // held long enough
      Serial.println("Long press!");

      screen_print("Discarding prefs!\n");
      ttn_erase_prefs();
      delay(5000); // Give some time to read the screen
      ESP.restart();
    } else {
      // short press, send beacon
      Serial.println("Short press.");
      justSendNow = true;
      trySend();
    }
    pressTime = 0;  // Released
  }

  if (trySend()) {
      // Good send
  } else {
      // Nothing sent. 
      // Do NOT delay() here.. the LoRa receiver and join housekeeping also needs to run!
  }
}