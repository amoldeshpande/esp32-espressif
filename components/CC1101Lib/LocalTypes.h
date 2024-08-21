#pragma once
#include <stdint.h>

namespace TI_CC1101
{
    typedef uint8_t byte;


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
		assert(exp);
		goto Error; \
	}\
}
#define CBRA(exp)        {\
	if(!(exp)) {\
		bRet = false; \
		assert(exp);
		goto Error; \
	}\
}
#else
#define CERA CER
#define CBRA CBR
#endif
}