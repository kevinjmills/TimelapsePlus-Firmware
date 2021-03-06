/*
 *  shutter.cpp
 *  Timelapse+
 *
 *  Created by Elijah Parker
 *  Copyright 2012 Timelapse+
 *	Licensed under GPLv3
 *
 */

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/version.h>
#include <avr/eeprom.h>
#include <string.h>

#include "tldefs.h"
#include "shutter.h"
#include "clock.h"
#include "hardware.h"
#include "IR.h"
#include "bluetooth.h"
#include "debug.h"
#include "math.h"
#include "settings.h"
#include "PTP_Driver.h"
#include "PTP.h"
#include "timelapseplus.h"
#include "light.h"
#include "5110LCD.h"
#include "button.h"
#include "menu.h"

#define DEBUG

//#define DEBUG_MODE

#define RUN_DELAY 0
#define RUN_BULB 1
#define RUN_PHOTO 2
#define RUN_GAP 3
#define RUN_NEXT 4
#define RUN_END 5
#define RUN_ERROR 6

extern IR ir;
extern PTP camera;
extern settings conf;
extern shutter timer;
extern BT bt;
extern Clock clock;
extern Light light;
extern MENU menu;
extern LCD lcd;

volatile unsigned char state;
const uint16_t settings_warn_time = 0;
const uint16_t settings_mirror_up_time = 3;
volatile char cable_connected; // 1 = cable connected, 0 = disconnected

char shutter_state, ir_shutter_state; // used only for momentary toggle mode //
uint16_t BulbMax; // calculated during bulb ramp mode
program stored[MAX_STORED+1]EEMEM;
uint8_t BulbMaxEv;

const char STR_BULBMODE_ALERT[]PROGMEM = "Please make sure the camera is in bulb mode before continuing";
const char STR_BULBSUPPORT_ALERT[]PROGMEM = "Bulb mode not supported via USB. Use an adaptor cable with the USB";
const char STR_MEMORYSPACE_ALERT[]PROGMEM = "Please confirm there is enough space on the memory card in the camera";
const char STR_APERTURE_ALERT[]PROGMEM = "Note that using an aperture other than the maximum without lens-twist can cause flicker";
const char STR_DEVMODE_ALERT[]PROGMEM = "DEV Mode can interfere with the light sensor. Please disable before continuing";


/******************************************************************
 *
 *   shutter::
 *
 *
 ******************************************************************/

shutter::shutter()
{
    if(eeprom_read_byte((const uint8_t*)&stored[0].Name[0]) == 255) 
        setDefault();
    
    restoreCurrent(); // Default Program //
    ENABLE_MIRROR;
    ENABLE_SHUTTER;
    CHECK_CABLE;
    shutter_state = 0;
    ir_shutter_state = 0;
}

/******************************************************************
 *
 *   shutter::setDefault
 *
 *
 ******************************************************************/

void shutter::setDefault()
{
    current.Name[0] = 'D';
    current.Name[1] = 'E';
    current.Name[2] = 'F';
    current.Name[3] = 'A';
    current.Name[4] = 'U';
    current.Name[5] = 'L';
    current.Name[6] = 'T';
    current.Name[7] = '\0';
    current.Delay = 5;
    current.Photos = 10;
    current.Gap = 20;
    current.Exps = 3;
    current.Mode = MODE_TIMELAPSE;
    current.Exp = 46;
    current.ArbExp = 100;
    current.Bracket = 6;
    current.Keyframes = 1;
    current.Duration = 3600;
    current.BulbStart = 53;
    current.Bulb[0] = 36;
    current.Key[0] = 3600;
    current.brampMethod = BRAMP_METHOD_AUTO;
    current.Integration = 10;
    save(0);

    uint8_t i;
    for(i = 1; i < MAX_STORED; i++)
    {
        eeprom_write_byte((uint8_t*)&stored[i].Name[0],  255);
    }

    saveCurrent();
}

/******************************************************************
 *
 *   shutter::off
 *
 *
 ******************************************************************/

void shutter::off(void)
{
    shutter_off();
}
void shutter_off(void)
{
    if(conf.devMode) 
        hardware_flashlight(0);
    
    SHUTTER_CLOSE;
    MIRROR_DOWN; 
    clock.in(20, &check_cable);
    ir_shutter_state = 0;
    shutter_state = 0;
}
void shutter_off_quick(void)
{
    SHUTTER_CLOSE;
    MIRROR_DOWN;
    ir_shutter_state = 0;
    shutter_state = 0;
}

/******************************************************************
 *
 *   shutter::half
 *
 *
 ******************************************************************/

void shutter::half(void)
{
    shutter_half();
}
void shutter_half(void)
{
    shutter_off(); // first we completely release the shutter button since some cameras need this to release the bulb

    if(conf.halfPress == HALF_PRESS_ENABLED) clock.in(30, &shutter_half_delayed);
}
void shutter_half_delayed(void)
{
    if(conf.devMode) 
        hardware_flashlight(0);
    
    SHUTTER_CLOSE;
    MIRROR_UP;
}

/******************************************************************
 *
 *   shutter::full
 *
 *
 ******************************************************************/

void shutter::full(void)
{
    shutter_full();
}
void shutter_full(void)
{
    if(conf.devMode) 
        hardware_flashlight(1);

    MIRROR_UP;
    SHUTTER_OPEN;
}

