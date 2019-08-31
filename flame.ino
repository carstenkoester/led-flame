/*
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <WiFiDMX.h>

// Required for OTA
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>


/**
 ** This application implements a basic PWM dimmer for an RGBW strip
 ** (or, of course, an RGBA strip or any other 4-channel LED device).
 **
 ** It consumes FIVE DMX channels:
 **  - First channel is an overall intensity
 **  - Second through 5th channel are the values for Red, Green, Blue
 **    and White (in that order).
 **
 ** The 5-channel method seems to work well with lighting consoles
 ** and software that treat RGB fixtures as intelligent fixtures
 ** which have a single intensity value, and colors defined as
 ** attributes.
 **/

/*
 * User-level constants. These (most likely) need to be modified each time
 * this application is used in a new location
 */
// WLAN SSID and password. These are included from a separate file.
#include "wlan_credentials.h"

// DMX universe and address
const int DMX_UNIVERSE = 3;
const int DMX_ADDRESS = 2;

// Whether or not to enable packet debug. Use for testing only.
const boolean ALL_PACKET_DEBUG = false;  // Debug print ALL packets
const boolean NEW_PACKET_DEBUG = false;  // Debug print on new packets (changed values) only


/*
 * Internal constants. These DO NOT need to be changed unless the hardware
 * layout is changed.
 */
const String VERSION = "0.5.0";   // Application version

// GPIO pins that are used. Set these to match your board
const int NUM_ROWS = 6;
const int ROW_GPIO[NUM_ROWS] = {27, 33, 15, 32, 14, 26};

const int NUM_COLUMNS = 3;
const int COLUMN_GPIO[NUM_COLUMNS] = {25, 4, 21};

const int TILT_GPIO = 13;


// PWM properties
const int PWM_FREQ = 4000;
const int PWM_RESOLUTION = 8;

const int LED_PWM_MAX = 255;


// Misc properties
const int DMX_TIMEOUT = 5;      // in seconds. Turn off if not received DMX for this amount of time.
const bool TEST_MODE = false;


/*
 * Following parameters influence the visual appearance of the flame.
 */

/*
 * FLAME_PROBABLILITY defines a probability, in percent, that a row is "on"
 * during a any given cycle.
 */
const int FLAME_PROBABILITY[NUM_ROWS] = {90, 80, 50, 20, 10, 5};

/*
 * We design a "flame" by creating a 3x6 matrix of output levels.
 * Currently, each dot in that matrix is either on or off, according to the
 * FLAME_PROBABILITY of that row. (The bottom dots are almost always on;
 * the top dots rarely).
 * 
 * Once we generated a "flame", we'll fade from the current output to the new
 * intended output. We'll do that in FADE_STEPS steps, and each step will
 * remain for FADE_DWELL mlliseconds.
 */
const int FADE_STEPS = 16;
const int FADE_DWELL = 3;          // milliseconds


/*
 * Internal variables used for the matrix above.
 */
int currentMatrix[NUM_COLUMNS][NUM_ROWS] = {
  {0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0},
};
int nextMatrix[NUM_COLUMNS][NUM_ROWS] = {
  {0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0},
};
int deltaMatrix[NUM_COLUMNS][NUM_ROWS] = {
  {0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0},
};


// Intensity value received via DMX
unsigned char dmx_master_intensity;


/*
 * Arduino setup and loop routines
 */
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n\nFlame LED control program -- Version %s\n", VERSION);
  Serial.printf("Listening on DMX Universe %d, address %d\n", DMX_UNIVERSE, DMX_ADDRESS);


  /*
   * GPIO setup
   */ 
  // Set the Column pins as output ping
  for (int column = 0; column < NUM_COLUMNS; column++) {
    pinMode(COLUMN_GPIO[column], OUTPUT);
    digitalWrite(COLUMN_GPIO[column], LOW);
  }

  // We use PWM for the rows. Note that we need to invert the GPIO pin for this board.
  for (int ledChannel = 0; ledChannel < NUM_ROWS; ledChannel++) {
    ledcSetup(ledChannel, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(ROW_GPIO[ledChannel], ledChannel);

    GPIO.func_out_sel_cfg[ROW_GPIO[ledChannel]].inv_sel = 1;
    ledcWrite(ledChannel, 0);
  }

  // Set the tilt switch as input
  pinMode(TILT_GPIO, INPUT_PULLUP);


  /*
   * One LED up while we wait for WiFi
   */
  digitalWrite(COLUMN_GPIO[0], HIGH);
  ledcWrite(0, 50);
  
  /*
   * Network setup
   */
  //  Initialize the WiFi_DMX routines with the Universe of interest
  WifiDMX::setup_with_callback(sizeof(WLAN_CREDENTIALS)/sizeof(WLAN_CREDENTIALS[0]), WLAN_CREDENTIALS, DMX_UNIVERSE, pwmDimmerUpdateFunction, ALL_PACKET_DEBUG);

  // OTA upgrade preparation
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
} // setup()


