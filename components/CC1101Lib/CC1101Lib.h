#pragma once
#include <stdint.h>
namespace TI_CC1101
{
    // Various configuration elements from the Texas Instruments CC1101 datasheet
    // https://www.ti.com/lit/ds/symlink/cc1101.pdf?ts=1704109305538

    typedef uint8_t byte;
    namespace CC1101_CONFIG
    {
        // Table 42 - Command Strobes
        const byte SRES    = 0x30;// Reset chip.
        const byte SFSTXON = 0x31;// Enable and calibrate frequency synthesizer (if MCSM0.FS_AUTOCAL=1).  
                                  // If in RX (with CCA): Go to a wait state where only the synthesizer is running(for quick RX / TX turnaround). 
        const byte SXOFF   = 0x32;// Turn off crystal oscillator.
        const byte SCAL    = 0x33;// Calibrate frequency synthesizer and turn it off. SCAL can be strobed from IDLE mode without setting manual calibration mode(MCSM0.FS_AUTOCAL= 0)
        const byte SRX     = 0x34;// Enable RX. Perform calibration first if coming from IDLE and MCSM0.FS_AUTOCAL=1.
        const byte STX     = 0x35;// In IDLE state: Enable TX. Perform calibration first if MCSM0.FS_AUTOCAL=1.
                                  // If in RX state and CCA is enabled: Only go to TX if channel is clear.
        const byte SIDLE   = 0x36;// Exit RX / TX, turn off frequency synthesizer and exit Wake-On-Radio mode if applicable.
        const byte SAFC    = 0x37;// Perform AFC adjustment of the frequency synthesizer
        const byte SWOR    = 0x38;// Start automatic RX polling sequence (Wake-on-Radio) as described in Section 19.5 if WORCTRL.RC_PD=0.
        const byte SPWD    = 0x39;// Enter power down mode when CSn goes high.
        const byte SFRX    = 0x3A;// Flush the RX FIFO buffer. Only issue SFRX in IDLE or RXFIFO_OVERFLOW states
        const byte SFTX    = 0x3B;// Flush the TX FIFO buffer. Only issue SFTX in IDLE or TXFIFO_UNDERFLOW states.
        const byte SWORRST = 0x3C;// Reset real time clock to Event1 value.
        const byte SNOP    = 0x3D;// No operation. May be used to get access to the chip status byte.

        // Table 43 - Configuration Registers
        const byte IOCFG2     = 0x00;// GDO2 output pin configuration
        const byte IOCFG1     = 0x01;// GDO1 output pin configuration
        const byte IOCFG0     = 0x02;// GDO0 output pin configuration
        const byte FIFOTHR    = 0x03;// RX FIFO and TX FIFO thresholds
        const byte SYNC1      = 0x04;// Sync word, high byte
        const byte SYNC0      = 0x05;// Sync word, low byte
        const byte PKTLEN     = 0x06;// Packet length
        const byte PKTCTRL1   = 0x07;// Packet automation control
        const byte PKTCTRL0   = 0x08;// Packet automation control
        const byte ADDR       = 0x09;// Device address
        const byte CHANNR     = 0x0A;// Channel number
        const byte FSCTRL1    = 0x0B;// Frequency synthesizer control
        const byte FSCTRL0    = 0x0C;// Frequency synthesizer control
        const byte FREQ2      = 0x0D;// Frequency control word, high byte
        const byte FREQ1      = 0x0E;// Frequency control word, middle byte
        const byte FREQ0      = 0x0F;// Frequency control word, low byte
        const byte MDMCFG4    = 0x10;// Modem configuration
        const byte MDMCFG3    = 0x11;// Modem configuration
        const byte MDMCFG2    = 0x12;// Modem configuration
        const byte MDMCFG1    = 0x13;// Modem configuration
        const byte MDMCFG0    = 0x14;// Modem configuration
        const byte DEVIATN    = 0x15;// Modem deviation setting
        const byte MCSM2      = 0x16;// Main Radio Control State Machine configuration
        const byte MCSM1      = 0x17;// Main Radio Control State Machine configuration
        const byte MCSM0      = 0x18;// Main Radio Control State Machine configuration
        const byte FOCCFG     = 0x19;// Frequency Offset Compensation configuration
        const byte BSCFG      = 0x1A;// Bit Synchronization configuration
        const byte AGCCTRL2   = 0x1B;// AGC control
        const byte AGCCTRL1   = 0x1C;// AGC control
        const byte AGCCTRL0   = 0x1D;// AGC control
        const byte WOREVT1    = 0x1E;// High byte Event 0 timeout
        const byte WOREVT0    = 0x1F;// Low byte Event 0 timeout
        const byte WORCTRL    = 0x20;// Wake On Radio control
        const byte FREND1     = 0x21;// Front end RX configuration
        const byte FREND0     = 0x22;// Front end TX configuration
        const byte FSCAL3     = 0x23;// Frequency synthesizer calibration
        const byte FSCAL2     = 0x24;// Frequency synthesizer calibration
        const byte FSCAL1     = 0x25;// Frequency synthesizer calibration
        const byte FSCAL0     = 0x26;// Frequency synthesizer calibration
        const byte RCCTRL1    = 0x27;// RC oscillator configuration
        const byte RCCTRL0    = 0x28;// RC oscillator configuration
        const byte FSTEST     = 0x29;// Frequency synthesizer calibration control
        const byte PTEST      = 0x2A;// Production test
        const byte AGCTEST    = 0x2B; // AGC test
        const byte TEST2      = 0x2C; // Various test settings
        const byte TEST1      = 0x2D;// Various test settings
        const byte TEST0      = 0x2E;// Various test settings