/******************************************************************
 *
 *   shutter::bulbStart
 *
 *
 ******************************************************************/

void shutter::bulbStart(void)
{
    shutter_bulbStart();
}
void shutter_bulbStart(void)
{
    camera.bulbMode();
    if((cable_connected == 0 || !(conf.interface & INTERFACE_CABLE)) && ir_shutter_state != 1)
    {
        if(camera.supports.bulb && (conf.interface & INTERFACE_USB))
        {
            camera.bulbStart();
        }
        else if(conf.interface & INTERFACE_IR)
        {
            ir_shutter_state = 1;
            ir.bulbStart();
        }
    }
    else
    {
        //if(camera.supports.capture) camera.busy = true;
    }

    if(conf.interface & INTERFACE_CABLE)
    {
        if(conf.bulbMode == 0)
        {
            shutter_full();
        } 
        else if(conf.bulbMode == 1 && shutter_state != 1)
        {
            shutter_full();
            shutter_state = 1;
            if(camera.ready)
                clock.in(SHUTTER_PRESS_TIME, &shutter_off);
            else
                clock.in(SHUTTER_PRESS_TIME, &shutter_half);
        }
    }
}

/******************************************************************
 *
 *   shutter::bulbEnd
 *
 *
 ******************************************************************/

void shutter::bulbEnd(void)
{
    shutter_bulbEnd();
}
void shutter_bulbEnd(void)
{
    debug(PSTR("Bulb End: "));
    if(camera.bulb_open || ir_shutter_state == 1)
    {
        if(camera.bulb_open && (conf.interface & INTERFACE_USB))
        {
            debug(PSTR("USB "));
            camera.bulbEnd();
        }
        else if(conf.interface & INTERFACE_IR)
        {
            debug(PSTR("IR "));
            ir.bulbEnd();
        }
        ir_shutter_state = 0;
    } 
    if(conf.interface & INTERFACE_CABLE)
    {
        if(conf.bulbMode == 0)
        {
            debug(PSTR("CABLE "));
            shutter_off();
        }
        else if(conf.bulbMode == 1 && shutter_state == 1)
        {
            shutter_full();
            shutter_state = 0;
            clock.in(SHUTTER_PRESS_TIME, &shutter_off);
        }
    }
    debug_nl();
}

/******************************************************************
 *
 *   shutter::capture
 *
 *
 ******************************************************************/

void shutter::capture(void)
{
    shutter_capture();
}
void shutter_capture(void)
{
    if(conf.interface & (INTERFACE_CABLE | INTERFACE_USB))
    {
        shutter_full();
        clock.in(SHUTTER_PRESS_TIME, &shutter_off);
        ir_shutter_state = 0;
        shutter_state = 0;
        if(cable_connected == 0)
        {
            if(camera.supports.capture)
            {
                camera.capture();
            }
            else
            {
                if(conf.interface & INTERFACE_IR) ir.shutterNow();
            }
        }
        else
        {
            if(camera.supports.capture) camera.busy = true;
        }
    }
    else if(conf.interface == INTERFACE_IR)
    {
        ir.shutterNow();
    }
}

/******************************************************************
 *
 *   shutter::cableIsConnected
 *
 *
 ******************************************************************/

char shutter::cableIsConnected(void)
{
    if(MIRROR_IS_DOWN)
    {
        CHECK_CABLE;
    }
    return cable_connected;
}

/******************************************************************
 *
 *   shutter::save
 *
 *
 ******************************************************************/

void shutter::save(char id)
{
    eeprom_write_block((const void*)&current, &stored[(uint8_t)id], sizeof(program));
    currentId = id;
}

void shutter::saveCurrent()
{
    eeprom_write_block((const void*)&current, &stored[MAX_STORED], sizeof(program));
}
void shutter::restoreCurrent()
{
    eeprom_read_block((void*)&current, &stored[MAX_STORED], sizeof(program));
}

/******************************************************************
 *
 *   shutter::load
 *
 *
 ******************************************************************/

void shutter::load(char id)
{
    eeprom_read_block((void*)&current, &stored[(uint8_t)id], sizeof(program));
    currentId = id;
}

/******************************************************************
 *
 *   shutter::nextId
 *
 *
 ******************************************************************/

int8_t shutter::nextId(void)
{
    int8_t id = -1, i;
    for(i = 1; i < MAX_STORED; i++)
    {
        if(eeprom_read_byte((uint8_t*)&stored[i].Name[0]) == 255)
        {
            id = i;
            break;
        }
    }
    return id;
}

/******************************************************************
 *
 *   shutter::begin
 *
 *
 ******************************************************************/

void shutter::begin()
{
    running = 1;
}

/******************************************************************
 *
 *   shutter::run
 *
 *
 ******************************************************************/