void loop()
{
  ArduinoOTA.handle();

  if (TEST_MODE) {
    testLoop();
  } else {
    flameLoop();
  }
}


/*
 * Subroutines
 */
void pwmDimmerUpdateFunction(WifiDMX::dmxBuffer dmxBuffer)
{
  dmx_master_intensity = dmxBuffer[DMX_ADDRESS];
} // pwmDimmerUpdateFunction()


/*
 * Write to an output row. Takes care of applying the DMX "master" value
 * and also takes care of inverting the rows based on the tilt switch.
 */
void rowWrite(int row, int intensity)
{
  /*
   * Ensure intensity is within boundaries
   */
  if (intensity > LED_PWM_MAX) {
    intensity = LED_PWM_MAX;
  }
  if (intensity < 0) {
    intensity = 0;
  }

  /*
   * Apply DMX master intensity value
   */
 intensity = intensity * dmx_master_intensity / 255;

  /*
   * Read tilt sensor and apply value
   */
  bool tiltedUp = digitalRead(TILT_GPIO);
  if (tiltedUp) {
    ledcWrite(row, intensity);
  } else {
    ledcWrite(NUM_ROWS-1-row, intensity);
  }
}


/*
 * Test mode loop
 *
 * This code is not normally invoked, but can be triggered by setting the
 * TEST_MODE constant. It simply loops through all columns and rows, for
 * a hardware test.
 */
void testLoop()
{
  /*
   * Otherwise, apply the selected levels
   */
  for (int column = 0; column < NUM_COLUMNS; column++) {
    Serial.println((String) "Column " + column);
    digitalWrite(COLUMN_GPIO[column], HIGH);

    for (int row = 0; row < NUM_ROWS; row++) {
      int tilt = digitalRead(TILT_GPIO);
      Serial.println((String) "Column " + column + " Row " + row + " Tilt " + tilt);
      for (int intensity = 0; intensity < LED_PWM_MAX ; intensity++) {
        rowWrite(row, intensity);
        delay(2);
      }

      /*
       * Take care of an OTA upgrade here. If we don't, too much time will elapse in this
       * loop and upgrades will time out.
       */
      ArduinoOTA.handle();
      
      delay(100);
      for (int intensity = LED_PWM_MAX; intensity >= 0 ; intensity--) {
        rowWrite(row, intensity);
        delay(2);
      }
    }

    digitalWrite(COLUMN_GPIO[column], LOW);
  }
}

/*
 * This is the main flame loop.
 */
void flameLoop()
{
  /*
   * Check if we have a current DMX signal. If not, turn off.
   */
  if (millis() > WifiDMX::dmxLastReceived + DMX_TIMEOUT*1000) {
    dmx_master_intensity = 0;
  }

  /*
   * If our master is at zero, just ensure all pins remain low. Otherwise, the bulb may flicker
   * and glow a tiny bit even when at zero.
   */
  if (dmx_master_intensity == 0) {
    for (int column = 0; column < NUM_COLUMNS; column++) {
      digitalWrite(COLUMN_GPIO[column], LOW);
    }
    return;
  }

  /*
   * Generate a new flame matrix based on the flame probability values.
   */
  for (int column = 0; column < NUM_COLUMNS; column++)
  {
    for (int row = 0; row < NUM_ROWS; row++)
    {
      bool isOn = (FLAME_PROBABILITY[row] > random(100));

      nextMatrix[column][row] = (isOn ? LED_PWM_MAX : 0);
      deltaMatrix[column][row] = (nextMatrix[column][row] - currentMatrix[column][row]) / FADE_STEPS;
    }
  }

  /*
   * Fade to the new matrix over FADE_STEPS steps
   */
  for (int fadeStep = 0; fadeStep < FADE_STEPS ; fadeStep++ )
  {
    for (int column = 0; column < NUM_COLUMNS; column++)
    {
      // Set the row outputs "in the blind" -- all columns are currently turned off.
      for (int row = 0; row < NUM_ROWS; row++)
      {
        currentMatrix[column][row] = currentMatrix[column][row] + deltaMatrix[column][row];
        rowWrite(row, currentMatrix[column][row]);
      }

      // Once the output is set for all rows in this column, display the output for a brief period of time.
      digitalWrite(COLUMN_GPIO[column], HIGH);
      delay(FADE_DWELL);
      digitalWrite(COLUMN_GPIO[column], LOW);
    }
  }
}
