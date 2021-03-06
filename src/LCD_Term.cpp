/*
 *  LCD_Term.cpp
 *  Timelapse+
 *
 *  Created by Elijah Parker
 *  Copyright 2012 Timelapse+
 *	Licensed under GPLv3
 *
 */

#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/io.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "timelapseplus.h"
#include "LCD_Term.h"
#include "5110LCD.h"
#include "tlp_menu_functions.h"

extern LCD lcd;

uint8_t x, y;

/******************************************************************
 *
 *   termInit
 *
 *
 ******************************************************************/

void termInit()
{
    lcd.init(0x8);
    termClear();
}

/******************************************************************
 *
 *   termPrintChar
 *
 *
 ******************************************************************/

void termPrintChar(char c)
{
    if(c == 13 || c == 10)
    {
        y += TERM_LINE_HEIGHT;
        x = 1;
    } 
    else
    {
        x += lcd.writeCharTiny(x, y, c) + 1;
        
        if(x > TERM_WIDTH)
        {
            y += TERM_LINE_HEIGHT;
            x = 1;
        }
    }
    
    if(y > TERM_HEIGHT)
    {
        char j, i;
        
        for(i = 0; i < LCD_WIDTH; i++)
        {
            for(j = 0; j < LCD_HEIGHT - TERM_LINE_HEIGHT; j++)
            {
                if(lcd.getPixel(i, j + TERM_LINE_HEIGHT)) 
                    lcd.setPixel(i, j);
                else 
                    lcd.clearPixel(i, j);
            }
            
            for(j = LCD_HEIGHT - TERM_LINE_HEIGHT; j < LCD_HEIGHT; j++)
            {
                lcd.clearPixel(i, j);
            }
        }
        
        y -= TERM_LINE_HEIGHT;
    }
}

/******************************************************************
 *
 *   termPrintByte
 *
 *
 ******************************************************************/

void termPrintByte(uint8_t c)
{
    char buf[5];
    uint8_t i = 0;

    int_to_str((uint16_t)c, buf);
    
    while(buf[i])
    {
        termPrintChar(buf[i]);
        i++;
    }
}

/******************************************************************
 *
 *   termPrintStr
 *
 *
 ******************************************************************/

void termPrintStrP(const char *s)
{
    char c;
    c = pgm_read_byte(s);
    while (c != 0)
    {
        termPrintChar(c);
        s++;
        c = pgm_read_byte(s);
    }
    
    lcd.update();
}

/******************************************************************
 *
 *   termClear
 *
 *
 ******************************************************************/

void termClear()
{
    lcd.cls();
    x = 1;
    y = 1;
    lcd.update();
}