        //Table 44:Status Registers
        const byte PARTNUM        = 0x30; // Part number for CC1101
        const byte VERSION        = 0x31; // Current version number
        const byte FREQEST        = 0x32; // Frequency Offset Estimate
        const byte LQI            = 0x33; // Demodulator estimate for Link Quality
        const byte RSSI           = 0x34; // Received signal strength indication
        const byte MARCSTATE      = 0x35; // Control state machine state
        const byte WORTIME1       = 0x36; // High byte of WOR timer
        const byte WORTIME0       = 0x37; // Low byte of WOR timer
        const byte PKTSTATUS      = 0x38; // Current GDOx status and packet status
        const byte VCO_VC_DAC     = 0x39; // Current setting from PLL calibration module
        const byte TXBYTES        = 0x3A; // Underflow and number of bytes in the TX FIFO
        const byte RXBYTES        = 0x3B; // Overflow and number of bytes in the RX FIFO
        const byte RCCTRL1_STATUS = 0x3C; // Last RC oscillator calibration result
        const byte RCCTRL0_STATUS = 0x3D; // Last RC oscillator calibration result
    }

  namespace ConfigValues
  {
      // Table 41 GDOx Signal Selection (x = 0,1, or 2)
      enum class GDx_CFG_LowerSixBits : byte
      {
          RX_FIFO_BELOW_THRESHOLD                       = 0x00,//Associated to the RX FIFO: Asserts when RX FIFO is filled at or above the RX FIFO threshold. De-asserts when RX FIFO is drained below the same threshold. 
          RX_FIFO_ABOVE_THRESHOLD                       = 0x01,//Associated to the RX FIFO: Asserts when RX FIFO is filled at or above the RX FIFO threshold or the end of packet is reached. De-asserts when the RX FIFO is empty
          TX_FIFO_BELOW_THRESHOLD                       = 0x02,//Associated to the TX FIFO: Asserts when the TX FIFO is filled at or above the TX FIFO threshold.De-asserts when the TX FIFO is below the same threshold.
          TX_FIFO_ABOVE_THRESHOLD                       = 0x03,//Associated to the TX FIFO: Asserts when TX FIFO is full.De-asserts when the TX FIFO is drained below the TX FIFO threshold.
          RX_FIFO_OVERFLOW                              = 0x04,//Asserts when the RX FIFO has overflowed.De-asserts when the FIFO has been flushed.
          TX_FIFO_OVERFLOW                              = 0x05,//Asserts when the TX FIFO has underflowed.De-asserts when the FIFO is flushed.
          SYNC_WORD_OR_RX_PKT_DISCARDED_OR_TX_UNDERFLOW = 0x06,// Asserts when sync word has been sent / received, and de-asserts at the end of the packet.In RX, the pin will also deassert when a packet is discarded due to address or maximum length filtering or when the radio enters
                                                               //  RXFIFO_OVERFLOW state.In TX the pin will de-assert if the TX FIFO underflows.
          CRC_OK_RECVD                                  = 0x07,// Asserts when a packet has been received with CRC OK.De-asserts when the first byte is read from the RX FIFO.
          PREAMBLE_QUALITY_REACHED                      = 0x08,//Preamble Quality Reached.Asserts when the PQI is above the programmed PQT value.De-asserted when the chip reenters RX state(MARCSTATE= 0x0D) or the PQI gets below the programmed PQT value.
          CLEAR_CHANNEL_ASSESSMENT                      = 0x09,//Clear channel assessment.High when RSSI level is below threshold(dependent on the current CCA_MODE setting).
          LOCK_DETECTOR_OUTPUT                          = 0x0A,//Lock detector output.The PLL is in lock if the lock detector output has a positive transition or is constantly logic high.To
                                                               //check for PLL lock the lock detector output should be used as an interrupt for the MCU.
          SERIAL_CLOCK                                  = 0x0B,//Serial Clock.Synchronous to the data in synchronous serial mode.
                                                               //  In RX mode, data is set up on the falling edge by CC1101 when GDOx_INV = 0.
                                                               //  In TX mode, data is sampled by CC1101 on the rising edge of the serial clock when GDOx_INV= 0.
          SERIAL_SYNCHRONOUS_DATA_OUTPUT                = 0x0C,//Serial Synchronous Data Output.Used for synchronous serial mode.
          SERIAL_DATA_OUTPUT                            = 0x0D,//Serial Data Output.Used for asynchronous serial mode.
          CARRIER_SENSE                                 = 0x0E,//Carrier sense.High if RSSI level is above threshold. Cleared when entering IDLE mode.
          CRC_OK                                        = 0x0F,//CRC_OK.The last CRC comparison matched.Cleared when entering/restarting RX mode.
          // 16 (0x10) to 21 (0x15) Reserved – used for test 
          RX_HARD_DATA_1                                = 0x16,// RX_HARD_DATA[1]. Can be used together with RX_SYMBOL_TICK for alternative serial RX output.
          RX_HARD_DATA_0                                = 0x17,// RX_HARD_DATA[0]. Can be used together with RX_SYMBOL_TICK for alternative serial RX output.
          //24 (0x18) to 26 (0x1A) Reserved – used for test
          PA_PD                                         = 0x1B,//PA_PD.Note: PA_PD will have the same signal level in SLEEP and TX states.To control an external PA or RX/TX switch
                                                               //in applications where the SLEEP state is used it is recommended to use GDOx_CFGx = 0x2F instead.
          LNA_PD                                        = 0x1C,// LNA_PD.Note: LNA_PD will have the same signal level in SLEEP and RX states.To control an external LNA or RX/TX switch
                                                               // in applications where the SLEEP state is used it is recommended to use GDOx_CFGx = 0x2F instead.
          RX_SYMBOL_TICK                                = 0x1D,// RX_SYMBOL_TICK.Can be used together with RX_HARD_DATA for alternative serial RX output.
          //30 (0x1E) to 35 (0x23) Reserved – used for test
          WOR_EVNT0                                     = 0x24,//WOR_EVNT0
          WOR_EVNT1                                     = 0x25,//WOR_EVNT1
          CLK_256                                       = 0x26,//CLK_256
          CLK_32k                                       = 0x27,//CLK_32k
          //40 (0x28) Reserved – used for test
          CHIP_RDY_N                                    = 0x29,// CHIP_RDYn
          //42 (0x2A) Reserved – used for test
          XOSC_STABLE                                   = 0x2B,//XOSC_STABLE
          //44 (0x2C) Reserved – used for test
          //45 (0x2D) Reserved – used for test
          HIGH_IMPEDANCE_3_STATE                        = 0x2E,//High impedance(3-state)
          HW_TO_0                                       = 0x2F,//HW to 0 (HW1 achieved by setting GDOx_INV = 1). Can be used to control an external LNA/PA or RX/TX switch.
          CLK_XOSC_1                                    = 0x30,// CLK_XOSC/1
          //Note: There are 3 GDO pins, but only one CLK_XOSC/n can be selected as an output at any
          //time.If CLK_XOSC/n is to be monitored on one of the GDO pins, the other two GDO pins must
          //be configured to values less than 0x30. The GDO0 default value is CLK_XOSC/192.
          //To optimize RF performance, these signals should not be used while the radio is in RX or TX
          //mode.
          CLK_XOSC_BY_1_5                               = 0x31,//CLK_XOSC/1.5
          CLK_XOSC_BY_2                                 = 0x32,//CLK_XOSC/2
          CLK_XOSC_BY_3                                 = 0x33,//CLK_XOSC/3
          CLK_XOSC_BY_4                                 = 0x33,//CLK_XOSC/4
          CLK_XOSC_BY_6                                 = 0x35,//CLK_XOSC/6
          CLK_XOSC_BY_8                                 = 0x36,//CLK_XOSC/8
          CLK_XOSC_BY_12                                = 0x37,//CLK_XOSC/12
          CLK_XOSC_BY_16                                = 0x38,//CLK_XOSC/16
          CLK_XOSC_BY_24                                = 0x39,//CLK_XOSC/24
          CLK_XOSC_BY_32                                = 0x3A,//CLK_XOSC/32
          CLK_XOSC_BY_48                                = 0x3B,//CLK_XOSC/48
          CLK_XOSC_BY_64                                = 0x3C,//CLK_XOSC/64
          CLK_XOSC_BY_96                                = 0x3D,//CLK_XOSC/96
          CLK_XOSC_BY_128                               = 0x3E,//CLK_XOSC/128
          CLK_XOSC_BY_192                               = 0x3F,//CLK_XOSC/192
      };

