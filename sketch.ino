/*******************************************************************************
 *
 *            ARCS - Amateur Radio Control & Clock Solution
 *           -----------------------------------------------
 * A full QRP/Hombrew transceiver control with RF generation, the Cuban way.
 *
 * Copyright (C) 2016 Pavel Milanes (CO7WT) <pavelmc@gmail.com>
 *
 * This work is based on the previous work of these great people:
 *  * NT7S (http://nt7s.com)
 *  * SQ9NJE (http://sq9nje.pl)
 *  * AK2B (http://ak2b.blogspot.com)
 *  * WJ6C for the idea and hardware support.
 *  * Many other Cuban hams with critics and opinions
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*******************************************************************************/

/*******************************************************************************
 * Important information you need to know about this code
 *  * The Si5351 original lib use the PLLs like this
 *    * CLK0 uses the PLLA
 *    * CLK1 uses the PLLB
 *    * CLK2 uses the PLLB
 *
 *  * We are fine with that specs, as we need the VFO source to be unique
 *   (it has it's own PLL) to minimize the crosstalk between outputs
 *    > CLK0 is used to VFO (the VFO will *always* be above the RF freq)
 *    > CLK1 will be used optionally as a XFO for trx with a second conversion
 *    > CLK2 is used for the BFO
 *
 *  * Please have in mind that this IC has a SQUARE wave output and you need to
 *    apply some kind of low-pass/band-pass filtering to smooth it and get rid
 *    of the nasty harmonics
 ******************************************************************************/

// define the max count for analog buttons in the BMux library
#define BUTTONS_COUNT 4

// now the main include block
#include <Rotary.h>         // https://github.com/mathertel/RotaryEncoder/
#include <si5351.h>         // https://github.com/etherkit/Si5351Arduino/
#include <Bounce2.h>        // https://github.com/thomasfredericks/Bounce2/
#include <BMux.h>           // https://github.com/pavelmc/BMux/
#include <ft857d.h>         // https://github.com/pavelmc/ft857d/
#include <EEPROM.h>         // default
#include <Wire.h>           // default
#include <LiquidCrystal.h>  // default

// the fingerprint to know the EEPROM is initialized, we need to stamp something
// on it, as the 5th birthday anniversary of my daughter was the date I begin to
// work on this project, so be it: 2016 June 1st
#define EEPROMfingerprint "20160601"

/***************************** !!! WARNING !!! *********************************
 *
 * Be aware that the library use the freq on 1/10 hz resolution, so 10 Khz
 * is expressed as 10_000_0 (note the extra zero)
 *
 * This will allow us to climb up to 160 Mhz & we don't need the extra accuracy
 *
 * Check you have in the Si5351 this param defined as this:
 *     #define SI5351_FREQ_MULT                    10ULL
 *
 * Also we use a module with a 27 Mhz xtal, check this also
 *     #define SI5351_XTAL_FREQ                    27000000
 *
* *****************************************************************************/

/************************** USER BOARD SELECTION *******************************
 * if you have the any of the COLAB shields uncomment the following line.
 * (the sketch is configured by default for my particular hardware)
 ******************************************************************************/
#define COLAB

/*********************** FILTER PRE-CONFIGURATIONS *****************************
 * As this project aims to easy the user configuration we will pre-stablish some
 * defaults for some of the most common hardware configurations we have in Cuba
 *
 * The idea is that if you find a matching hardware case is just a matter of
 * uncomment the option and comment the others, compile the sketch an upload and
 * you will get ball park values for your configuration.
 *
 * See the Setup_en|Setup_es PDF files in the doc directory, if you use this and
 * use a custom hardware variant I can code it to make your life easier, write
 * to pavelmc@gmail.com for details [author]
 *
 * WARNING: at least one must be un-commented for the compiler to work
 ******************************************************************************/
// Single conversion Radio using the SSB filter of an FT-80C/FT-747GX
// the filter reads: "Type: XF-8.2M-242-02, CF: 8.2158 Mhz"
#define SSBF_FT747GX

// Single conversion Radio using the SSB filter of a Polosa/Angara
// the filter reads: "ZMFDP-500H-3,1"
//
// WARNING !!!! This filters has a very high loss (measured around -20dB)
//
// You must connect to it by the low impedance taps and tune both start & end
// of the filter with a 5-20 pF trimmer and a 33 to 56 pF in parallel depending
// on the particular trimmer range
//
//#define SSBF_URSS_500H

// Double conversion (28.8MHz/200KHz) radio from RF board of a RFT SEG-15
// the radio has two 200 Khz filters, we used the one that reads:
// "MF 200 - E - 0235 / RTF 02.83"
//
// WARNING !!!! See notes below in the ifdef structure for the RFT SEG-15
//
//#define SSBF_RFT_SEG15


// The eeprom & sketch version; if the eeprom version is lower than the one on
// the sketch we force an update (init) to make a consistent work on upgrades
#define EEP_VER     4
#define FMW_VER     9

// the limits of the VFO, for now just 40m for now; you can tweak it with the
// limits of your particular hardware, again this are LCD diplay frequencies.
#define F_MIN      65000000     // 6.500.000
#define F_MAX      75000000     // 7.500.000

// encoder pins
#define ENC_A    3              // Encoder pin A
#define ENC_B    2              // Encoder pin B
#define inPTT   13              // PTT/CW KEY Line with pullup from the outside
#define PTT     12              // PTT actuator, this will put thet radio on TX

#if defined (COLAB)
    // Any of the COLAB shields
    #define btnPush  11             // Encoder Button
#else
    // Pavel's hardware
    #define btnPush  4              // Encoder Button
#endif

// rotary encoder library setup
Rotary encoder = Rotary(ENC_A, ENC_B);

