/************************************
 * Rage
 * Against
 * The
 * Garage
 * Door
 * Opener
 *
 * Copyright (C) 2022  Paul Wieland
 *
 * GNU GENERAL PUBLIC LICENSE
 ************************************/

#include "ratgdo.h"
#include "common.h"

#include "esphome/core/log.h"

namespace esphome {
namespace ratgdo {

static const char *const TAG = "ratgdo";

void RATGDOComponent::setup() {
  pinMode(TRIGGER_OPEN, INPUT_PULLUP);
  pinMode(TRIGGER_CLOSE, INPUT_PULLUP);
  pinMode(TRIGGER_LIGHT, INPUT_PULLUP);
  pinMode(STATUS_DOOR, OUTPUT);
  pinMode(STATUS_OBST, OUTPUT);
  pinMode(INPUT_RPM1,
          INPUT_PULLUP); // set to pullup to add support for reed switches
  pinMode(INPUT_RPM2,
          INPUT_PULLUP); // make sure pin doesn't float when using reed switch
                         // and fire interrupt by mistake
  pinMode(INPUT_OBST, INPUT);

  attachInterrupt(TRIGGER_OPEN, isrDoorOpen, CHANGE);
  attachInterrupt(TRIGGER_CLOSE, isrDoorClose, CHANGE);
  attachInterrupt(TRIGGER_LIGHT, isrLight, CHANGE);
  attachInterrupt(INPUT_OBST, isrObstruction, CHANGE);
  attachInterrupt(INPUT_RPM1, isrRPM1, RISING);
  attachInterrupt(INPUT_RPM2, isrRPM2, RISING);

  LittleFS.begin();

  readCounterFromFlash();

  if (useRollingCodes) {
    // if(rollingCodeCounter == 0) rollingCodeCounter = 1;

    ESP_LOGD(TAG, "Syncing rolling code counter after reboot...");
    sync(); // if rolling codes are being used (rolling code counter > 0), send
            // reboot/sync to the opener on startup
  } else {
    ESP_LOGD(TAG, "Rolling codes are disabled.");
  }
}

void RATGDOComponent::loop() {
  obstructionLoop();
  doorStateLoop();
  dryContactLoop();
}

} // namespace ratgdo
} // namespace esphome

/*************************** DETECTING THE DOOR STATE
 * ***************************/
void doorStateLoop() {
  static bool rotaryEncoderDetected = false;
  static int lastDoorPositionCounter = 0;
  static int lastDirectionChangeCounter = 0;
  static int lastCounterMillis = 0;

  // Handle reed switch
  // This may need to be debounced, but so far in testing I haven't detected any
  // bounces
  if (!rotaryEncoderDetected) {
    if (digitalRead(INPUT_RPM1) == LOW) {
      if (doorState != "reed_closed") {
        ESP_LOGD(TAG, "Reed switch closed");
        doorState = "reed_closed";
        if (isConfigFileOk) {
          bootstrapManager.publish(overallStatusTopic.c_str(), "reed_closed",
                                   true);
          bootstrapManager.publish(doorStatusTopic.c_str(), "reed_closed",
                                   true);
        }
        digitalWrite(STATUS_DOOR, HIGH);
      }
    } else if (doorState != "reed_open") {
      ESP_LOGD(TAG, "Reed switch open");
      doorState = "reed_open";
      if (isConfigFileOk) {
        bootstrapManager.publish(overallStatusTopic.c_str(), "reed_open", true);
        bootstrapManager.publish(doorStatusTopic.c_str(), "reed_open", true);
      }
      digitalWrite(STATUS_DOOR, LOW);
    }
  }
  // end reed switch handling

  // If the previous and the current state of the RPM2 Signal are different,
  // that means there is a rotary encoder detected and the door is moving
  if (doorPositionCounter != lastDoorPositionCounter) {
    rotaryEncoderDetected = true; // this disables the reed switch handler
    lastCounterMillis = millis();

    ESP_LOGD(TAG, "Door Position: %d", doorPositionCounter);
  }

  // Wait 5 pulses before updating to door opening status
  if (doorPositionCounter - lastDirectionChangeCounter > 5) {
    if (doorState != "opening") {
      ESP_LOGD(TAG, "Door Opening...");
      if (isConfigFileOk) {
        bootstrapManager.publish(overallStatusTopic.c_str(), "opening", true);
        bootstrapManager.publish(doorStatusTopic.c_str(), "opening", true);
      }
    }
    lastDirectionChangeCounter = doorPositionCounter;
    doorState = "opening";
  }

  if (lastDirectionChangeCounter - doorPositionCounter > 5) {
    if (doorState != "closing") {
      ESP_LOGD(TAG, "Door Closing...");
      if (isConfigFileOk) {
        bootstrapManager.publish(overallStatusTopic.c_str(), "closing", true);
        bootstrapManager.publish(doorStatusTopic.c_str(), "closing", true);
      }
    }
    lastDirectionChangeCounter = doorPositionCounter;
    doorState = "closing";
  }

  // 250 millis after the last rotary encoder pulse, the door is stopped
  if (millis() - lastCounterMillis > 250) {
    // if the door was closing, and is now stopped, then the door is closed
    if (doorState == "closing") {
      doorState = "closed";
      ESP_LOGD(TAG, "Closed");
      if (isConfigFileOk) {
        bootstrapManager.publish(overallStatusTopic.c_str(), doorState.c_str(),
                                 true);
        bootstrapManager.publish(doorStatusTopic.c_str(), doorState.c_str(),
                                 true);
      }
      digitalWrite(STATUS_DOOR, LOW);
    }

    // if the door was opening, and is now stopped, then the door is open
    if (doorState == "opening") {
      doorState = "open";
      ESP_LOGD(TAG, "Open");
      if (isConfigFileOk) {
        bootstrapManager.publish(overallStatusTopic.c_str(), doorState.c_str(),
                                 true);
        bootstrapManager.publish(doorStatusTopic.c_str(), doorState.c_str(),
                                 true);
      }
      digitalWrite(STATUS_DOOR, HIGH);
    }
  }

  lastDoorPositionCounter = doorPositionCounter;
}

