#pragma once
#ifndef ARDUINO
#include <esp_log.h>
#else
#include <ArduinoLog.h>
#include <stdio.h>
#include <stdarg.h>
#endif
#include <stdint.h>

#ifndef NDEBUG
#define _DEBUG 1
#endif

#ifdef ARDUINO
#define ESP_LOGD(tg,fmt,...)    Log.traceln(fmt __VA_OPT__(,) __VA_ARGS__)
#define ESP_LOGI(tg,fmt,...)    Log.noticeln(fmt __VA_OPT__(,) __VA_ARGS__)
#define ESP_LOGW(tg,fmt,...)    Log.warningln(fmt __VA_OPT__(,) __VA_ARGS__) 
#define ESP_LOGE(tg,fmt,...)    Log.errorln(fmt __VA_OPT__(,) __VA_ARGS__) 

#define gpio_num_t int // typedef will conflict with esp_32 header included somewhere deep in Arduino.h
#define IRAM_ATTR

//non-standard format specifications in Arduino-Log library
#define FLOAT_FMT "%F"
#define HEX_FMT "%X"
#else
#define delayMicroseconds(micros) { esp_rom_delay_us(micros); }
#define FLOAT_FMT "%g"
#define HEX_FMT "0x%X"
#endif
namespace TI_CC1101
{
    typedef uint8_t byte;

#define ARRAYSIZE(a) ((sizeof(a) / sizeof(*(a))) / \
                      static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))

#define CER(exp)             \
    {                        \
        if ((exp) != ESP_OK) \
        {                    \
            bRet = false;    \
            goto Error;      \
        }                    \
    }
#define CBR(exp)          \
    {                     \
        if (!(exp))       \
        {                 \
            bRet = false; \
            goto Error;   \
        }                 \
    }
#if defined(_DEBUG)
#define CERA(exp)                          \
    {                                      \
        if ((exp) != ESP_OK)               \
        {                                  \
            bRet = false;                  \
            ESP_LOGE(TAG, #exp " failed"); \
            assert(bRet);                  \
            goto Error;                    \
        }                                  \
    }
#define CBRA(exp)         \
    {                     \
        if (!(exp))       \
        {                 \
            bRet = false; \
            ESP_LOGE(TAG, #exp " failed"); \
            assert(bRet); \
            goto Error;   \
        }                 \
    }
#else
#define CERA CER
#define CBRA CBR
#endif
} // namespace TI_CC1101