char shutter::task()
{
    char cancel = 0;
    static uint8_t enter, exps, run_state = RUN_DELAY, old_state = 255, preChecked = false;
    static int8_t evShift;
    static uint16_t photos;
    static uint32_t last_photo_end_ms;

    if(MIRROR_IS_DOWN)
    {
        CHECK_CABLE;
    }
    
    if(!running)
    {
        if(enter) 
            cancel = 1;
        else 
            return 0;
    }

    if(enter == 0 || preChecked) // Initialize variables and setup I/O pins
    {
        if(current.Mode & RAMP)
        {
            uint32_t tmp = (uint32_t)current.Duration * 10;
            tmp /= (uint32_t) current.Gap;
            current.Photos = (uint16_t) tmp;
        }

        ////////////////////////////// pre check ////////////////////////////////////
        if(camera.ready && camera.aperture() != camera.apertureWideOpen() && (current.Mode & TIMELAPSE) && !((current.Mode & RAMP) && (conf.brampMode & BRAMP_MODE_APERTURE)) )
        {
            if(!preChecked) menu.alert(STR_APERTURE_ALERT);
        }
        else
        {
            if(preChecked) menu.clearAlert(STR_APERTURE_ALERT);
        }
        if((current.Mode & RAMP) && camera.ready && !camera.supports.bulb && !cable_connected)
        {
            if(!preChecked) menu.alert(STR_BULBSUPPORT_ALERT);
        }
        else
        {
            if(preChecked) menu.clearAlert(STR_BULBSUPPORT_ALERT);
        }
        if((current.Mode & RAMP) && !camera.isInBulbMode())
        {
            if(!preChecked) menu.alert(STR_BULBMODE_ALERT);
        }
        else
        {
            if(preChecked) menu.clearAlert(STR_BULBMODE_ALERT);
        }
        if((current.Photos > 0) && camera.ready && camera.photosRemaining && camera.photosRemaining < current.Photos)
        {
            if(!preChecked) menu.alert(STR_MEMORYSPACE_ALERT);
        }
        else
        {
            if(preChecked) menu.clearAlert(STR_MEMORYSPACE_ALERT);
        }
        if((current.Mode & RAMP) && (current.brampMethod == BRAMP_METHOD_AUTO) && conf.devMode)
        {
            if(!preChecked) menu.alert(STR_DEVMODE_ALERT);
        }
        else
        {
            if(preChecked) menu.clearAlert(STR_DEVMODE_ALERT);
        }
        preChecked = true;
        if(menu.waitingAlert()) return 0; //////////////////////////////////////////////

        menu.message(TEXT("Loading"));
        menu.task();

        enter = 1;
        preChecked = false;


        if(current.Mode & RAMP)
        {
            calcBulbMax();
            status.rampMax = calcRampMax();
            status.rampMin = calcRampMin();
            rampRate = 0;
            status.rampStops = 0;
            internalRampStops = 0;
            light.integrationStart(current.Integration, current.NightSky);
            lightReading = lightStart = light.readIntegratedEv();
        }

        run_state = RUN_DELAY;
        clock.tare();
        photos = 0;
        exps = 0;

        if(camera.supports.aperture) aperture = camera.aperture();
        if(camera.supports.iso) iso = camera.iso();

        current.infinitePhotos = current.Photos == 0 ? 1 : 0;
        status.infinitePhotos = current.infinitePhotos;
        status.photosRemaining = current.Photos;
        status.photosTaken = 0;
        status.mode = (uint8_t) current.Mode;
        last_photo_end_ms = 0;
        last_photo_ms = 0;
        evShift = 0;

        ENABLE_MIRROR;
        ENABLE_SHUTTER;
        SHUTTER_CLOSE;
        MIRROR_DOWN;
    }

    /////// RUNNING PROCEDURE ///////
    if(run_state == RUN_DELAY)
    {
        if(old_state != run_state)
        {
            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_DELAY"));
                debug_nl();
            }
            strcpy((char *) status.textStatus, TEXT("Delay"));
            old_state = run_state;
        }


        if(((unsigned long)clock.event_ms / 1000) > current.Delay)
        {
            clock.tare();
            clock.reset();
            last_photo_ms = 0;
            run_state = RUN_PHOTO;
        } 
        else
        {
            status.nextPhoto = (unsigned int) (current.Delay - (unsigned long)clock.event_ms / 1000);
            if(((unsigned long)clock.event_ms / 1000) + settings_mirror_up_time >= current.Delay)
            {
                // Mirror Up //
                shutter_half(); // This is to wake up the camera (even if USB is connected)
            }

            if((settings_warn_time > 0) && (((unsigned long)clock.event_ms / 1000) + settings_warn_time >= current.Delay))
            {
                // Flash Light //
                _delay_ms(50);
            }
        }
    }

    if(run_state == RUN_PHOTO && (exps + photos == 0 || (uint8_t)((clock.Ms() - last_photo_end_ms) / 10) >= conf.cameraFPS))
    {
        last_photo_end_ms = 0;
        if(old_state != run_state)
        {
            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_PHOTO"));
                debug_nl();
            }
            strcpy((char *) status.textStatus, TEXT("Photo"));
            old_state = run_state;
        }
        if(current.Exp > 0 || (current.Mode & RAMP) || conf.arbitraryBulb)
        {
            clock.tare();
            run_state = RUN_BULB;
        } 
        else
        {
            exps++;
            capture();
            
            if(current.Gap <= settings_mirror_up_time && !camera.ready) 
                shutter_half(); // Mirror Up //

            run_state = RUN_NEXT;
        }
    }
    
    if(run_state == RUN_BULB)
    {
        static uint32_t bulb_length, exp;
        static uint8_t m = SHUTTER_MODE_BULB;

        if(old_state != run_state)
        {
            old_state = run_state;

            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_BULB"));
                debug_nl();
            }
            strcpy((char *) status.textStatus, TEXT("Bulb"));

            m = camera.shutterType(current.Exp);
            if(m & SHUTTER_MODE_BULB || conf.arbitraryBulb)
            {
                if(conf.arbitraryBulb)
                {
                    exp = current.ArbExp * 100;
                    m = SHUTTER_MODE_BULB;
                }
                else
                {
                    exp = camera.bulbTime((int8_t)current.Exp);
                }
            }

            if(current.Mode & RAMP)
            {
                float key1 = 1, key2 = 1, key3 = 1, key4 = 1;
                char found = 0;
                uint8_t i;
                m = SHUTTER_MODE_BULB;
                shutter_off();


                if(current.brampMethod == BRAMP_METHOD_KEYFRAME) //////////////////////////////// KEYFRAME RAMP /////////////////////////////////////
                {
                    // Bulb ramp algorithm goes here
                    for(i = 0; i < current.Keyframes; i++)
                    {
                        if(clock.Seconds() <= current.Key[i])
                        {
                            found = 1;
                            if(i == 0)
                            {
                                key2 = key1 = (float)(current.BulbStart);
                            }
                            else if(i == 1)
                            {
                                key1 = (float)(current.BulbStart);
                                key2 = (float)((int8_t)current.BulbStart - *((int8_t*)&current.Bulb[i - 1]));
                            }
                            else
                            {
                                key1 = (float)((int8_t)current.BulbStart - *((int8_t*)&current.Bulb[i - 2]));
                                key2 = (float)((int8_t)current.BulbStart - *((int8_t*)&current.Bulb[i - 1]));
                            }
                            key3 = (float)((int8_t)current.BulbStart - *((int8_t*)&current.Bulb[i]));
                            key4 = (float)((int8_t)current.BulbStart - *((int8_t*)&current.Bulb[i < (current.Keyframes - 1) ? i + 1 : i]));
                            break;
                        }
                    }
                    
                    if(found)
                    {
                        uint32_t var1 = clock.Seconds();
                        uint32_t var2 = (i > 0 ? current.Key[i - 1] : 0);
                        uint32_t var3 = current.Key[i];

                        float t = (float)(var1 - var2) / (float)(var3 - var2);
                        float curveEv = curve(key1, key2, key3, key4, t);
                        status.rampStops = (float)current.BulbStart - curveEv;
                        exp = camera.bulbTime(curveEv - (float)evShift);

                        if(conf.debugEnabled)
                        {
                            debug(PSTR("   Keyframe: "));
                            debug(i);
                            debug_nl();
                            debug(PSTR("    Percent: "));
                            debug(t);
                            debug_nl();
                            debug(PSTR("    CurveEv: "));
                            debug(curveEv);
                            debug_nl();
                            debug(PSTR("CorrectedEv: "));
                            debug(curveEv - (float)evShift);
                            debug_nl();
                            debug(PSTR("   Exp (ms): "));
                            debug(exp);
                            debug_nl();
                            debug(PSTR("    evShift: "));
                            debug(evShift);
                            debug_nl();
                        }
                    }
                    else
                    {
                        status.rampStops = (float)((int8_t)(current.BulbStart - (current.BulbStart - *((int8_t*)&current.Bulb[current.Keyframes - 1]))));
                        exp = camera.bulbTime((int8_t)(current.BulbStart - *((int8_t*)&current.Bulb[current.Keyframes - 1]) - (float)evShift));
                    }
                }
                else if(current.brampMethod == BRAMP_METHOD_GUIDED) //////////////////////////////// GUIDED RAMP /////////////////////////////////////
                {
                    status.rampStops += ((float)rampRate / 1800.0) * ((float)current.Gap / 10.0);
                    if(status.rampStops >= status.rampMax)
                    {
                        rampRate = 0;
                        status.rampStops = status.rampMax;
                    }
                    else if(status.rampStops <= status.rampMin)
                    {
                        rampRate = 0;
                        status.rampStops = status.rampMin;
                    }
                    exp = camera.bulbTime(current.BulbStart - status.rampStops - (float)evShift);
                }
                else if(current.brampMethod == BRAMP_METHOD_AUTO) //////////////////////////////// AUTO RAMP /////////////////////////////////////
                {
                    internalRampStops += ((float)rampRate / 1800.0) * ((float)current.Gap / 10.0);
                    status.rampStops = (lightStart - lightReading) + internalRampStops;
                    
                    if(status.rampStops > status.rampMax)
                    {
                        debug(PSTR("   (ramp max)\n"));
                        status.rampStops = status.rampMax;
                    }
                    else if(status.rampStops < status.rampMin)
                    {
                        debug(PSTR("   (ramp min)\n"));
                        status.rampStops = status.rampMin;
                    }
                    //                                   56        -     0            -    -6 
                    float tmp_ev = (float)current.BulbStart - status.rampStops - (float)evShift;
                    exp = camera.bulbTime(tmp_ev);

                    if(conf.debugEnabled)
                    {
                        debug(PSTR("     lightStart: "));
                        debug(lightStart);
                        debug_nl();
                        debug(PSTR("   lightReading: "));
                        debug(lightReading);
                        debug_nl();
                        debug(PSTR("  bulbStart Ev: "));
                        debug(current.BulbStart);
                        debug_nl();
                        debug(PSTR("   bulbTime Ev: "));
                        debug(tmp_ev);
                        debug_nl();
                        debug(PSTR("   Exp (ms): "));
                        debug(exp);
                        debug_nl();                        
                    }
                }

                bulb_length = exp;

                if(camera.supports.iso || camera.supports.aperture)
                {
                    uint8_t nextAperture = aperture;
                    uint8_t nextISO = iso;

                    int8_t tmpShift = 0;

                    if((conf.brampMode & BRAMP_MODE_APERTURE) && camera.supports.aperture)
                    {
                        // Check for too long bulb time and adjust Aperture //
                        while(bulb_length > BulbMax)
                        {
                            nextAperture = camera.apertureDown(aperture);
                            if(nextAperture != aperture)
                            {
                                evShift += nextAperture - aperture;
                                tmpShift += nextAperture - aperture;
                                aperture = nextAperture;

                                debug(PSTR("   Aperture UP:"));
                                debug(evShift);
                                debug(PSTR("   Aperture Val:"));
                                debug(nextAperture);
                            }
                            else
                            {
                                debug(PSTR("   Reached Aperture Max!!!\r\n"));
                                break;
                            }
                            bulb_length = camera.shiftBulb(exp, tmpShift);
                        }
                        debug(PSTR("Aperture Close: Done!\r\n\r\n"));
                    }

                    if((conf.brampMode & BRAMP_MODE_ISO) && camera.supports.iso)
                    {
                        // Check for too long bulb time and adjust ISO //
                        while(bulb_length > BulbMax)
                        {
                            nextISO = camera.isoUp(iso);
                            if(nextISO != iso)
                            {
                                evShift += nextISO - iso;
                                tmpShift += nextISO - iso;
                                iso = nextISO;

                                debug(PSTR("   ISO UP:"));
                                debug(evShift);
                                debug(PSTR("   ISO Val:"));
                                debug(nextISO);
                            }
                            else
                            {
                                debug(PSTR("   Reached ISO Max!!!\r\n"));
                                break;
                            }
                            bulb_length = camera.shiftBulb(exp, tmpShift);
                        }
                        debug(PSTR("ISO Up: Done!\r\n\r\n"));
                    }


                    if((conf.brampMode & BRAMP_MODE_ISO) && camera.supports.iso)
                    {
                        // Check for too short bulb time and adjust ISO //
                        for(;;)
                        {
                            nextISO = camera.isoDown(iso);
                            if(nextISO != iso && nextISO < 127)
                            {
                                uint16_t bulb_length_test = camera.shiftBulb(exp, tmpShift + (nextISO - iso));
                                if(bulb_length_test < BulbMax)
                                {
                                    evShift += nextISO - iso;
                                    tmpShift += nextISO - iso;
                                    iso = nextISO;
                                    debug(PSTR("   ISO DOWN:"));
                                    debug(evShift);
                                    debug(PSTR("   ISO Val:"));
                                    debug(nextISO);
                                    bulb_length = bulb_length_test;
                                    continue;
                                }
                            }
                            nextISO = iso;
                            break;
                        }
                        debug(PSTR("ISO Down: Done!\r\n\r\n"));
                    }


                    if((conf.brampMode & BRAMP_MODE_APERTURE) && camera.supports.aperture)
                    {
                        // Check for too short bulb time and adjust Aperture //
                        for(;;)
                        {
                            nextAperture = camera.apertureUp(aperture);
                            if(nextAperture != aperture && nextAperture < 127)
                            {
                                uint16_t bulb_length_test = camera.shiftBulb(exp, tmpShift + (nextAperture - aperture)); // two stops extra padding here
                                if(bulb_length_test < BulbMax)
                                {
                                    evShift  += nextAperture - aperture;
                                    tmpShift += nextAperture - aperture;
                                    aperture = nextAperture;
                                    debug(PSTR("Aperture DOWN:"));
                                    debug(evShift);
                                    debug(PSTR(" Aperture Val:"));
                                    debug(nextAperture);
                                    bulb_length = camera.shiftBulb(exp, tmpShift);
                                    continue;
                                }
                            }
                            nextAperture = aperture;
                            break;
                        }
                        debug(PSTR("Aperture Open: Done!\r\n\r\n"));
                    }
                    if(bulb_length < camera.bulbTime((int8_t)camera.bulbMin()))
                    {
                        debug(PSTR("   Reached Bulb Min!!!\r\n"));
                        bulb_length = camera.bulbTime((int8_t)camera.bulbMin());
                    }


                    shutter_off_quick(); // Can't change parameters when half-pressed
                    if(conf.brampMode & BRAMP_MODE_APERTURE)
                    {
                        // Change the Aperture //
                        if(camera.aperture() != nextAperture)
                        {
                            if(camera.setAperture(nextAperture) == PTP_RETURN_ERROR)
                            {
                                run_state = RUN_ERROR;
                                return CONTINUE;
                            }
                        }
                    }
                    if(conf.brampMode & BRAMP_MODE_ISO)
                    {
                        // Change the ISO //
                        if(camera.iso() != nextISO)
                        {
                            if(camera.setISO(nextISO) == PTP_RETURN_ERROR)
                            {
                                run_state = RUN_ERROR;
                                return CONTINUE;
                            }
                        }
                    }
                }
                
                if(conf.debugEnabled)
                {
                    debug(PSTR("   Seconds: "));
                    debug((uint16_t)clock.Seconds());
                    debug_nl();
                    debug(PSTR("   evShift: "));
                    debug(evShift);
                    debug_nl();
                    debug(PSTR("BulbLength: "));
                    debug((uint16_t)bulb_length);
                    if(found) debug(PSTR(" (calculated)"));
                    debug_nl();

                    /*
                    debug(PSTR("i: "));
                    debug(current.Bulb[i]);
                    debug_nl();
                    debug(PSTR("Key1: "));
                    debug((int16_t)key1);
                    debug_nl();
                    debug(PSTR("Key2: "));
                    debug((int16_t)key2);
                    debug_nl();
                    debug(PSTR("Key3: "));
                    debug((int16_t)key3);
                    debug_nl();
                    debug(PSTR("Key4: "));
                    debug((int16_t)key4);
                    debug_nl();
                    */
                }
            }

            if(current.Mode & HDR)
            {
                uint8_t tv_offset = ((current.Exps - 1) / 2) * current.Bracket - exps * current.Bracket;
                if(current.Mode & RAMP)
                {
                    bulb_length = camera.shiftBulb(bulb_length, tv_offset);
                }
                else
                {
                    shutter_off_quick(); // Can't change parameters when half-pressed
                    m = camera.shutterType(current.Exp - tv_offset);
                    if(m == 0) m = SHUTTER_MODE_PTP;

                    if(m & SHUTTER_MODE_PTP)
                    {
                        debug(PSTR("Shutter Mode PTP\r\n"));
                        camera.setShutter(current.Exp - tv_offset);
                        bulb_length = camera.bulbTime((int8_t)(current.Exp - tv_offset));
                    }
                    else
                    {
                        camera.bulbMode();
                        debug(PSTR("Shutter Mode BULB\r\n"));
                        m = SHUTTER_MODE_BULB;
                        bulb_length = camera.bulbTime((int8_t)(current.Exp - tv_offset));
                    }
                }

                if(conf.debugEnabled)
                {
                    debug_nl();
                    debug(PSTR("Mode: "));
                    debug(m);
                    debug_nl();
                    debug(PSTR("Tv: "));
                    debug(current.Exp - tv_offset);
                    debug_nl();
                    debug(PSTR("Bulb: "));
                    debug((uint16_t)bulb_length);
                    debug_nl();
                }
            }
            
            if((current.Mode & (HDR | RAMP)) == 0)
            {
                if(conf.debugEnabled)
                {
                    debug(PSTR("***Using exp: "));
                    debug(exp);
                    debug(PSTR(" ("));
                    debug(current.Exp);
                    debug(PSTR(")"));
                    debug_nl();
                }
                bulb_length = exp;
                if(m & SHUTTER_MODE_PTP)
                {
                    shutter_off_quick(); // Can't change parameters when half-pressed
                    camera.manualMode();
                    camera.setShutter(current.Exp);
                }
            }

            status.bulbLength = bulb_length;

            if(m & SHUTTER_MODE_BULB)
            {
                //debug(PSTR("Running BULB\r\n"));
                camera.bulb_open = true;
                clock.job(&shutter_bulbStart, &shutter_bulbEnd, bulb_length + conf.bulbOffset);
            }
            else
            {
                //debug(PSTR("Running Capture\r\n"));
                shutter_capture();
            }
        }
        else if(!clock.jobRunning && !camera.busy)
        {
            exps++;

            lightReading = light.readIntegratedEv();

            _delay_ms(50);

            if(current.Gap <= settings_mirror_up_time && !camera.ready) 
                shutter_half(); // Mirror Up //

            run_state = RUN_NEXT;
        }
        else
        {
            //if(clock.jobRunning) debug(PSTR("Waiting on clock  "));
            //if(camera.busy) debug(PSTR("Waiting on camera  "));
        }
    }
    
    if(run_state == RUN_NEXT)
    {
        last_photo_end_ms = clock.Ms();
        if(old_state != run_state)
        {
            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_NEXT"));
                debug_nl();
            }
            old_state = run_state;
        }

        if(camera.busy && !(photos >= current.Photos))
        {
            run_state = RUN_NEXT;
        }
        else if((exps >= current.Exps && (current.Mode & HDR)) || (current.Mode & HDR) == 0)
        {
            exps = 0;
            photos++;
            clock.tare();
            run_state = RUN_GAP;
        }
        else
        {
            clock.tare();
            run_state = RUN_PHOTO;
        }
    
        if(!current.infinitePhotos)
        {
            if(photos >= current.Photos || (((current.Mode & TIMELAPSE) == 0) && photos >= 1))
            {
                run_state = RUN_END;
            }
        }
        else
        {
            run_state = RUN_GAP;
        }

        status.photosRemaining = current.Photos - photos;
        status.photosTaken = photos;
    }
    
    if(run_state == RUN_GAP)
    {
        if(old_state != run_state)
        {
            if(run_state == RUN_GAP && conf.auxPort == AUX_MODE_DOLLY) aux_pulse();
            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_GAP"));
                debug_nl();
            }
            strcpy((char *) status.textStatus, TEXT("Waiting"));
            old_state = run_state;
        }
        uint32_t cms = clock.Ms();

        if((cms - last_photo_ms) / 100 >= current.Gap)
        {
            last_photo_ms = cms;
            clock.tare();
            run_state = RUN_PHOTO;
        } 
        else
        {
            status.nextPhoto = (unsigned int) ((current.Gap - (cms - last_photo_ms) / 100) / 10);
            if((cms - last_photo_ms) / 100 + (uint32_t)settings_mirror_up_time * 10 >= current.Gap)
            {
                // Mirror Up //
                if(!camera.ready) shutter_half(); // Don't do half-press if the camera is connected by USB
            }
        }
    }
    
    if(run_state == RUN_ERROR)
    {
        if(old_state != run_state)
        {
            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_ERROR"));
                debug_nl();
            }
            strcpy((char *) status.textStatus, TEXT("Error"));
            old_state = run_state;
        }

        //enter = 0;
        //running = 0;
        shutter_off();
        camera.bulbEnd();
        light.stop();

        hardware_flashlight((uint8_t) clock.Seconds() % 2);

        return CONTINUE;
    }

    if(run_state == RUN_END)
    {
        if(old_state != run_state)
        {
            if(conf.debugEnabled)
            {
                debug(PSTR("State: RUN_END"));
                debug_nl();
            }
            strcpy((char *) status.textStatus, TEXT("Done"));
            old_state = run_state;
        }

        enter = 0;
        running = 0;
        shutter_off();
        camera.bulbEnd();
        hardware_flashlight(0);
        light.stop();
        aux_off();
        clock.awake();

        return DONE;
    }

    /////////////////////////////////////////

    if(cancel)
    {
        run_state = RUN_END;
    }

    return CONTINUE;
}