// the debounce instances
#define debounceInterval  10    // in milliseconds
Bounce dbBtnPush = Bounce();

// analog buttons library declaration (BMux)
// define the analog pin to handle the buttons
#define KEYS_PIN  2
BMux abm;

// creating the analog buttons for the BMux lib
Button bvfoab   = Button(597, &btnVFOABClick);      // 10k
Button bmode    = Button(372, &btnModeClick);       // 4.7k
Button brit     = Button(208, &btnRITClick);        // 2.2k
Button bsplit   = Button(817, &btnSPLITClick);      // 22k

// the CAT radio lib
ft857d cat = ft857d();

// lcd pins assuming a 1602 (16x2) at 4 bits
#if defined (COLAB)
    // COLAB shield + Arduino Mini/UNO Board
    #define LCD_RS      5
    #define LCD_E       6
    #define LCD_D4      7
    #define LCD_D5      8
    #define LCD_D6      9
    #define LCD_D7      10
#else
    // Pavel's hardware
    #define LCD_RS      8    // 14 < Real pins in a 28PDIP
    #define LCD_E       7    // 13
    #define LCD_D4      6    // 12
    #define LCD_D5      5    // 11
    #define LCD_D6      10   // 16
    #define LCD_D7      9    // 15
#endif

// lcd library setup
LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
// defining the chars

byte bar[8] = {
  B11111,
  B11111,
  B11111,
  B11111,
  B11111,
  B11111,
  B11111
};

byte s1[8] = {
  B11111,
  B10011,
  B11011,
  B11011,
  B11011,
  B10001,
  B11111
};

byte s3[8] = {
  B11111,
  B10001,
  B11101,
  B10001,
  B11101,
  B10001,
  B11111
};

byte s5[8] = {
  B11111,
  B10001,
  B10111,
  B10001,
  B11101,
  B10001,
  B11111
};

byte s7[8] = {
  B11111,
  B10001,
  B11101,
  B11011,
  B11011,
  B11011,
  B11111
};

byte s9[8] = {
  B11111,
  B10001,
  B10101,
  B10001,
  B11101,
  B11101,
  B11111
};

// Si5351 library declaration
Si5351 si5351;

// run mode constants
#define MODE_LSB 0
#define MODE_USB 1
#define MODE_CW  2
#define MODE_MAX 2                // the mode count to cycle (-1)
#define MAX_RIT 99900             // RIT 9.99 Khz * 10

// config constants
#define CONFIG_IF       0
#define CONFIG_VFO_A    1
#define CONFIG_MODE_A   2
#define CONFIG_LSB      3
#define CONFIG_USB      4
#define CONFIG_CW       5
#define CONFIG_XFO      6
#define CONFIG_PPM      7
// --
#define CONFIG_MAX 7               // the amount of configure options

// sampling interval for the AGC, 1/4 second, averaged every 4 samples and with
// a scope of the last 6 samples (aka: 2/3 moving average)
#define SM_SAMPLING_INTERVAL  250

// EERPOM saving interval (if some parameter has changed) in 1/4 seconds var is
// word so max is 65535 of 1/4 secs or: ~ 16383 sec ~ 273 min ~ 4h 33 min
#define SAVE_INTERVAL 40      // aprox 10 minutes

// hardware pre configured values
#if defined (SSBF_FT747GX)
    // Pre configured values for a Single conversion radio using the FT-747GX
    int lsb =        -16000;
    int usb =         16000;
    int cw =              0;
    long xfo =            0;
    long ifreq =   82076600;
#endif

#if defined (SSBF_URSS_500H)
    // Pre configured values for a Single conversion radio using the Polosa
    // Angara 500H filter
    int lsb =        -17500;
    int usb =         17500;
    int cw =              0;
    long xfo =            0;
    long ifreq =    4980800;
#endif

#if defined (SSBF_RFT_SEG15)
    // Pre configured values for a Double conversion radio using the RFT SEG15
    // RF board using the filter marked as:
    int lsb =        -14350;
    int usb =         14350;
    int cw =              0;
    long ifreq =  282000000;
    /***************************************************************************
     *                     !!!!!!!!!!!    WARNING   !!!!!!!!!!
     *
     *  The Si5351 has serious troubles to keep the accuracy in the XFO & BFO
     *  for the case in where you want to substitute all the frequency
     *  generators inside the SEG-15.
     *
     *  This combination can lead to troubles with the BFO not moving in the
     *  steps you want.
     *
     *  This is because the XFO & BFO share the same VCO inside the Si5351 and
     *  the firmware has to make some compromises to get the two oscillators
     *  running at the same time, in this case some accuracy is sacrificed and
     *  you get as a result the BFO jumping in about 150 hz steps even if you
     *  instruct it as 1 Hz.
     *
     *  The obvious solution is to keep the XFO as a XTAL one inside the radio
     *  and move the IF Frequency to tune the differences of your own XFO Xtal
     *  in our test the XTAL was in the valued showed below.
     *
     *  In this case the firmware will keep track of the XFO but will never
     *  turn it on. It is used only for the calculations
    */
    long xfo =  279970000;
#endif

// Put the value here if you have the default ppm corrections for you Si5351
// if you set it here it will be stored on the EEPROM on the initial start.
//
// Otherwise set it to zero and you can set it up via the SETUP menu, but it
// will get reset to zero each time you program the arduino...
// so... PUT YOUR VALUE HERE
long si5351_ppm = 224380;    // it has the *10 included

// the variables
long vfoa = 71038000;             // default starting VFO A freq
long vfob = 71755000;             // default starting VFO B freq
long tvfo = 0;                    // temporal VFO storage for RIT usage
long txSplitVfo =  0;             // temporal VFO storage for RIT usage when TX
byte step = 3;                    // default steps position index:
                                  // as 1*10E3 = 1000 = 100hz; step position is
                                  // calculated to avoid to use a big array
                                  // see getStep() function
