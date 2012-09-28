/*
 *  bluetooth.cpp
 *  Timelapse+
 *
 *  Created by Elijah Parker
 *  Copyright 2012 Timelapse+
 *  Licensed under GPLv3
 *
 */
 
#include <LUFA/Drivers/Peripheral/Serial.h>
#include <avr/io.h>
#include <stdlib.h>
#include <util/delay.h>
#include "tldefs.h"
#include "hardware.h"
#include "bluetooth.h"

/******************************************************************
 *
 *   BT::BT
 *
 *
 ******************************************************************/

BT::BT(void)
{
    // Configure Bluetooth //
    BT_INIT_IO;
    Serial_Init(115200, true);
}

/******************************************************************
 *
 *   BT::init
 *
 *
 ******************************************************************/

uint8_t BT::init(void)
{
    _delay_ms(100);
    present = true;

    read(); // Flush buffer

    send(STR("AT\r")); // Check connection
    
    if(checkOK() == 0)
    {
        present = false;
        return 0;
    }

    // Set Name //
    send(STR("ATSN,Timelapse+\r")); // Set Name

    if(checkOK() == 0) 
        return 0;

    send(STR("ATSZ,1,1,0\r")); // Configure Sleep mode (sleep on reset, wake on UART)

    if(checkOK() == 0) 
        return 0;

    send(STR("ATSDIF,782,1,1\r")); // Configure discovery display

    if(checkOK() == 0) 
        return 0;

    return power(3);
}

/******************************************************************
 *
 *   BT::power
 *
 *
 ******************************************************************/

uint8_t BT::power(void) 
{ 
    return btPower; 
}

/******************************************************************
 *
 *   BT::power
 *
 *
 ******************************************************************/

uint8_t BT::power(uint8_t level)
{
    if(!present) return 1;
    
    switch(level)
    {
       case 1:
            send(STR("ATSPL,1,1\r"));
            btPower = 1;
            break;
        
       case 2:
            send(STR("ATSPL,2,1\r"));
            btPower = 2;
            break;
            
       case 3:
            send(STR("ATSPL,3,1\r"));
            btPower = 3;
            break;
            
       default:
            send(STR("ATSPL,0,0\r"));
            btPower = 0;
            break;
    }

    return checkOK();
}

/******************************************************************
 *
 *   BT::cancel
 *
 *
 ******************************************************************/

uint8_t BT::cancel(void)
{
    if(!present) 
        return 1;
    
    send(STR("ATDC\r"));

    return checkOK();
}

/******************************************************************
 *
 *   BT::sleep
 *
 *
 ******************************************************************/

uint8_t BT::sleep(void)
{
    if(!present) 
        return 1;
    
    cancel();
    
    if(version() >= 3)
    {
        send(STR("ATZ\r"));

        return checkOK();
    } 
    else
    {
        return 1;
    }
}

/******************************************************************
 *
 *   BT::temperature
 *
 *
 ******************************************************************/

uint8_t BT::temperature(void)
{
    if(!present) 
        return 1;
    
    send(STR("ATT?\r"));

    uint8_t i = checkOK();
    uint8_t n = 0;
    
    if(i > 0)
    {
        for(++i; i < BT_DATA_SIZE; i++)
        {
            if(data[i] == 0) 
                break;
            
            if(data[i] == ',')
            {
                n = (data[i + 1] - '0') * 100;
                n += (data[i + 2] - '0') * 10;
                n += (data[i + 3] - '0');
                return n;
            }
        }
    }
    
    return 255;
}

/******************************************************************
 *
 *   BT::version
 *
 *
 ******************************************************************/

uint8_t BT::version(void)
{
    if(!present) 
        return 1;
    
    send(STR("ATV?\r"));
    
    uint8_t i = checkOK();
    uint8_t n = 0;
    
    if(i > 0)
    {
        for(++i; i < BT_DATA_SIZE; i++)
        {
            if(data[i] == 0) 
                break;
            
            if(i == 14)
            {
                n = (data[i] - '0');
                return n;
            }
        }
    }
    
    return 255;
}

/******************************************************************
 *
 *   BT::checkOK
 *
 *
 ******************************************************************/

uint8_t BT::checkOK(void)
{
    if(!present) 
        return 1;
    
    _delay_ms(50);
    uint8_t bytes = read();
    
    for(uint8_t i = 0; i < bytes; i++)
    {
        if(data[i] == 0) 
            break;
        
        if(data[i] == '\n' || data[i] == '\r') 
            continue; // SKIP CR/LF
        
        if(data[i] == 'O' && data[i + 1] == 'K')
        {
            return i;
        }
    }
    
    return 0;
}

/******************************************************************
 *
 *   BT::send
 *
 *
 ******************************************************************/

uint8_t BT::send(char *data)
{
    if(!present) 
        return 1;
    
    if(!(BT_RTS))
    {
        char i = 0;

        Serial_SendByte('A');
        
        while (!(BT_RTS))
        {
            if(++i > 250) 
                break;
            
            _delay_ms(1);
        }
    }

    if(BT_RTS)
    {
        char *ptr;
        
        ptr = data;
        
        while (*ptr != 0)
        {
            Serial_SendByte(*ptr);
            ptr++;
        }
    }
    // todo - No return value here. Defaulting to 0 -- John
    return 0;
}

/******************************************************************
 *
 *   BT::read
 *
 *
 ******************************************************************/

uint8_t BT::read(void)
{
    if(!present) 
        return 1;
    
    uint8_t bytes = 0;

    BT_SET_CTS;

    uint16_t timeout;
    
    for(;;)
    {
        timeout = 0;

        while(!Serial_IsCharReceived())
        {
            if(++timeout > 5000) 
                break;
        }
        
        if(timeout > 5000) 
            break;
        
        data[bytes] = (char)Serial_ReceiveByte();
        bytes++;
        
        if(bytes >= BT_DATA_SIZE) 
            break;
    }
    
    BT_CLR_CTS;

    data[bytes] = 0;
    
    return bytes;
}
