#include <driver/rtc_io.h>
#include "CC1101Lib.h"
#include "CC1101Device.h"
#include "SpiDevice.h"

namespace TI_CC1101
{
    CC1101Device::CC1101Device()
    {
    }

    CC1101Device::~CC1101Device()
    {
    }

    void CC1101Device::Init(std::shared_ptr<SpiDevice> spiDevice)
    {
        m_spiDevice = spiDevice;
    }

    void CC1101Device::Reset()
    {
         // See page 51, automatic POR (power-on reset)
        //
        /* * Set SCLK = 1 and SI = 0, to avoid potential problems with pin control mode (see Section 11.3).
            * Strobe CSn low / high.
            * Hold CSn low and then high for at least 40µs relative to pulling CSn low
            * Pull CSn low and wait for SO to go low (CHIP_RDYn).
            * Issue the SRES strobe on the SI line.
            * When SO goes low again, reset is complete and the chip is in the IDLE state.
        */
        bool bRet = true;

        CERA(gpio_set_level(m_spiDevice->ClockPin(),1));
        CERA(gpio_set_level(m_spiDevice->MosiPin(),0));

        CERA(gpio_set_level(m_spiDevice->ChipSelectPin(),0));
        delayMicroseconds(5);
        CERA(gpio_set_level(m_spiDevice->ChipSelectPin(),1));
        delayMicroseconds(5);
        CERA(gpio_set_level(m_spiDevice->ChipSelectPin(),0));
        delayMicroseconds(40);
        CERA(gpio_set_level(m_spiDevice->ChipSelectPin(),1));

        CBRA(m_spiDevice->WriteByte(CC1101_CONFIG::SRES));

        waitForMisoLow();

    Error:
        if(!bRet)
        {
            //reset failed
        }
    }

    void CC1101Device::SetFrequency(float frequencyMHz)
    {
    }

    void CC1101Device::SetReceiveChannelFilterBandwidth(float bandwidthKHz)
    {
    }

    void CC1101Device::SetModemDeviation(float deviationKHz)
    {
    }

    void CC1101Device::SetOutputPower(int outputPower)
    {
    }

    void CC1101Device::SetModulation(ModulationType modulationType)
    {
    }

    void CC1101Device::SetManchesterEncoding(bool shouldEnable)
    {
    }

    void CC1101Device::waitForMisoLow()
    {
        while(gpio_get_level(m_spiDevice->MisoPin()) == 1)
            ; //busy wait
    }

    void CC1101Device::delayMilliseconds(int millis)
    {
        vTaskDelay(millis/portTICK_PERIOD_MS);
    }

    void CC1101Device::delayMicroseconds(int micros)
    {
        vTaskDelay(micros/1000/portTICK_PERIOD_MS);
    }

} // namespace TI_CC1101