/*
 * Copyright (C) 2016 José Ignacio Alamos
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @{
 *
 * @file
 * @author  José Ignacio Alamos <jialamos@uc.cl>
 * @author  Hyung-Sin Kim <hs.kim@berkeley.edu>
 */

#include <stdio.h>
#include <assert.h>
#include <openthread/platform/radio.h>
#include "ot.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

#include "errno.h"
#include "net/ethernet/hdr.h"
#include "net/ethertype.h"
#include "byteorder.h"
#include <string.h>


/**
 * The following are valid radio state transitions:
 *
 *                                    (Radio ON)
 *  +----------+  Enable()  +-------+  Receive() +---------+   Transmit()  +----------+
 *  |          |----------->|       |----------->|         |-------------->|          |
 *  | Disabled |            | Sleep |            | Receive |               | Transmit |
 *  |          |<-----------|       |<-----------|         |<--------------|          |
 *  +----------+  Disable() +-------+   Sleep()  +---------+   Receive()   +----------+
 *                                    (Radio OFF)                 or
 *                                                        signal TransmitDone
 */



otInstance* myInstance = NULL;
otRadioCaps sRadioCaps;
static RadioPacket sTransmitFrame;
static RadioPacket sReceiveFrame;

static netdev2_t *_dev;


/* asks the driver the current 15.4 channel */
uint16_t get_channel(void)
{
    uint16_t channel;

    _dev->driver->get(_dev, NETOPT_CHANNEL, &channel, sizeof(uint16_t));
    return channel;
}


/* set 15.4 channel */
int set_channel(uint16_t channel)
{
    return _dev->driver->set(_dev, NETOPT_CHANNEL, &channel, sizeof(uint16_t));
}


/*get transmission power from driver */
int16_t get_power(void)
{
    int16_t power;

    _dev->driver->get(_dev, NETOPT_TX_POWER, &power, sizeof(int16_t));
    return power;
}


/* set transmission power */
int set_power(int16_t power)
{
    return _dev->driver->set(_dev, NETOPT_TX_POWER, &power, sizeof(int16_t));
}


/* set IEEE802.15.4 PAN ID */
int set_panid(uint16_t panid)
{
    return _dev->driver->set(_dev, NETOPT_NID, &panid, sizeof(uint16_t));
}

/* get extended HW address */
uint64_t get_long_addr(uint8_t *ext_addr)
{
    return _dev->driver->get(_dev, NETOPT_ADDRESS_LONG, ext_addr, sizeof(uint64_t));
}


/* set extended HW address */
int set_long_addr(uint8_t *ext_addr)
{
    return _dev->driver->set(_dev, NETOPT_ADDRESS_LONG, ext_addr, sizeof(uint64_t));
}


/* get short address */
uint16_t get_addr(uint16_t addr)
{
    return _dev->driver->get(_dev, NETOPT_ADDRESS, &addr, sizeof(uint16_t));
}


/* set short address */
int set_addr(uint16_t addr)
{
    return _dev->driver->set(_dev, NETOPT_ADDRESS, &addr, sizeof(uint16_t));
}


/* check the state of promiscuous mode */
netopt_enable_t is_promiscuous(void)
{
    netopt_enable_t en;

    _dev->driver->get(_dev, NETOPT_PROMISCUOUSMODE, &en, sizeof(en));
    return en == NETOPT_ENABLE ? true : false;;
}


/* set the state of promiscuous mode */
int set_promiscuous(netopt_enable_t enable)
{
    return _dev->driver->set(_dev, NETOPT_PROMISCUOUSMODE, &enable, sizeof(enable));
}

/* wrapper for getting device state */
int get_state(void)
{
    netopt_state_t en;
    _dev->driver->get(_dev, NETOPT_STATE, &en, sizeof(netopt_state_t));
    return en;
}


/* wrapper for setting device state */
void set_state(netopt_state_t state)
{
    _dev->driver->set(_dev, NETOPT_STATE, &state, sizeof(netopt_state_t));
}

/* sets device state to DISABLED */
void ot_disable(void)
{
    set_state(NETOPT_STATE_OFF);
}

/* sets device state to SLEEP */
void ot_sleep(void)
{
	printf("SLEEP\n");
    set_state(NETOPT_STATE_SLEEP);
}

/* set device state to RECEIVE */
void ot_receive(void)
{
    set_state(NETOPT_STATE_IDLE);
}


/* check if device state is SLEEP */
bool is_sleep(void)
{
	return get_state() == NETOPT_STATE_SLEEP;
}


/* check if device state is RECEIVE */
bool is_receive(void)
{
    return (get_state() == NETOPT_STATE_IDLE || get_state() == NETOPT_STATE_RX);
}


/* check if device is receiving a packet */
bool is_transmit(void)
{
    return get_state() == NETOPT_STATE_TX;
}


/* check if device is receiving a packet */
bool is_busy(void)
{
    return (get_state() == NETOPT_STATE_TX || get_state() == NETOPT_STATE_RX);
}