      // values in bits 4 and 5 of PKTCTRL0 register (Page 74)
      enum class PKTCTRL0_PKT_FORMAT : byte // Format of RX and TX data
      {
          Normal                = 0,//Normal mode, use FIFOs for RX and TX
          SynchronousSerialMode = 1,//Synchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins
          RandomTxMode          = 2,// Random TX mode; sends random data using PN9 generator. Used for test.  Works as normal mode, setting 0 (00), in RX
          AsyncSerialMode       = 3,//Asynchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins
      };
      // values in bits 0 and 1 of PKTCTRL0 register (Page 74)
      enum class PKTCTRL0_LENGTH_CONFIG : byte //Packet length configuration
      {
          Fixed    = 0,//Fixed packet length mode. Length configured in PKTLEN register
          Variable = 1,//Variable packet length mode. Packet length configured by the first byte after sync word
          Infinite = 2,//Infinite packet length mode
          Reserved = 3,//Reserved
      };

      // Once again, we're bitten by nanoFramework's lack of Dictionary
      // Below correspond to power levels of -30,-20,-15,-10,0,5,7,10
      // Multi-layer inductors are used in the OEM reference design for 315 and 433 MHz
      const byte PATABLE_315_SETTINGS[] = { 0x12, 0x0D, 0x1C, 0x34, 0x51, 0x85, 0xCB, 0xC2 };
      const byte PATABLE_433_SETTINGS[] = { 0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0 };
      // Below correspond to power levels of -30,-20,-15,-10,-6,0,5,7,10,11
      // Wire-wound inductors are used in the OEM reference design for 868 and 915 MHz
      const byte PATABLE_868_SETTINGS[] = { 0x03, 0x17, 0x1D, 0x26, 0x37, 0x50, 0x86, 0xCD, 0xC5, 0xC0 };
      const byte PATABLE_915_SETTINGS[] = { 0x03, 0x0E, 0x1E, 0x27, 0x38, 0x8E, 0x84, 0xCC, 0xC3, 0xC0 };