void check_cable()
{
    CHECK_CABLE;
}

void aux_pulse()
{
    aux_on();
    if(conf.dollyPulse == 65535) conf.dollyPulse = 100;
    clock.in(conf.dollyPulse, &aux_off);
}

void aux_on()
{
    ENABLE_AUX_PORT;
    AUX_OUT1_ON;
    AUX_OUT2_ON;
}

void aux_off()
{
    ENABLE_AUX_PORT;
}

uint8_t stopName(char name[8], uint8_t stop)
{
    name[0] = ' ';
    name[1] = ' ';
    name[2] = ' ';
    name[3] = ' ';
    name[4] = ' ';
    name[5] = ' ';
    name[6] = ' ';
    name[7] = '\0';

    int8_t ev = *((int8_t*)&stop);

    uint8_t sign = 0;
    if(ev < 0)
    {
        sign = 1;
        ev = 0 - ev;
    }
    uint8_t mod = ev % 3;
    ev /= 3;
    if(mod == 0)
    {
        if(sign) name[5] = '-'; else name[5] = '+';
        if(ev > 9)
        {
            name[4] = name[5];
            name[5] = '0' + ev / 10;
            ev %= 10;
            name[6] = '0' + ev;
        }
        else
        {
            name[6] = '0' + ev;
        }
    }
    else
    {
        if(sign) name[ev > 0 ? 1 : 3] = '-'; else name[ev > 0 ? 1 : 3] = '+';
        if(ev > 9)
        {
            name[0] = name[1];
            name[1] = '0' + ev / 10;
            ev %= 10;
            name[2] = '0' + ev;
        }
        else if(ev > 0)
        {
            name[2] = '0' + ev;
        }
        name[4] = '0' + mod;
        name[5] = '/';
        name[6] = '3';
    }
    return 1;
}

