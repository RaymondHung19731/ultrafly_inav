/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifdef USE_SERIALRX_GHST

#include "build/build_config.h"
#include "build/debug.h"

#include "common/crc.h"
#include "common/maths.h"
#include "common/utils.h"

#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "drivers/system.h"
#include "drivers/time.h"

#include "io/serial.h"

#include "rx/rx.h"
#include "rx/ghst.h"

#include "telemetry/ghst.h"

#define GHST_PORT_OPTIONS               (SERIAL_STOPBITS_1 | SERIAL_PARITY_NO | SERIAL_BIDIR | SERIAL_BIDIR_PP)
#define GHST_PORT_MODE                  MODE_RXTX   // bidirectional on single pin

#define GHST_MAX_FRAME_TIME_US          500         // 14 bytes @ 420k = ~450us
#define GHST_TIME_BETWEEN_FRAMES_US     4500        // fastest frame rate = 222.22Hz, or 4500us

// define the time window after the end of the last received packet where telemetry packets may be sent
// NOTE: This allows the Rx to double-up on Rx packets to transmit data other than servo data, but
// only if sent < 1ms after the servo data packet.
#define GHST_RX_TO_TELEMETRY_MIN_US     1000
#define GHST_RX_TO_TELEMETRY_MAX_US     2000

// At max frame rate 222Hz we should expect to see each of 3 RC frames at least every 13.5ms
// Set the individual frame timeout high-enough to tolerate 2 on-wire frames being lost + some jitter
// As a recovery condition we would expect at least 3 packets arriving on time
#define GHST_RC_FRAME_TIMEOUT_MS        300     // To accommodate the LR mode (12Hz)
#define GHST_RC_FRAME_COUNT_THRESHOLD   4       // should correspond to ~50-60ms in the best case

#define GHST_PAYLOAD_OFFSET offsetof(ghstFrameDef_t, type)

STATIC_UNIT_TESTED volatile bool ghstFrameAvailable = false;
STATIC_UNIT_TESTED volatile bool ghstValidatedFrameAvailable = false;
STATIC_UNIT_TESTED volatile bool ghstTransmittingTelemetry = false;

STATIC_UNIT_TESTED ghstFrame_t ghstIncomingFrame;   // incoming frame, raw, not CRC checked, destination address not checked
STATIC_UNIT_TESTED ghstFrame_t ghstValidatedFrame;  // validated frame, CRC is ok, destination address is ok, ready for decode

STATIC_UNIT_TESTED uint32_t ghstChannelData[GHST_MAX_NUM_CHANNELS];

typedef struct ghstFailsafeTracker_s {
    unsigned onTimePacketCounter;
    timeMs_t lastSeenMs;
} ghstFailsafeTracker_t;

static serialPort_t *serialPort;
static timeUs_t ghstRxFrameStartAtUs = 0;
static timeUs_t ghstRxFrameEndAtUs = 0;
static uint8_t telemetryBuf[GHST_FRAME_SIZE_MAX];
static uint8_t telemetryBufLen = 0;
static ghstFailsafeTracker_t ghstFsTracker[GHST_UL_RC_CHANS_FRAME_COUNT];

/* GHST Protocol
 * Ghost uses 420k baud single-wire, half duplex connection, connected to a FC UART 'Tx' pin
 * Each control packet is interleaved with one or more corresponding downlink packets
 *
 * Uplink packet format (Control packets)
 * <Addr><Len><Type><Payload><CRC>
 *
 * Addr:        u8      Destination address
 * Len          u8      Length includes the packet ID, but not the CRC
 * CRC          u8
 *
 * Ghost packets are designed to be as short as possible, for minimum latency.
 *
 * Note that the GHST protocol does not handle, itself, failsafe conditions. Packets are passed from
 * the Ghost receiver to Betaflight as and when they arrive. Betaflight itself is responsible for
 * determining when a failsafe is necessary based on dropped packets.
 *
  */