boolean update = true;            // lcd update flag in normal mode
byte encoderState = DIR_NONE;     // encoder state
byte config = 0;                  // holds the configuration item selected
boolean inSetup = false;          // the setup mode, just looking or modifying
boolean mustShowStep = false;     // var to show the step instead the bargraph
#define showStepTimer   8000      // a relative amount of time to show the mode
                                  // aprox 3 secs
word showStepCounter = showStepTimer; // the timer counter
boolean showStepEnd =  false;     // this one rises when the mustShowStep falls
boolean runMode =      true;      // true: normal, false: setup
boolean activeVFO =    true;      // true: A, False: B
byte VFOAMode =        MODE_LSB;
byte VFOBMode =        MODE_LSB;
boolean ritActive =    false;     // true: rit active, false: rit disabled
boolean tx =           false;     // whether we are on TX mode or not
#define BARGRAPH_SAMPLES    6
byte pep[BARGRAPH_SAMPLES];        // s-meter readings storage
long lastMilis =       0;          // to track the last sampled time
boolean smeterOk =     false;      // it's ok to show the bar graph
boolean split =        false;      // this holds th split state
boolean catTX =        false;      // CAT command to go to PTT
byte sMeter =          0;          // hold the value of the Smeter readings
                                   // in both RX and TX modes
word qcounter =        0;          // Timer to be incremented each 1/4 second
                                   // approximately, to trigger a EEPROM update
                                   // if needed

// temp vars (used in the loop function)
boolean tbool   = false;

// structured data: Main Configuration Parameters
struct mConf {
    char finger[9] =  EEPROMfingerprint;
    byte version = EEP_VER;
    long ifreq;
    long vfoa;
    byte vfoaMode;
    long vfob;
    byte vfobMode;
    long xfo;
    int lsb;
    int usb;
    int cw;
    int ppm;
};

// declaring the main configuration variable for mem storage
struct mConf conf;

// pointers to the actual values
long *ptrVFO;
byte *ptrMode;


/******************************* MISCELLANEOUS ********************************/


// split check
void splitCheck() {
    if (split) {
        // revert back the VFO
        activeVFO = !activeVFO;
        updateAllFreq();
    }
}


// change the modes in a cycle
void changeMode() {
    // normal increment
    *ptrMode += 1;

    // checking for overflow
    if (*ptrMode > MODE_MAX) *ptrMode = 0;

    // Apply the changes
    updateAllFreq();
}


// change the steps
void changeStep() {
    // calculating the next step
    if (step < 7) {
        // simple increment
        step += 1;
    } else {
        // default start mode is 2 (10Hz)
        step = 2;
        // in setup mode and just specific modes it's allowed to go to 1 hz
        boolean allowedModes = false;
        allowedModes = allowedModes or (config == CONFIG_LSB);
        allowedModes = allowedModes or (config == CONFIG_USB);
        allowedModes = allowedModes or (config == CONFIG_PPM);
        allowedModes = allowedModes or (config == CONFIG_XFO);
        if (!runMode and allowedModes) step = 1;
    }

    // if in normal mode reset the counter to show the change in the LCD
    if (runMode) showStepCounter = showStepTimer;     // aprox half second

    // tell the LCD that it must show the change
    mustShowStep = true;
}


// RIT toggle
void toggleRit() {
    if (!ritActive) {
        // going to activate it: store the active VFO
        tvfo = *ptrVFO;
        ritActive = true;
    } else {
        // going to deactivate: reset the stored VFO
        *ptrVFO = tvfo;
        ritActive = false;
    }
}


// return the right step size to move
long getStep () {
    // we get the step from the global step var
    long ret = 1;

    // validation just in case
    if (step == 0) step = 1;
    for (byte i=0; i < step; i++) ret *= 10;
    return ret;
}


// in TX, check if we must go to RX
void going2RX(boolean lptt) {
    // if hardware PTT or CAT changed, I must go to RX
    if (lptt or !catTX) {
        // PTT released, going to RX
        tx = false;
        digitalWrite(PTT, tx);

        // make changes if tx goes active when RIT is active
        if (ritActive) {
            // get the TX vfo and store it as the reference for the RIT
            tvfo = *ptrVFO;
            // restore the rit VFO to the actual VFO
            *ptrVFO = txSplitVfo;
        }

        // make the split changes if needed
        splitCheck();

        // rise the update flag
        update = true;
    }
}


// in RX, check if we must go to TX
void going2TX(boolean lptt) {
    if (!lptt or catTX) {
        // PTT asserted, going into TX
        tx = true;
        digitalWrite(PTT, tx);

        // make changes if tx goes active when RIT is active
        if (ritActive) {
            // save the actual rit VFO
            txSplitVfo = *ptrVFO;
            // set the TX freq to the active VFO
            *ptrVFO = tvfo;
        }

        // make the split changes if needed
        splitCheck();

        // rise the update flag
        update = true;
    }
}


// swaps the VFOs
void swapVFO(byte force = 2) {
    // swap the VFOs if needed
    if (force == 2) {
        // just toggle it
        activeVFO = !activeVFO;
    } else {
        // set it as commanded
        activeVFO = bool(force);
    }

    // setting the VFO/mode pointers
    if (activeVFO) {
        ptrVFO = &vfoa;
        ptrMode = &VFOAMode;
    } else {
        ptrVFO = &vfob;
        ptrMode = &VFOBMode;
    }
}


/******************************** ENCODER *************************************/