void calcBulbMax()
{
    BulbMax = (timer.current.Gap / 10 - 5) * 1000;

    BulbMaxEv = 1;
    for(uint8_t i = camera.bulbMax(); i < camera.bulbMin(); i++)
    {
        if(BulbMax > camera.bulbTime((int8_t)i))
        {
            if(i < 1) i = 1;
            BulbMaxEv = i - 1;
            break;
        }
    }

    BulbMax = camera.bulbTime((int8_t)BulbMaxEv);

    debug(PSTR("BulbMax: "));
    debug(BulbMax);
    debug_nl();
}

uint8_t stopUp(uint8_t stop)
{
    int8_t ev = *((int8_t*)&stop);

    int8_t evMax = calcRampMax();

    if(ev < 30*3) ev++; else ev = 30*3;

    if(ev <= evMax) return ev; else return evMax;
}

uint8_t stopDown(uint8_t stop)
{
    int8_t ev = *((int8_t*)&stop);

    int8_t evMin = calcRampMin();

    if(ev > -30*3) ev--; else ev = -30*3;

    if(ev >= evMin) return ev; else return evMin;
}

int8_t calcRampMax()
{
    calcBulbMax();

    int8_t bulbRange = 0;
    int8_t isoRange = 0;
    int8_t apertureRange = 0;

    if(conf.brampMode & BRAMP_MODE_BULB) bulbRange = (int8_t)timer.current.BulbStart - (int8_t)BulbMaxEv;
    if(conf.brampMode & BRAMP_MODE_ISO) isoRange = (int8_t)camera.iso() - (int8_t)camera.isoMax();
    if(conf.brampMode & BRAMP_MODE_APERTURE) apertureRange = (int8_t)camera.aperture() - (int8_t)camera.apertureMin();

    return isoRange + apertureRange + bulbRange;
}