/* init framebuffers and initial state */
void openthread_radio_init(netdev2_t *dev, uint8_t *tb, uint8_t *rb)
{
    sTransmitFrame.mPsdu = tb;
    sTransmitFrame.mLength = 0;
    sReceiveFrame.mPsdu = rb;
    sReceiveFrame.mLength = 0;
    _dev = dev;

	/* Radio supports full functionalities */
	sRadioCaps = (kRadioCapsTransmitRetries | kRadioCapsCsmaBackOff | 
				  kRadioCapsTransmitRetries | kRadioCapsAckTimeout);
}


/* Called upon NETDEV2_EVENT_RX_COMPLETE event */
void recv_pkt(netdev2_t *dev)
{
    /* Read frame length from driver */
    int len = dev->driver->recv(dev, NULL, 0, NULL);

    /* very unlikely */
    if ((len > (unsigned) UINT16_MAX)) {
        otPlatRadioReceiveDone(myInstance, NULL, kThreadError_Abort);
        return;
    }

    /* Fill OpenThread receive frame */
    sReceiveFrame.mLength = len;
    sReceiveFrame.mPower = get_power();

    /* Read received frame */
    int res = dev->driver->recv(dev, (char *) sReceiveFrame.mPsdu, len, NULL);

    /* Tell OpenThread that receive has finished */
    otPlatRadioReceiveDone(myInstance, res > 0 ? &sReceiveFrame : NULL, 
							res > 0 ? kThreadError_None : kThreadError_Abort);
}


/* Called upon TX event */
void sent_pkt(netdev2_t *dev, netdev2_event_t event)
{
    /* Tell OpenThread transmission is done depending on the NETDEV2 event */
    switch (event) {
        case NETDEV2_EVENT_TX_COMPLETE:
            DEBUG("openthread: NETDEV2_EVENT_TX_COMPLETE\n");
            otPlatRadioTransmitDone(myInstance, &sTransmitFrame, false, kThreadError_None);
            break;
        case NETDEV2_EVENT_TX_COMPLETE_DATA_PENDING:
            DEBUG("openthread: NETDEV2_EVENT_TX_COMPLETE_DATA_PENDING\n");
            otPlatRadioTransmitDone(myInstance, &sTransmitFrame, true, kThreadError_None);
            break;
        case NETDEV2_EVENT_TX_NOACK:
            DEBUG("openthread: NETDEV2_EVENT_TX_NOACK\n");
            otPlatRadioTransmitDone(myInstance, &sTransmitFrame, false, kThreadError_NoAck);
            break;
        case NETDEV2_EVENT_TX_MEDIUM_BUSY:
            DEBUG("openthread: NETDEV2_EVENT_TX_MEDIUM_BUSY\n");
            otPlatRadioTransmitDone(myInstance, &sTransmitFrame, false,
								    kThreadError_ChannelAccessFailure);
            break;
        default:
            break;
    }
}


/* Get the factory-assigned IEEE EUI-64 for this interface. */
void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    DEBUG("openthread: otPlatRadioGetIeeeEui64\n");
	get_long_addr(aIeeeEui64);
}


/* OpenThread will call this for setting PAN ID */
void otPlatRadioSetPanId(otInstance *aInstance, uint16_t aPanId) 
{
    DEBUG("openthread: otPlatRadioSetPanId: setting PAN ID to %04x\n", apanid);
    set_panid(((aPanId & 0xff) << 8) | ((aPanId >> 8) & 0xff));
}


/* OpenThread will call this for setting extended address */
void otPlatRadioSetExtendedAddress(otInstance *aInstance, uint8_t *aExtendedAddress)
{
    DEBUG("openthread: otPlatRadioSetExtendedAddress\n");
    uint8_t reversed_addr[IEEE802154_LONG_ADDRESS_LEN];
    for (int i = 0; i < IEEE802154_LONG_ADDRESS_LEN; i++) {
        reversed_addr[i] = aExtendedAddress[IEEE802154_LONG_ADDRESS_LEN - 1 - i];
    }
    set_long_addr(reversed_addr);
}


/* OpenThread will call this for setting short address */
void otPlatRadioSetShortAddress(otInstance *aInstance, uint16_t aShortAddress)
{
    DEBUG("openthread: otPlatRadioSetShortAddress: setting address to %04x\n", aShortAddress);
    set_addr(((aShortAddress & 0xff) << 8) | ((aShortAddress >> 8) & 0xff));
}



/* Get current state of the radio. */
PhyState otPlatRadioGetState(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioGetState\n");
	netopt_state_t state = get_state();
	switch (state) {
		case NETOPT_STAET_OFF:
			return kStateDisabled;
		case NETOPT_STATE_SLEEP:
			return kStateSleep;
		case NETOPT_STATE_IDLE:
		case NETOPT_STATE_RX:
			return kStateReceive;
		case NETOPT_STATE_TX:
			return kStateTransmit;
	}
}