// the encoder has moved
void encoderMoved(int dir) {
    // check the run mode
    if (runMode) {
        // update freq
        updateFreq(dir);
        update = true;
    } else {
        // update the values in the setup mode
        updateSetupValues(dir);
    }
}


// update freq procedure
void updateFreq(int dir) {
    long newFreq = *ptrVFO;

    if (ritActive) {
        // we fix the steps to 10 Hz in rit mode
        newFreq += 100 * dir;
    } else {
        // otherwise we use the default step on the environment
        newFreq += getStep() * dir;
    }

    // limit check
    if (ritActive) {
        // check we don't exceed the MAX_RIT
        if (abs(tvfo - newFreq) > MAX_RIT) return;
    } else {
        // limit check for normal VFO
        if(newFreq > F_MAX) newFreq = F_MIN;
        if(newFreq < F_MIN) newFreq = F_MAX;
    }

    // apply the change
    *ptrVFO = newFreq;

    // update the output freq
    setFreqVFO();
}


/************************** LCD INTERFACE RELATED *****************************/


// update the setup values
void updateSetupValues(int dir) {
    // we are in setup mode, showing or modifying?
    if (!inSetup) {
        // just showing, show the config on the LCD
        updateShowConfig(dir);
    } else {
        // change the VFO to A by default
        swapVFO(1);
        // I'm modifying, switch on the config item
        switch (config) {
            case CONFIG_IF:
                // change the IF value
                ifreq += getStep() * dir;
                break;
            case CONFIG_VFO_A:
                // change VFOa
                *ptrVFO += getStep() * dir;
                break;
            case CONFIG_MODE_A:
                // hot swap it
                changeMode();
                // set the default mode in the VFO A
                showModeSetup(VFOAMode);
                break;
            case CONFIG_USB:
                // change the mode to USB
                *ptrMode = MODE_USB;
                // change the USB BFO
                usb += getStep() * dir;
                break;
            case CONFIG_LSB:
                // change the mode to LSB
                *ptrMode = MODE_LSB;
                // change the LSB BFO
                lsb += getStep() * dir;
                break;
            case CONFIG_CW:
                // change the mode to CW
                *ptrMode = MODE_CW;
                // change the CW BFO
                cw += getStep() * dir;
                break;
            case CONFIG_PPM:
                // change the Si5351 PPM
                si5351_ppm += getStep() * dir;
                // instruct the lib to use the new ppm value
                si5351.set_correction(si5351_ppm);
                break;
            case CONFIG_XFO:
                // change XFO
                xfo += getStep() * dir;
                break;
        }

        // for all cases update the freqs
        updateAllFreq();

        // update el LCD
        showModConfig();
    }
}


// update the configuration item before selecting it
void updateShowConfig(int dir) {
    // move the config item
    int tconfig = config;
    tconfig += dir;

    if (tconfig > CONFIG_MAX) tconfig = 0;
    if (tconfig < 0) tconfig = CONFIG_MAX;
    config = tconfig;

    // update the LCD
    showConfig();
}


// show the mode for the passed mode in setup mode
void showModeSetup(byte mode) {
    // now I have to print it out
    lcd.setCursor(0, 1);
    spaces(11);
    showModeLcd(mode);
}


// print a string of spaces, to save some space
void spaces(byte m) {
    // print m spaces in the LCD
    while (m) {
        lcd.print(" ");
        m--;
    }
}


// show the labels of the config
void showConfigLabels() {
    switch (config) {
        case CONFIG_IF:
            lcd.print(F("  IF frequency  "));
            break;
        case CONFIG_VFO_A:
            lcd.print(F("VFO A freq"));
            break;
        case CONFIG_MODE_A:
            lcd.print(F("VFO A mode"));
            break;
        case CONFIG_USB:
            lcd.print(F(" BFO freq. USB  "));
            break;
        case CONFIG_LSB:
            lcd.print(F(" BFO freq. LSB  "));
            break;
        case CONFIG_CW:
            lcd.print(F(" BFO freq. CW   "));
            break;
        case CONFIG_PPM:
            lcd.print(F("Si5351 PPM error"));
            break;
        case CONFIG_XFO:
            lcd.print(F("XFO frequency   "));
            break;
    }
}


// show the setup main menu
void showConfig() {
    // we have update the whole LCD screen
    lcd.setCursor(0, 0);
    lcd.print(F("#> SETUP MENU <#"));
    lcd.setCursor(0, 1);
    // show the specific item label
    showConfigLabels();
}


// print the sign of a passed parameter
void showSign(long val) {
    // just print it
    if (val > 0) lcd.print("+");
    if (val < 0) lcd.print("-");
    if (val == 0) lcd.print(" ");
}


// show the ppm as a signed long
void showConfigValueSigned(long val) {
    if (config == CONFIG_PPM) {
        lcd.print(F("PPM: "));
    } else {
        lcd.print(F("Val:"));
    }

    // detect the sign
    showSign(val);

    // print it
    formatFreq(abs(val));
}


// Show the value for the setup item
void showConfigValue(long val) {
    lcd.print(F("Val:"));
    formatFreq(val);

    // if on normal mode we show in 10 Hz
    if (runMode) {
        lcd.print("0");
    }

    lcd.print(F("hz"));
}


// update the specific setup item
void showModConfig() {
    lcd.setCursor(0, 0);
    showConfigLabels();

    // show the specific values
    lcd.setCursor(0, 1);
    switch (config) {
        case CONFIG_IF:
            showConfigValue(ifreq);
            break;
        case CONFIG_VFO_A:
            showConfigValue(vfoa);
            break;
        case CONFIG_MODE_A:
            showModeSetup(VFOAMode);
        case CONFIG_USB:
            showConfigValueSigned(usb);
            break;
        case CONFIG_LSB:
            showConfigValueSigned(lsb);
            break;
        case CONFIG_CW:
            showConfigValueSigned(cw);
            break;
        case CONFIG_PPM:
            showConfigValueSigned(si5351_ppm);
            break;
        case CONFIG_XFO:
            showConfigValue(xfo);
            break;
    }
}