#define GHST_FRAME_LENGTH_ADDRESS       1
#define GHST_FRAME_LENGTH_FRAMELENGTH   1
#define GHST_FRAME_LENGTH_TYPE_CRC      1

// called from telemetry/ghst.c
void ghstRxWriteTelemetryData(const void *data, int len)
{
    len = MIN(len, (int)sizeof(telemetryBuf));
    memcpy(telemetryBuf, data, len);
    telemetryBufLen = len;
}

void ghstRxSendTelemetryData(void)
{
    // if there is telemetry data to write
    if (telemetryBufLen > 0) {
        serialWriteBuf(serialPort, telemetryBuf, telemetryBufLen);
        telemetryBufLen = 0; // reset telemetry buffer
    }
}

STATIC_UNIT_TESTED uint8_t ghstFrameCRC(ghstFrame_t *pGhstFrame)
{
    // CRC includes type and payload
    uint8_t crc = crc8_dvb_s2(0, pGhstFrame->frame.type);
    for (int i = 0; i < pGhstFrame->frame.len - GHST_FRAME_LENGTH_TYPE_CRC - 1; ++i) {
        crc = crc8_dvb_s2(crc, pGhstFrame->frame.payload[i]);
    }
    return crc;
}

// Receive ISR callback, called back from serial port
STATIC_UNIT_TESTED void ghstDataReceive(uint16_t c, void *data)
{
    UNUSED(data);

    static uint8_t ghstFrameIdx = 0;
    const timeUs_t currentTimeUs = microsISR();

    if (cmpTimeUs(currentTimeUs, ghstRxFrameStartAtUs) > GHST_MAX_FRAME_TIME_US) {
        // Character received after the max. frame time, assume that this is a new frame
        ghstFrameIdx = 0;
    }

    if (ghstFrameIdx == 0) {
        // timestamp the start of the frame, to allow us to detect frame sync issues
        ghstRxFrameStartAtUs = currentTimeUs;
    }

    // assume frame is 5 bytes long until we have received the frame length
    // full frame length includes the length of the address and framelength fields
    const int fullFrameLength = ghstFrameIdx < 3 ? 5 : ghstIncomingFrame.frame.len + GHST_FRAME_LENGTH_ADDRESS + GHST_FRAME_LENGTH_FRAMELENGTH;

    if (ghstFrameIdx < fullFrameLength) {
        ghstIncomingFrame.bytes[ghstFrameIdx++] = (uint8_t)c;
        if (ghstFrameIdx >= fullFrameLength) {
            ghstFrameIdx = 0;

            // NOTE: this data is not yet CRC checked, nor do we know whether we are the correct recipient, this is
            // handled in ghstFrameStatus
            memcpy(&ghstValidatedFrame, &ghstIncomingFrame, sizeof(ghstIncomingFrame));
            ghstFrameAvailable = true;

            // remember what time the incoming (Rx) packet ended, so that we can ensure a quite bus before sending telemetry
            ghstRxFrameEndAtUs = microsISR();
        }
    }
}

static bool shouldSendTelemetryFrame(void)
{
    const timeUs_t now = micros();
    const timeDelta_t timeSinceRxFrameEndUs = cmpTimeUs(now, ghstRxFrameEndAtUs);
    return telemetryBufLen > 0 && timeSinceRxFrameEndUs > GHST_RX_TO_TELEMETRY_MIN_US && timeSinceRxFrameEndUs < GHST_RX_TO_TELEMETRY_MAX_US;
}

static void ghstIdle(void)
{
    if (ghstTransmittingTelemetry) {
        ghstTransmittingTelemetry = false;
    }
}