      enum class PATables { PA_315, PA_433, PA_868, PA_915 };
  }

  enum class ModulationType
  {
      FSK_2     = 0, // 2FSK
      GFSK      = 1,
      INVALID_2 = 2,
      ASK_OOK   = 3,
      FSK_4     = 4, // 4FSK
      INVALID_5 = 5,
      INVALID_6 = 6,
      MSK       = 7
  };
  enum class PacketFormat
  {
      Normal       = 0, // Normal mode, use FIFOs for RX and TX
      Synchronous  = 1, // Synchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins
      RandomTX     = 2, // Random TX mode; sends random data using PN9 generator. Used for test.  Works as normal mode,
                    // setting 0 (00), in RX
      Asynchronous = 3, // Asynchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins
  };
  enum class SyncWordQualifierMode
  {
      NoPreambleOrSync                                         = 0, // No preamble/sync
      SyncWordBitsDetected_15_Of_16                            = 1, // 15/16 sync word bits detected
      SyncWordsBitDetected_16_Of_16                            = 2, // 16/16 sync word bits detected
      SyncWordsBitDetected_30_Of_32                            = 3, // 30/32 sync word bits detected
      NoPreambleOrSync_CarrierSenseAboveThreshold              = 4, // No preamble/sync, carrier-sense above threshold
      SyncWordBitsDetected_15_Of_16_CarrierSenseAboveThreshold = 5, // 15/16 + carrier-sense above threshold
      SyncWordsBitDetected_16_Of_16_CarrierSenseAboveThreshold = 6, // 16/16 + carrier-sense above threshold
      SyncWordsBitDetected_30_Of_32_CarrierSenseAboveThreshold = 7  // 30/32 + carrier-sense above threshold
  };
  enum class AddessCheckConfiguration
  {
      None                              = 0, // No address check
      AddressCheck_NoBroadcast          = 1, // Address check, no broadcast
      AddressCheck_ZeroBroadcast        = 2, // Address check and 0 (0x00) broadcast
      AdressCheck_Zero_And_FF_BroadCast = 3  // Address check and 0 (0x00) and 255 (0xFF) broadcast
  };
} // namespace TI_CC1101