// format the freq to easy viewing
void formatFreq(long freq) {
    // for easy viewing we format a freq like 7.110 to 7.110.00
    long t;

    // get the freq in Hz as the lib needs in 1/10 hz resolution
    freq /= 10;

    // Mhz part
    t = freq / 1000000;
    if (t < 10) lcd.print(" ");
    if (t == 0) {
        spaces(2);
    } else {
        lcd.print(t);
        // first dot: optional
        lcd.print(".");
    }
    // Khz part
    t = (freq % 1000000);
    t /= 1000;
    if (t < 100) lcd.print("0");
    if (t < 10) lcd.print("0");
    lcd.print(t);
    // second dot: forced
    lcd.print(".");
    // hz part
    t = (freq % 1000);
    if (t < 100) lcd.print("0");
    // check if in config and show up to 1hz resolution
    if (!runMode) {
        if (t < 10) lcd.print("0");
        lcd.print(t);
    } else {
        lcd.print(t/10);
    }
}


// lcd update in normal mode
void updateLcd() {
    // this is the designed normal mode LCD
    /******************************************************
     *   0123456789abcdef
     *  ------------------
     *  |A 14.280.25 lsb |
     *  |RX 0000000000000|
     *
     *  |RX +9.99 Khz    |
     *
     *  |RX 100hz        |
     *  ------------------
     ******************************************************/

    // first line
    lcd.setCursor(0, 0);
    // active a?
    if (activeVFO) {
        lcd.print(F("A"));
    } else {
        lcd.print(F("B"));
    }

    // split?
    if (split) {
        // ok, show the split status as a * sign
        lcd.print(F("*"));
    } else {
        // print a separator.
        spaces(1);
    }

    if (activeVFO) {
        formatFreq(vfoa);
        spaces(1);
        showModeLcd(VFOAMode);
    } else {
        formatFreq(vfob);
        spaces(1);
        showModeLcd(VFOBMode);
    }

    // second line
    lcd.setCursor(0, 1);
    if (tx) {
        lcd.print(F("TX "));
    } else {
        lcd.print(F("RX "));
    }

    // if we have a RIT or steps we manage it here and the bar will hold
    if (ritActive) showRit();

    // show the step if it must
    if (mustShowStep) showStep();
}


// show rit in LCD
void showRit() {
    /***************************************************************************
     * RIT show something like this on the line of the non active VFO
     *
     *   |0123456789abcdef|
     *   |----------------|
     *   |RX RIT -9.99 khz|
     *   |----------------|
     *
     *             WARNING !!!!!!!!!!!!!!!!!!!!1
     *  If the user change the VFO we need to *RESET* & disable the RIT ASAP.
     *
     **************************************************************************/

    // get the active VFO to calculate the deviation
    long vfo = *ptrVFO;

    long diff = vfo - tvfo;

    // scale it down, we don't need Hz resolution here
    diff /= 10;

    // we start on line 2, char 3 of the second line
    lcd.setCursor(3, 1);
    lcd.print(F("RIT "));

    // show the difference in Khz on the screen with sign
    // diff can overflow the input of showSign, so we scale it down
    showSign(diff);

    // print the freq now, we have a max of 10 Khz (9.990 Khz)
    diff = abs(diff);

    // Khz part
    word t = diff / 1000;
    lcd.print(t);
    lcd.print(".");
    // hz part
    t = diff % 1000;
    if (t < 100) lcd.print("0");
    if (t < 10) lcd.print("0");
    lcd.print(t);
    // unit
    lcd.print(F("kHz"));
}


// show the mode on the LCD
void showModeLcd(byte mode) {
    // print it
    switch (mode) {
        case MODE_USB:
          lcd.print(F("USB "));
          break;
        case MODE_LSB:
          lcd.print(F("LSB "));
          break;
        case MODE_CW:
          lcd.print(F("CW  "));
          break;
    }
}


// show the vfo step
void showStep() {
    // in nomal or setup mode?
    if (runMode) {
        // in normal mode is the second line, third char
        lcd.setCursor(3, 1);
    } else {
        // in setup mode is just in the begining of the second line
        lcd.setCursor(0, 1);
    }

    // show it
    if (step == 1) lcd.print(F("  1Hz"));
    if (step == 2) lcd.print(F(" 10Hz"));
    if (step == 3) lcd.print(F("100Hz"));
    if (step == 4) lcd.print(F(" 1kHz"));
    if (step == 5) lcd.print(F("10kHz"));
    if (step == 6) lcd.print(F(" 100k"));
    if (step == 7) lcd.print(F(" 1MHz"));
    spaces(11);
}


/********************************* SET & GET **********************************/


// set the calculated freq to the VFO
void setFreqVFO() {
    long freq = *ptrVFO + ifreq;

    if (*ptrMode == MODE_USB) freq += usb;
    if (*ptrMode == MODE_LSB) freq += lsb;
    if (*ptrMode == MODE_CW)  freq += cw;

    si5351.set_freq(freq, 0, SI5351_CLK0);
}