int8_t calcRampMin()
{
    int8_t bulbRange = 0;
    int8_t isoRange = 0;
    int8_t apertureRange = 0;

    if(conf.brampMode & BRAMP_MODE_BULB) bulbRange = (int8_t)camera.bulbMin() - (int8_t)timer.current.BulbStart;
    if((conf.brampMode & BRAMP_MODE_ISO) && camera.supports.iso) isoRange = (int8_t)camera.isoMin() - (int8_t)camera.iso();
    if((conf.brampMode & BRAMP_MODE_APERTURE) && camera.supports.aperture) apertureRange = (int8_t)camera.apertureMax() - (int8_t)camera.aperture();

    return 0 - (isoRange + apertureRange + bulbRange);
}

uint8_t checkHDR(uint8_t exps, uint8_t mid, uint8_t bracket)
{
    uint8_t up = mid - (exps / 2) * bracket;
    uint8_t down = mid + (exps / 2) * bracket;

    debug(PSTR("up: "));
    debug(up);
    debug_nl();
    debug(PSTR("down: "));
    debug(down);
    debug_nl();
    debug(PSTR("max: "));
    debug(camera.shutterMax());
    debug_nl();
    debug(PSTR("min: "));
    debug(camera.shutterMin());
    debug_nl();

    if(up < camera.shutterMax() || down > camera.shutterMin()) return 1; else return 0;
}

