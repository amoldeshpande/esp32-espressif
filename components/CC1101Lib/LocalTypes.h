#pragma once
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <stdint.h>

#ifndef NDEBUG
#define _DEBUG 1
#endif

#define delayMicroseconds(micros) { esp_rom_delay_us(micros); }
#define FLOAT_FMT "%g"
#define HEX_FMT "0x%X"
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