/*************************** DRY CONTACT CONTROL OF LIGHT & DOOR
 * ***************************/
void IRAM_ATTR isrDebounce(const char *type) {
  static unsigned long lastOpenDoorTime = 0;
  static unsigned long lastCloseDoorTime = 0;
  static unsigned long lastToggleLightTime = 0;
  unsigned long currentMillis = millis();

  // Prevent ISR during the first 2 seconds after reboot
  if (currentMillis < 2000)
    return;

  if (strcmp(type, "openDoor") == 0) {
    if (digitalRead(TRIGGER_OPEN) == LOW) {
      // save the time of the falling edge
      lastOpenDoorTime = currentMillis;
    } else if (currentMillis - lastOpenDoorTime > 500 &&
               currentMillis - lastOpenDoorTime < 10000) {
      // now see if the rising edge was between 500ms and 10 seconds after the
      // falling edge
      dryContactDoorOpen = true;
    }
  }

  if (strcmp(type, "closeDoor") == 0) {
    if (digitalRead(TRIGGER_CLOSE) == LOW) {
      // save the time of the falling edge
      lastCloseDoorTime = currentMillis;
    } else if (currentMillis - lastCloseDoorTime > 500 &&
               currentMillis - lastCloseDoorTime < 10000) {
      // now see if the rising edge was between 500ms and 10 seconds after the
      // falling edge
      dryContactDoorClose = true;
    }
  }

  if (strcmp(type, "toggleLight") == 0) {
    if (digitalRead(TRIGGER_LIGHT) == LOW) {
      // save the time of the falling edge
      lastToggleLightTime = currentMillis;
    } else if (currentMillis - lastToggleLightTime > 500 &&
               currentMillis - lastToggleLightTime < 10000) {
      // now see if the rising edge was between 500ms and 10 seconds after the
      // falling edge
      dryContactToggleLight = true;
    }
  }
}

void IRAM_ATTR isrDoorOpen() { isrDebounce("openDoor"); }

void IRAM_ATTR isrDoorClose() { isrDebounce("closeDoor"); }

void IRAM_ATTR isrLight() { isrDebounce("toggleLight"); }

// Fire on RISING edge of RPM1
void IRAM_ATTR isrRPM1() { rpm1Pulsed = true; }

// Fire on RISING edge of RPM2
// When RPM1 HIGH on RPM2 rising edge, door closing:
// RPM1: __|--|___
// RPM2: ___|--|__

