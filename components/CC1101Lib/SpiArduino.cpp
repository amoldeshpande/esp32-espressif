// Copyright (C) 2024 Amol Deshpande
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
// 
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
#include <SPI.h>
#include "CC1101Lib.h"
#include "SpiMaster.h"

static const char *TAG = "SpiMaster";

namespace TI_CC1101
{

SpiMaster::SpiMaster()
{
}

SpiMaster::~SpiMaster()
{
}

bool SpiMaster::Init(const SpiConfig &cfg)
{
    bool bRet = true;
    esp_err_t ret;

    m_config = cfg;
    startTransaction();

    digitalWrite(cfg.chipSelectPin,HIGH);
    digitalWrite(cfg.clockPin,HIGH);
    digitalWrite(cfg.mosiPin,LOW);

    endTransaction();

    return true;
}
bool SpiMaster::WriteByte(byte toWrite, byte& outData)
{
    startTransaction();

    lowerChipSelect();
    waitForMisoLow();
    outData = SPI.transfer(toWrite);
    raiseChipSelect();
    endTransaction();

    return true;
}

bool SpiMaster::WriteByteToAddress(byte address, byte value, byte&  outData)
{
    startTransaction();
    lowerChipSelect();
    waitForMisoLow();

    outData  = SPI.transfer(address);
    outData  = SPI.transfer(value);

    raiseChipSelect();
    endTransaction();

    return true;
}
bool SpiMaster::WriteBytesToAddress(byte address,byte *toWrite, size_t arrayLen, byte& outData)
{
    startTransaction();
    lowerChipSelect();
    waitForMisoLow();

    SPI.transfer(address);
    for(size_t si= 0; si< arrayLen;si++)
    {
        outData = SPI.transfer(toWrite[si]);
    }

    raiseChipSelect();
    endTransaction();

    return true;
}
bool SpiMaster::ReadBurstRegister(byte address, byte *toRead, size_t arrayLen)
{
    startTransaction();
    lowerChipSelect();
    waitForMisoLow();

    SPI.transfer(address);
    for(size_t si = 0; si < arrayLen; si++)
    {
        toRead[si] = SPI.transfer(0);
    }
    raiseChipSelect();

    endTransaction();

    return true;
}
bool SpiMaster::ReadRegister(byte addr, byte& outData)
{
    byte ignore = 0;
    startTransaction();
    lowerChipSelect();
    waitForMisoLow();

    ignore = SPI.transfer(addr);
    outData = SPI.transfer(0);

    raiseChipSelect();
    endTransaction();
    return true;
}
void SpiMaster::startTransaction()
{
    pinMode(m_config.mosiPin,OUTPUT);
    pinMode(m_config.misoPin,INPUT);
    pinMode(m_config.clockPin,OUTPUT);
    pinMode(m_config.chipSelectPin,OUTPUT);

    SPI.begin(m_config.clockPin,m_config.misoPin,m_config.mosiPin,m_config.chipSelectPin);
}
void SpiMaster::endTransaction()
{
    SPI.end();
}
void SpiMaster::lowerChipSelect()
{
    digitalWrite(m_config.chipSelectPin, 0);

}

void SpiMaster::raiseChipSelect()
{
    digitalWrite(m_config.chipSelectPin, 1);
}
void SpiMaster::waitForMisoLow()
{
    while(digitalRead(m_config.misoPin))
        delayMicroseconds(1);
}

} // namespace