// Force freq update for all the environment vars
void updateAllFreq() {
    // VFO update
    setFreqVFO();

    // BFO update
    long freq = ifreq - xfo;

    // mod it by mode
    if (*ptrMode == MODE_USB) freq += usb;
    if (*ptrMode == MODE_LSB) freq += lsb;
    if (*ptrMode == MODE_CW)  freq += cw;

    // deactivate it if zero
    if (freq == 0) {
        // deactivate it
        si5351.output_enable(SI5351_CLK2, 0);
    } else {
        // output it
        si5351.output_enable(SI5351_CLK2, 1);
        si5351.set_freq(freq, 0, SI5351_CLK2);
    }

    // XFO update
    #if defined (SSBF_RFT_SEG15)
        // RFT SEG-15 CASE:
        // the XFO is in PLACE but it is not generated in any case
        si5351.output_enable(SI5351_CLK1, 0);
    #else
        // just put it out if it's set
        if (xfo == 0) {
            // this is only enabled if we have a freq to send outside
            si5351.output_enable(SI5351_CLK1, 0);
        } else {
            si5351.output_enable(SI5351_CLK1, 1);
            si5351.set_freq(xfo, 0, SI5351_CLK1);
            // WARNING This has a shared PLL with the BFO and accuracy may be
            // affected
        }
    #endif
}


/********************************** EEPROM ************************************/


// check if the EEPROM is initialized
boolean checkInitEEPROM() {
    // read the eeprom config data
    EEPROM.get(0, conf);

    // check for the initializer and version
    if (strcmp(conf.finger, EEPROMfingerprint) and EEP_VER == conf.version)
        return true;

    // return false: default
    return false;
}


// initialize the EEPROM mem, also used to store the values in the setup mode
// this procedure has a protection for the EEPROM life using update semantics
// it actually only write a cell if it has changed
void saveEEPROM() {
    // load the parameters in the environment
    conf.vfoa       = vfoa;
    conf.vfoaMode   = VFOAMode;
    conf.vfob       = vfob;
    conf.vfobMode   = VFOBMode;
    conf.ifreq      = ifreq;
    conf.lsb        = lsb;
    conf.usb        = usb;
    conf.cw         = cw;
    conf.xfo        = xfo;
    conf.ppm        = si5351_ppm;

    // write it
    EEPROM.put(0, conf);
}


// load the eprom contents
void loadEEPROMConfig() {
    // write it
    EEPROM.get(0, conf);

    // load the parameters to the environment
    vfoa        = conf.vfoa;
    VFOAMode    = conf.vfoaMode;
    vfob        = conf.vfob;
    VFOBMode    = conf.vfobMode;
    lsb         = conf.lsb;
    usb         = conf.usb;
    cw          = conf.cw;
    xfo         = conf.xfo;
    si5351_ppm  = conf.ppm;
}


/**************************** SMETER / TX POWER *******************************/


// show the bar graph for the RX or TX modes
void showBarGraph() {
    // we are working on a 2x16 and we have 13 bars to show (0-12)
    byte ave = 0, i;
    volatile static byte barMax = 0;
    volatile static boolean lastShowStep = 0;

    // find the average
    for (i=0; i<15; i++) ave += pep[i];
    ave /= 15;

    // set the smeter reading on the global scope for CAT readings
    sMeter = ave;

    // scale it down to 0-12 from 0-15 now
    ave = map(ave, 0, 15, 0, 12);

    // printing only the needed part of the bar, if growing or shrinking
    // if the same no action is required, remember we have to minimize the
    // writes to the LCD to minimize QRM

    // but there is a special case: just after the show step expires we have
    // to push the limits to be sure the step label get fully erased
    if (showStepEnd and ave == barMax) ave = ave + 1;

    // growing bar: print the difference
    if (ave > barMax) {
        // special case
        if (showStepEnd) {
            barMax = 0;
            showStepEnd = false;

            // grow to erase the full step label
            if (ave < 5 ) ave = 5;
        }

        // LCD position & print the bars
        lcd.setCursor(3 + barMax, 1);

        // write it
        for (i = barMax; i <= ave; i++) {
            switch (i) {
                case 0:
                    lcd.write(byte(1));
                    break;
                case 2:
                    lcd.write(byte(2));
                    break;
                case 4:
                    lcd.write(byte(3));
                    break;
                case 6:
                    lcd.write(byte(4));
                    break;
                case 8:
                    lcd.write(byte(5));
                    break;
                default:
                    lcd.write(byte(0));
                    break;
            }
        }
    }

    // shrinking bar: erase the old ones print spaces to erase just the diff
    if (barMax > ave) {
        // special case
        if (showStepEnd) {
            barMax = 5;
            showStepEnd = false;
        }

        // erase it
        i = barMax;
        while (i > ave) {
            lcd.setCursor(3 + i, 1);
            lcd.print(" ");
            i--;
        }
    }

    // put the var for the next iteration
    barMax = ave;

    // load the last step for the next iteration
    lastShowStep = mustShowStep;
}


// take a sample an inject it on the array
void takeSample() {
    // we are sensing a value that must move in the 0-1.1v so internal reference
    analogReference(INTERNAL);
    word val;
    byte anPin;

    if (tx) { anPin = 1; } else { anPin = 0; }
    val = analogRead(anPin);

    // scale it to 4 bits (0-15) for CAT purposes
    val = map(val, 0, 1023, 0, 15);

    // push it in the array
    for (byte i = 0; i < BARGRAPH_SAMPLES - 1; i++) pep[i] = pep[i + 1];
    pep[BARGRAPH_SAMPLES - 1] = val;

    // reset the reference for the buttons handling
    analogReference(DEFAULT);
}


// smeter reading, this take a sample of the smeter/txpower each time; an will
// rise a flag when they have rotated the array of measurements 2/3 times to
// have a moving average
void smeter() {
    // static smeter array counter
    volatile static byte smeterCount = 0;

    // no matter what, I must keep taking samples
    takeSample();

    // it has rotated already?
    if (smeterCount > (BARGRAPH_SAMPLES * 2 / 3)) {
        // rise the flag about the need to show the bar graph and reset the count
        smeterOk = true;
        smeterCount = 0;
    } else {
        // just increment it
        smeterCount += 1;
    }
}