static void ghstUpdateFailsafe(unsigned pktIdx)
{
    // pktIdx is an offset of RC channel packet,
    // We'll track arrival time of each of the frame types we ever saw arriving from this receiver
    if (pktIdx < GHST_UL_RC_CHANS_FRAME_COUNT) {
        if (ghstFsTracker[pktIdx].onTimePacketCounter < GHST_RC_FRAME_COUNT_THRESHOLD) {
            ghstFsTracker[pktIdx].onTimePacketCounter++;
        }

        ghstFsTracker[pktIdx].lastSeenMs = millis();    // don't need microsecond resolution here
    }
}

static bool ghstDetectFailsafe(void)
{
    const timeMs_t currentTimeMs = millis();
    int pktIdx;


    // Inspect all of the frame types we ever saw arriving. If any of them times out - assume signal loss
    // We should track all frame types because we care about all channels, not only AETR. Losing AUX may
    // prevent the pilot from switching flight mode or disarming which is unsafe and should also be treated
    // as a failsafe condition

    for (pktIdx = 0; pktIdx < GHST_UL_RC_CHANS_FRAME_COUNT; pktIdx++) {

        // If a frame was not seen at least once, it's not sent and we should not detaect failsafe based on that
        if (ghstFsTracker[pktIdx].lastSeenMs == 0) {
            continue;
        }

        // Packet timeout. We didn't receive the packet containing the channel data within GHST_RC_FRAME_TIMEOUT_MS
        // This is a consistent signal loss, reset the recovery packet counter and report signal loss condition
        if ((currentTimeMs - ghstFsTracker[pktIdx].lastSeenMs) >= GHST_RC_FRAME_TIMEOUT_MS) {
            ghstFsTracker[pktIdx].onTimePacketCounter = 0;
            return true;
        }

        // Not having at least GHST_RC_FRAME_COUNT_THRESHOLD packets without timeouts is likely caused by intermittent signal
        // Stick to reporting signal loss
        if (ghstFsTracker[pktIdx].onTimePacketCounter < GHST_RC_FRAME_COUNT_THRESHOLD) {
            return true;
        }
    }

    return false;
}

uint8_t ghstFrameStatus(rxRuntimeConfig_t *rxRuntimeState)
{
    UNUSED(rxRuntimeState);

    if (serialIsIdle(serialPort)) {
        ghstIdle();
    }

    uint8_t ghstFailsafeFlag = ghstDetectFailsafe() ? RX_FRAME_FAILSAFE : 0;

    if (ghstFrameAvailable) {
        ghstFrameAvailable = false;

        const uint8_t crc = ghstFrameCRC(&ghstValidatedFrame);
        const int fullFrameLength = ghstValidatedFrame.frame.len + GHST_FRAME_LENGTH_ADDRESS + GHST_FRAME_LENGTH_FRAMELENGTH;
        if (crc == ghstValidatedFrame.bytes[fullFrameLength - 1] && ghstValidatedFrame.frame.addr == GHST_ADDR_FC) {
            ghstValidatedFrameAvailable = true;
            return ghstFailsafeFlag | RX_FRAME_COMPLETE | RX_FRAME_PROCESSING_REQUIRED;            // request callback through ghstProcessFrame to do the decoding  work
        }

        return ghstFailsafeFlag | RX_FRAME_DROPPED;                            // frame was invalid
    }

    if (shouldSendTelemetryFrame()) {
        return ghstFailsafeFlag | RX_FRAME_PROCESSING_REQUIRED;
    }

    return ghstFailsafeFlag | RX_FRAME_PENDING;
}