uint8_t hdrTvUp(uint8_t ev)
{
    if(checkHDR(timer.current.Exps, ev, timer.current.Bracket))
    {
        uint8_t mid = (camera.shutterMin() - camera.shutterMax()) / 2 + camera.shutterMax();
        if(mid > ev)
        {
            for(uint8_t i = ev; i <= mid; i++)
            {
                if(!checkHDR(timer.current.Exps, i, timer.current.Bracket))
                {
                    ev = i;
                    break;
                }
            }
        }
        else
        {
            for(uint8_t i = ev; i >= mid; i--)
            {
                if(!checkHDR(timer.current.Exps, i, timer.current.Bracket))
                {
                    ev = i;
                    break;
                }
            }
        }
    }
    else
    {
        uint8_t tmp = camera.shutterUp(ev);
        if(!checkHDR(timer.current.Exps, tmp, timer.current.Bracket)) ev = tmp;
    }
    return ev;
}

uint8_t hdrTvDown(uint8_t ev)
{
    if(checkHDR(timer.current.Exps, ev, timer.current.Bracket))
    {
        uint8_t mid = (camera.shutterMin() - camera.shutterMax()) / 2 + camera.shutterMax();
        if(mid > ev)
        {
            for(uint8_t i = ev; i <= mid; i++)
            {
                if(!checkHDR(timer.current.Exps, i, timer.current.Bracket))
                {
                    ev = i;
                    break;
                }
            }
        }
        else
        {
            for(uint8_t i = ev; i >= mid; i--)
            {
                if(!checkHDR(timer.current.Exps, i, timer.current.Bracket))
                {
                    ev = i;
                    break;
                }
            }
        }
    }
    else
    {
        uint8_t tmp = camera.shutterDown(ev);
        if(!checkHDR(timer.current.Exps, tmp, timer.current.Bracket)) ev = tmp;
    }
    return ev;
}