/* OpenThread will call this for enabling the radio */
ThreadError otPlatRadioEnable(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioEnable\n");
    if (is_busy()) {
        DEBUG("openthread: Radio is already enabled and busy\n");
	    return kThreadError_None;
    }
    set_state(NETOPT_STATE_SLEEP);
    return kThreadError_None;
}


/* OpenThread will call this for disabling the radio */
ThreadError otPlatRadioDisable(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioDisable/Sleep\n");
    if (is_busy()) {
        DEBUG("openthread: Radio is busy and couldn't sleep/turn off\n");
        return kThreadError_Busy;
    }
    set_state(NETOPT_STATE_OFF);
    return kThreadError_None;
}


/* Check whether radio is enabled or not. */
bool otPlatRadioIsEnabled(otInstance *aInstance) 
{
    DEBUG("openthread: otPlatRadioIsEnabled\n");
	return (get_state() != NETOPT_STATE_OFF);
}


/* OpenThread will call this for setting device state to SLEEP */
ThreadError otPlatRadioSleep(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioSleep\n");
    if (is_busy()) {
        DEBUG("openthread: Radio is busy\n");
	    return kThreadError_Busy;
    }
	set_state(NETOPT_STATE_SLEEP);
	return otPlatRadioDisable(aInstance);
}


/* OpenThread will call this for waiting the reception of a packet */
ThreadError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    DEBUG("openthread: otPlatRadioReceive\n");
    if (is_busy()) {
        DEBUG("openthread: otPlatRadioReceive: Device not ready\n");
        return kThreadError_Busy;
    }
    set_channel(aChannel);
    sReceiveFrame.mChannel = aChannel;
	set_state(NETOPT_STATE_IDLE);
    return kThreadError_None;
}


/* Enable/Disable source address match feature. */
void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
}


/* Add a short address to the source address match table */
ThreadError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, const uint16_t aShortAddress)
{
	return kThreadError_None;
}


/* Add an extended address to the source address match table */
ThreadError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const uint8_t *aExtAddress)
{
	return kThreadError_None;
}


/* Remove a short address from the source address match table */
ThreadError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, const uint16_t aShortAddress)
{
	return kThreadError_None;
}


/* Remove an extended address from the source address match table */
ThreadError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const uint8_t *aExtAddress)
{
	return kThreadError_None;
}


/* Clear all short addresses from the source address match table */
void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
}


/* Clear all the extended/long addresses from source address match table */
void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
}


/* OpenThread will call this function to get the transmit buffer */
RadioPacket *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioGetTransmitBuffer\n");
    return &sTransmitFrame;
}


/* OpenThread will call this for transmitting a packet*/
ThreadError otPlatRadioTransmit(otInstance *aInstance, RadioPacket *aPacket)
{
    DEBUG("openthread: otPlatRadioTransmit\n");

    if (is_rx() || is_tx()) {
        DEBUG("openthread: otPlatRadioTransmit: Device not ready.\n");

        /* OpenThread will assert(false) if this function returns kThreadError_None.
         * These asserts don't throw core_panic, so it's better to assert here.*/
        assert(false);
        return kThreadError_Busy;
    }

    struct iovec pkt;

    /* Populate iovec with transmit data */
    pkt.iov_base = sTransmitFrame.mPsdu;
    pkt.iov_len = sTransmitFrame.mLength;

    /*Set channel and power based on transmit frame */
    set_channel(sTransmitFrame.mChannel);
    set_power(sTransmitFrame.mPower);

    /* send packet though netdev2 */
	/* ToDo: Radio should do CSMA and CCA */ 
    _dev->driver->send(_dev, &pkt, 1);

    return kThreadError_None;
}


/* OpenThread will call this for getting the Noise Floor */
int8_t otPlatRadioGetNoiseFloor(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioGetNoiseFloor\n");

	/* ToDo */
    return 0;
}


/* Get the most recent RSSI measurement */
int8_t otPlatRadioGetRssi(otInstance *aInstance) {
    DEBUG("openthread: otPlatRadioGetRssi\n");

	/* ToDo */
	return 0;
}


/* OpenThread will call this for getting the radio caps */
otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioGetCaps\n");
    return sRadioCaps;
}

/* Set the radio Tx power used for auto-generated frames. */
void otPlatRadioSetDefaultTxPower(otInstance *aInstance, int8_t aPower) 
{
    DEBUG("openthread: otPlatRadioSetDefaultTxPower\n");
    set_power(aPower);
}


/* OpenThread will call this for getting the state of promiscuous mode */
bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    DEBUG("openthread: otPlatRadioGetPromiscuous\n");
    return is_promiscuous();
}


/* OpenThread will call this for setting the state of promiscuous mode */
void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    DEBUG("openthread: otPlatRadioSetPromiscuous\n");
    set_promiscuous((aEnable) ? NETOPT_ENABLE : NETOPT_DISABLE);
}


ThreadError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration) 
{
    DEBUG("openthread: otPlatRadioEnergyScan\n");

	/* ToDo */
    return kThreadError_None;
}



/** @} */