static bool ghstProcessFrame(const rxRuntimeConfig_t *rxRuntimeConfig)
{
    // Assume that the only way we get here is if ghstFrameStatus returned RX_FRAME_PROCESSING_REQUIRED, which indicates that the CRC
    // is correct, and the message was actually for us.

    // do we have a telemetry buffer to send?
    if (shouldSendTelemetryFrame()) {
        ghstTransmittingTelemetry = true;
        ghstRxSendTelemetryData();
    }

    if (ghstValidatedFrameAvailable) {
        int startIdx = 0;

        if (
            ghstValidatedFrame.frame.type >= GHST_UL_RC_CHANS_HS4_FIRST &&
            ghstValidatedFrame.frame.type <= GHST_UL_RC_CHANS_HS4_LAST
        ) {
            const ghstPayloadPulses_t* const rcChannels = (ghstPayloadPulses_t*)&ghstValidatedFrame.frame.payload;

            // notify GHST failsafe detection that we received a channel packet
            ghstUpdateFailsafe(ghstValidatedFrame.frame.type - GHST_UL_RC_CHANS_HS4_FIRST);

            // all uplink frames contain CH1..4 data (12 bit)
            ghstChannelData[0] = rcChannels->ch1to4.ch1 >> 1;
            ghstChannelData[1] = rcChannels->ch1to4.ch2 >> 1;
            ghstChannelData[2] = rcChannels->ch1to4.ch3 >> 1;
            ghstChannelData[3] = rcChannels->ch1to4.ch4 >> 1;

            switch(ghstValidatedFrame.frame.type) {
                case GHST_UL_RC_CHANS_HS4_RSSI: {
                    const ghstPayloadPulsesRSSI_t* const rssiFrame = (ghstPayloadPulsesRSSI_t*)&ghstValidatedFrame.frame.payload;
                    lqTrackerSet(rxRuntimeConfig->lqTracker, scaleRange(constrain(rssiFrame->lq, 0, 100), 0, 100, 0, RSSI_MAX_VALUE));

                    break;
                }

                case GHST_UL_RC_CHANS_HS4_5TO8:     startIdx = 4;  break;
                case GHST_UL_RC_CHANS_HS4_9TO12:    startIdx = 8;  break;
                case GHST_UL_RC_CHANS_HS4_13TO16:   startIdx = 12; break;
            }

            if (startIdx > 0)
            {
                // remainder of uplink frame contains 4 more channels (8 bit), sent in a round-robin fashion

                ghstChannelData[startIdx++] = rcChannels->cha << 3;
                ghstChannelData[startIdx++] = rcChannels->chb << 3;
                ghstChannelData[startIdx++] = rcChannels->chc << 3;
                ghstChannelData[startIdx++] = rcChannels->chd << 3;
            }
        }
    }

    return true;
}

STATIC_UNIT_TESTED uint16_t ghstReadRawRC(const rxRuntimeConfig_t *rxRuntimeState, uint8_t chan)
{
    UNUSED(rxRuntimeState);

    // derived from original SBus scaling, with slight correction for offset (now symmetrical
    // around OpenTx 0 value)
    // scaling is:
    //      OpenTx   RC     PWM
    // min  -1024    172    988us
    // ctr  0        992    1500us
    // max  1024     1811   2012us
    //

    return (5 * (ghstChannelData[chan]+1) / 8) + 880;
}

bool ghstRxInit(const rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeState)
{
    for (int iChan = 0; iChan < GHST_MAX_NUM_CHANNELS; ++iChan) {
        ghstChannelData[iChan] = (16 * PWM_RANGE_MIDDLE) / 10 - 1408;
    }

    rxRuntimeState->channelCount = GHST_MAX_NUM_CHANNELS;
    rxRuntimeState->rcReadRawFn = ghstReadRawRC;
    rxRuntimeState->rcFrameStatusFn = ghstFrameStatus;
    rxRuntimeState->rcProcessFrameFn = ghstProcessFrame;

    const serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_RX_SERIAL);
    if (!portConfig) {
        return false;
    }

    serialPort = openSerialPort(portConfig->identifier,
        FUNCTION_RX_SERIAL,
        ghstDataReceive,
        NULL,
        GHST_RX_BAUDRATE,
        GHST_PORT_MODE,
        GHST_PORT_OPTIONS | (rxConfig->serialrx_inverted ? SERIAL_INVERTED : 0)
    );

    return serialPort != NULL;
}

bool ghstRxIsActive(void)
{
    return serialPort != NULL;
}
#endif