// When RPM1 LOW on RPM2 rising edge, door opening:
// RPM1: ___|--|__
// RPM2: __|--|___
void IRAM_ATTR isrRPM2() {
  // The encoder updates faster than the ESP wants to process, so by sampling
  // every 5ms we get a more reliable curve The counter is behind the actual
  // pulse counter, but it doesn't matter since we only need a reliable linear
  // counter to determine the door direction
  static unsigned long lastPulse = 0;
  unsigned long currentMillis = millis();

  if (currentMillis - lastPulse < 5) {
    return;
  }

  // In rare situations, the rotary encoder can be parked so that RPM2
  // continuously fires this ISR. This causes the door counter to change value
  // even though the door isn't moving To solve this, check to see if RPM1
  // pulsed. If not, do nothing. If yes, reset the pulsed flag
  if (rpm1Pulsed) {
    rpm1Pulsed = false;
  } else {
    return;
  }

  lastPulse = millis();

  // If the RPM1 state is different from the RPM2 state, then the door is
  // opening
  if (digitalRead(INPUT_RPM1)) {
    doorPositionCounter--;
  } else {
    doorPositionCounter++;
  }
}

// handle changes to the dry contact state
void dryContactLoop() {
  if (dryContactDoorOpen) {
    ESP_LOGD(TAG, "Dry Contact: open the door");
    dryContactDoorOpen = false;
    openDoor();
  }

  if (dryContactDoorClose) {
    ESP_LOGD(TAG, "Dry Contact: close the door");
    dryContactDoorClose = false;
    closeDoor();
  }

  if (dryContactToggleLight) {
    ESP_LOGD(TAG, "Dry Contact: toggle the light");
    dryContactToggleLight = false;
    toggleLight();
  }
}

/*************************** OBSTRUCTION DETECTION ***************************/
void IRAM_ATTR isrObstruction() {
  if (digitalRead(INPUT_OBST)) {
    lastObstructionHigh = millis();
  } else {
    obstructionLowCount++;
  }
}

void obstructionLoop() {
  long currentMillis = millis();
  static unsigned long lastMillis = 0;

  // the obstruction sensor has 3 states: clear (HIGH with LOW pulse every 7ms),
  // obstructed (HIGH), asleep (LOW) the transitions between awake and asleep
  // are tricky because the voltage drops slowly when falling asleep and is high
  // without pulses when waking up

  // If at least 3 low pulses are counted within 50ms, the door is awake, not
  // obstructed and we don't have to check anything else

  // Every 50ms
  if (currentMillis - lastMillis > 50) {
    // check to see if we got between 3 and 8 low pulses on the line
    if (obstructionLowCount >= 3 && obstructionLowCount <= 8) {
      obstructionCleared();

      // if there have been no pulses the line is steady high or low
    } else if (obstructionLowCount == 0) {
      // if the line is high and the last high pulse was more than 70ms ago,
      // then there is an obstruction present
      if (digitalRead(INPUT_OBST) && currentMillis - lastObstructionHigh > 70) {
        obstructionDetected();
      } else {
        // asleep
      }
    }

    lastMillis = currentMillis;
    obstructionLowCount = 0;
  }
}

void obstructionDetected() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  // Anything less than 100ms is a bounce and is ignored
  if (interruptTime - lastInterruptTime > 250) {
    doorIsObstructed = true;
    digitalWrite(STATUS_OBST, HIGH);

    ESP_LOGD(TAG, "Obstruction Detected");

    if (isConfigFileOk) {
      bootstrapManager.publish(overallStatusTopic.c_str(), "obstructed", true);
      bootstrapManager.publish(obstructionStatusTopic.c_str(), "obstructed",
                               true);
    }
  }
  lastInterruptTime = interruptTime;
}

void obstructionCleared() {
  if (doorIsObstructed) {
    doorIsObstructed = false;
    digitalWrite(STATUS_OBST, LOW);

    ESP_LOGD(TAG, "Obstruction Cleared");

    if (isConfigFileOk) {
      bootstrapManager.publish(overallStatusTopic.c_str(), "clear", true);
      bootstrapManager.publish(obstructionStatusTopic.c_str(), "clear", true);
    }
  }
}

void sendDoorStatus() {
  ESP_LOGD(TAG, "Door state %s", doorState);

  if (isConfigFileOk) {
    bootstrapManager.publish(overallStatusTopic.c_str(), doorState.c_str(),
                             true);
    bootstrapManager.publish(doorStatusTopic.c_str(), doorState.c_str(), true);
  }
}