uint8_t bracketUp(uint8_t ev)
{
    uint8_t max = 1;
    for(uint8_t i = 1; i < 255; i++)
    {
        if(checkHDR(timer.current.Exps, timer.current.Exp, i))
        {
            max = i - 1;
            break;
        }
    }
    if(ev < max) ev++; else ev = max;
    return ev;
}

uint8_t bracketDown(uint8_t ev)
{
    if(ev > 1) ev--; else ev = 1;
    return ev;
}

uint8_t hdrExpsUp(uint8_t hdr_exps)
{
    uint8_t max = 1;
    for(uint8_t i = 1; i < 255; i++)
    {
        if(checkHDR(i, timer.current.Exp, timer.current.Bracket))
        {
            max = i - 1;
            if(max < 3) max = 3;
            break;
        }
    }
    if(hdr_exps < max) hdr_exps++; else hdr_exps = max;
    return hdr_exps;
}

uint8_t hdrExpsDown(uint8_t hdr_exps)
{
    if(hdr_exps > 3) hdr_exps--; else hdr_exps = 3;
    return hdr_exps;
}

uint8_t hdrExpsName(char name[8], uint8_t hdr_exps)
{
    name[0] = ' ';
    name[1] = ' ';
    name[2] = ' ';
    name[3] = ' ';
    name[4] = ' ';
    name[5] = ' ';
    name[6] = ' ';
    name[7] = '\0';

    if(hdr_exps > 9)
    {
        name[5] = '0' + (hdr_exps / 10);
        hdr_exps %= 10;
    }
    name[6] = '0' + hdr_exps;

    return 1;
}

uint8_t rampTvUp(uint8_t ev)
{
    uint8_t tmp = PTP::bulbUp(ev);
    if(tmp > camera.bulbMin()) tmp = camera.bulbMin();
    return tmp;
}

uint8_t rampTvUpStatic(uint8_t ev)
{
    uint8_t tmp = PTP::bulbUp(ev);
    //if(tmp > camera.bulbMin()) tmp = camera.bulbMin();
    return tmp;
}

uint8_t rampTvDown(uint8_t ev)
{
    calcBulbMax();
    uint8_t tmp = PTP::bulbDown(ev);
    if(tmp < BulbMaxEv) tmp = BulbMaxEv;
    return tmp;
}