/******************************* CAT ******************************************/


// instruct the sketch that must go in/out of TX
void catGoPtt(boolean tx) {
    catTX = tx;
    update = true;
}


// set VFO toggles from CAT
void catGoToggleVFOs() {
    activeVFO = !activeVFO;
    update = true;
}


// set freq from CAT
void catSetFreq(long f) {
    // we use 1/10 hz so scale it
    f *= 10;

    // check for the freq boundaries
    if (f > F_MAX) return;
    if (f < F_MIN) return;

    // set the freq for the active VFO
    if (activeVFO) {
        vfoa = f;
    } else {
        vfob = f;
    }

    // apply changes
    updateAllFreq();
    update = true;
}


// set mode from CAT
void catSetMode(byte m) {
    // the mode can be any of the CAT ones, we have to restrict it to our modes
    if (m > 2) return;  // no change

    // by luck we use the same mode than the CAT lib so far
    *ptrMode = m;

    // Apply the changes
    updateAllFreq();
    update = true;
}


// get freq from CAT
long catGetFreq() {
    // get the active VFO freq and pass it
    return *ptrVFO / 10;
}


// get mode from CAT
byte catGetMode() {
    // get the active VFO mode and pass it
    return *ptrMode;
}


// get the s meter status to CAT
byte catGetSMeter() {
    // returns a byte in wich the s-meter is scaled to 4 bytes (15)
    // it's scaled already this our code
    return sMeter;
}


// get the TXstatus to CAT
byte catGetTXStatus() {
    // prepare a byte like the one the CAT wants:

    /*
     * this must return a byte in wich the different bits means this:
     * 0b abcdefgh
     *  a = 0 = PTT off
     *  a = 1 = PTT on
     *  b = 0 = HI SWR off
     *  b = 1 = HI SWR on
     *  c = 0 = split on
     *  c = 1 = split off
     *  d = dummy data
     *  efgh = PO meter data
     */

    // build the byte to return
    return tx<<7 + split<<5 + sMeter;
}


// delay with CAT check, this is for the welcome screen
// see the note in the setup; default ~2 secs
void delayCat(int del = 2000) {
    // delay in msecs to wait
    long delay = millis() + del;
    long m = 0;

    // loop to waste time
    while (m < delay) {
        cat.check();
        m = millis();
    }
}


/******************************* BUTTONS *************************************/


// VFO A/B button click >>>> (OK/SAVE click)
void btnVFOABClick() {
    // normal mode
    if (runMode) {
        // we force to deactivate the RIT on VFO change, as it will confuse
        // the users and have a non logical use, only if activated and
        // BEFORE we change the active VFO
        if (ritActive) toggleRit();

        // now we swap the VFO.
        swapVFO();

        // update VFO/BFO and instruct to update the LCD
        updateAllFreq();

        // set the LCD update flag
        update = true;

        // Save the VFOs and modes in the eeprom
        saveEEPROM();
        // reset the timed save counter to avoid re-saving
        qcounter = 0;
    } else {
        // SETUP mode
        if (!inSetup) {
            // I'm going to setup a element
            inSetup = true;

            // change the mode to follow VFO A
            if (config == CONFIG_USB) VFOAMode = MODE_USB;
            if (config == CONFIG_LSB) VFOAMode = MODE_LSB;

            // config update on the LCD
            showModConfig();
        } else {
            // get out of the setup change
            inSetup = false;

            // save to the eeprom
            saveEEPROM();

            // lcd delay to show it properly (user feedback)
            lcd.setCursor(0, 0);
            lcd.print(F("##   SAVED    ##"));
            delay(1000);

            // show setup
            showConfig();

            // reset the minimum step if set (1hz > 10 hz)
            if (step == 1) step = 2;
        }
    }
}


// MODE button click >>>> (CANCEL click)
void btnModeClick() {
    // normal mode
    if (runMode) {
        changeMode();
        update = true;
    } else if (inSetup) {
        // setup mode, just inside a value edit

        // get out of here
        inSetup = false;

        // user feedback
        lcd.setCursor(0, 0);
        lcd.print(F(" #  Canceled  # "));
        delay(1000);

        // show it
        showConfig();
    }
}


// RIT button click >>>> (RESET values click)
void btnRITClick() {
    // normal mode
    if (runMode) {
        toggleRit();
        update = true;
    } else if (inSetup) {
        // SETUP mode just inside a value edit.

        // where we are to know what to reset?
        if (config == CONFIG_LSB) lsb = 0;
        if (config == CONFIG_USB) usb = 0;
        if (config == CONFIG_CW) cw = 0;
        if (config == CONFIG_PPM) {
            // reset, ppm
            si5351_ppm = 0;
            si5351.set_correction(0);
        }

        // update the freqs for
        updateAllFreq();
        showModConfig();
    }
}


// SPLIT button click  >>>> (Nothing yet)
void btnSPLITClick() {
    // normal mode
    if (runMode) {
        split = !split;
        update = true;
    }

    // no function in SETUP yet.
}


/************************* SETUP and LOOP *************************************/


