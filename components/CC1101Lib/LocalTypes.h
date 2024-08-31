#pragma once
#include <stdint.h>
#include <esp_log.h>

#ifndef NDEBUG 
#define _DEBUG 
#endif
namespace TI_CC1101
{
    typedef uint8_t byte;

#define ARRAYSIZE(a) ((sizeof(a) / sizeof(*(a))) / \
                     static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))

#define CER(exp)        {\
	if((exp) != ESP_OK) {\
		bRet = false; \
		goto Error; \
	}\
}
#define CBR(exp)        {\
	if(!(exp)) {\
		bRet = false; \
		goto Error; \
	}\
}
#if defined(_DEBUG)
#define CERA(exp)        {\
	if((exp) != ESP_OK) {\
		bRet = false; \
		assert(exp); \
		goto Error; \
	}\
}
#define CBRA(exp)        {\
	if(!(exp)) {\
		bRet = false; \
		assert(exp); \
		goto Error; \
	}\
}
#else
#define CERA CER
#define CBRA CBR
#endif
}