void sendCurrentCounter() {
  String msg = String(rollingCodeCounter);
  ESP_LOGD(TAG, "Current counter %d", rollingCodeCounter);
  if (isConfigFileOk) {
    bootstrapManager.publish(rollingCodeTopic.c_str(), msg.c_str(), true);
  }
}

/********************************** MANAGE HARDWARE BUTTON
 * *****************************************/
void manageHardwareButton() {}

/************************* DOOR COMMUNICATION *************************/
/*
 * Transmit a message to the door opener over uart1
 * The TX1 pin is controlling a transistor, so the logic is inverted
 * A HIGH state on TX1 will pull the 12v line LOW
 *
 * The opener requires a specific duration low/high pulse before it will accept
 * a message
 */
void transmit(byte *payload, unsigned int length) {
  digitalWrite(OUTPUT_GDO, HIGH); // pull the line high for 1305 micros so the
                                  // door opener responds to the message
  delayMicroseconds(1305);
  digitalWrite(OUTPUT_GDO, LOW); // bring the line low

  delayMicroseconds(1260); // "LOW" pulse duration before the message start
  swSerial.write(payload, length);
}

void sync() {
  if (!useRollingCodes)
    return;

  getRollingCode("reboot1");
  transmit(rollingCode, CODE_LENGTH);
  delay(45);

  getRollingCode("reboot2");
  transmit(rollingCode, CODE_LENGTH);
  delay(45);

  getRollingCode("reboot3");
  transmit(rollingCode, CODE_LENGTH);
  delay(45);

  getRollingCode("reboot4");
  transmit(rollingCode, CODE_LENGTH);
  delay(45);

  getRollingCode("reboot5");
  transmit(rollingCode, CODE_LENGTH);
  delay(45);

  getRollingCode("reboot6");
  transmit(rollingCode, CODE_LENGTH);
  delay(45);

  writeCounterToFlash();
}

void openDoor() {
  if (doorState == "open" || doorState == "opening") {
    ESP_LOGD(TAG, "The door is already %s", doorState);
    return;
  }

  doorState = "opening"; // It takes a couple of pulses to detect
                         // opening/closing. by setting here, we can avoid
                         // bouncing from rapidly repeated commands

  if (useRollingCodes) {
    getRollingCode("door1");
    transmit(rollingCode, CODE_LENGTH);

    delay(40);

    getRollingCode("door2");
    transmit(rollingCode, CODE_LENGTH);

    writeCounterToFlash();
  } else {
    for (int i = 0; i < 4; i++) {
      ESP_LOGD(TAG, "sync_code[%d]", i);

      transmit(SYNC_CODE[i], CODE_LENGTH);
      delay(45);
    }
    ESP_LOGD(TAG, "door_code")
    transmit(DOOR_CODE, CODE_LENGTH);
  }
}

void closeDoor() {
  if (doorState == "closed" || doorState == "closing") {
    ESP_LOGD(TAG, "The door is already %s", doorState);
    return;
  }

  doorState = "closing"; // It takes a couple of pulses to detect
                         // opening/closing. by setting here, we can avoid
                         // bouncing from rapidly repeated commands

  if (useRollingCodes) {
    getRollingCode("door1");
    transmit(rollingCode, CODE_LENGTH);

    delay(40);

    getRollingCode("door2");
    transmit(rollingCode, CODE_LENGTH);

    writeCounterToFlash();
  } else {
    for (int i = 0; i < 4; i++) {
      ESP_LOGD(TAG, "sync_code[%d]", i);

      transmit(SYNC_CODE[i], CODE_LENGTH);
      delay(45);
    }
    ESP_LOGD(TAG, "door_code")
    transmit(DOOR_CODE, CODE_LENGTH);
  }
}

void toggleLight() {
  if (useRollingCodes) {
    getRollingCode("light");
    transmit(rollingCode, CODE_LENGTH);
    writeCounterToFlash();
  } else {
    for (int i = 0; i < 4; i++) {
      ESP_LOGD(TAG, "sync_code[%d]", i);

      transmit(SYNC_CODE[i], CODE_LENGTH);
      delay(45);
    }
    ESP_LOGD(TAG, "light_code")
    transmit(LIGHT_CODE, CODE_LENGTH);
  }
}