void setup() {
    // CAT Library setup
    cat.addCATPtt(catGoPtt);
    cat.addCATAB(catGoToggleVFOs);
    cat.addCATFSet(catSetFreq);
    cat.addCATMSet(catSetMode);
    cat.addCATGetFreq(catGetFreq);
    cat.addCATGetMode(catGetMode);
    cat.addCATSMeter(catGetSMeter);
    cat.addCATTXStatus(catGetTXStatus);
    // now we activate the library
    cat.begin(57600, SERIAL_8N1);

    // LCD init, create the custom chars first
    lcd.createChar(0, bar);
    lcd.createChar(1, s1);
    lcd.createChar(2, s3);
    lcd.createChar(3, s5);
    lcd.createChar(4, s7);
    lcd.createChar(5, s9);
    // now load the library
    lcd.begin(16, 2);
    lcd.clear();

    // buttons debounce encoder & push
    pinMode(btnPush, INPUT_PULLUP);
    pinMode(ENC_A, INPUT_PULLUP);
    pinMode(ENC_B, INPUT_PULLUP);
    dbBtnPush.attach(btnPush);
    dbBtnPush.interval(debounceInterval);

    // analog buttons setup
    abm.init(KEYS_PIN, 5, 20);
    abm.add(bvfoab);
    abm.add(bmode);
    abm.add(brit);
    abm.add(bsplit);

    // pin mode of the PTT
    pinMode(inPTT, INPUT_PULLUP);
    pinMode(PTT, OUTPUT);
    // default awake mode is RX
    digitalWrite(PTT, 0);

    // I2C init
    Wire.begin();

    // disable the outputs from the begining
    si5351.output_enable(SI5351_CLK0, 0);
    si5351.output_enable(SI5351_CLK1, 0);
    si5351.output_enable(SI5351_CLK2, 0);
    // Si5351 Xtal capacitive load
    si5351.init(SI5351_CRYSTAL_LOAD_10PF, 0);
    // setup the PLL usage
    si5351.set_ms_source(SI5351_CLK0, SI5351_PLLA);
    si5351.set_ms_source(SI5351_CLK1, SI5351_PLLB);
    si5351.set_ms_source(SI5351_CLK2, SI5351_PLLB);
    // use low power on the Si5351
    si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);
    si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_2MA);
    si5351.drive_strength(SI5351_CLK2, SI5351_DRIVE_2MA);


    // check the EEPROM to know if I need to initialize it
    if (checkInitEEPROM()) {
        // LCD
        lcd.setCursor(0, 0);
        lcd.print(F("Init EEPROM...  "));
        lcd.setCursor(0, 1);
        lcd.print(F("Please wait...  "));
        saveEEPROM();
        delayCat(); // see note below on the welcome screen
        lcd.clear();
    } else {
        // just if it's already ok
        loadEEPROMConfig();
    }

    // Welcome screen

    // A software controlling the CAT via USB will reset the sketch upon
    // connection, so we need turn the cat.check() when running the welcome
    // banners (use case: Fldigi)
    lcd.clear();
    lcd.print(F("  Aduino Arcs  "));
    lcd.setCursor(0, 1);
    lcd.print(F("Fv: "));
    lcd.print(FMW_VER);
    lcd.print(F("  Mfv: "));
    lcd.print(EEP_VER);
    delayCat();
    lcd.setCursor(0, 0);
    lcd.print(F(" by Pavel CO7WT "));
    delayCat();
    lcd.clear();

    // Check for setup mode
    if (digitalRead(btnPush) == LOW) {
        // CAT is disabled in SETUP mode
        cat.enabled = false;

        // we are in the setup mode
        lcd.setCursor(0, 0);
        lcd.print(F(" You are in the "));
        lcd.setCursor(0, 1);
        lcd.print(F("   SETUP MODE   "));
        delay(2000); // 2 secs
        lcd.clear();

        // rise the flag of setup mode for every body to see it.
        runMode = false;

        // show setup mode
        showConfig();
    }

    // setting up VFO A as principal.
    activeVFO = true;
    ptrVFO = &vfoa;
    ptrMode = &VFOAMode;

    // Enable the Si5351 outputs
    si5351.output_enable(SI5351_CLK0, 1);
    si5351.output_enable(SI5351_CLK2, 1);

    // start the VFOa and it's mode
    updateAllFreq();
}


void loop() {
    // encoder check
    encoderState = encoder.process();
    if (encoderState == DIR_CW) encoderMoved(+1);
    if (encoderState == DIR_CCW) encoderMoved(-1);

    // LCD update check in normal mode
    if (update and runMode) {
        // update and reset the flag
        updateLcd();
        update = false;
    }

    // check PTT and make the RX/TX changes
    tbool = digitalRead(inPTT);
    if (tx) { going2RX(tbool); } else { going2TX(tbool); }

    // debouce for the push
    dbBtnPush.update();
    tbool = dbBtnPush.fell();

    if (runMode) {
        // we are in normal mode

        // step (push button)
        if (tbool) {
            // VFO step change
            changeStep();
            update = true;
        }

        // Second line of the LCD, I must show the bargraph only if not rit nor steps
        if ((!ritActive and !mustShowStep) and smeterOk) showBarGraph();

        // decrement step counter
        if (mustShowStep) {
            // decrement the counter
            showStepCounter -= 1;

            // compare to show it just once, as showing it continuosly generate
            // QRM from the arduino
            if (showStepCounter == (showStepTimer - 1)) showStep();

            // detect the count end and restore to normal
            if (showStepCounter == 0) {
                mustShowStep = false;
                showStepEnd = true;
            }
        }

    } else {
        // setup mode

        // Push button is step in Config mode
        if (tbool) {
            // change the step and show it on the LCD
            changeStep();
            showStep();
        }

    }

    // sample and process the S-meter in RX & TX
    if ((millis() - lastMilis) >= SM_SAMPLING_INTERVAL) {
        // Reset the last reading to keep track
        lastMilis = millis();

        // I must sample the input
        smeter();

        // time counter for VFO remember after power off
        if (qcounter >= SAVE_INTERVAL) {
            saveEEPROM();
            qcounter = 0;
        } else {
            qcounter += 1;
        }
    }

    // CAT check
    cat.check();

    // analog buttons check
    abm.check();
}
