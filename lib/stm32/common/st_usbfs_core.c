/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2015 Robin Kreis <r.kreis@uni-bremen.de>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/cm3/common.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/tools.h>
#include <libopencm3/stm32/st_usbfs.h>
#include <libopencm3/usb/usbd.h>
#include "../../usb/usb_private.h"
#include "st_usbfs_core.h"

/* TODO - can't these be inside the impls, not globals from the core? */
uint8_t st_usbfs_force_nak[8];
struct _usbd_device st_usbfs_dev;

void st_usbfs_set_address(usbd_device *dev, uint8_t addr)
{
	(void)dev;
	/* Set device address and enable. */
	SET_REG(USB_DADDR_REG, (addr & USB_DADDR_ADDR) | USB_DADDR_EF);
}

struct st_usbfs_bufsize {
    uint16_t realsize;
    uint32_t bufsize;
};

static struct st_usbfs_bufsize st_usbfs_calculate_bufsize(uint32_t ep_size)
{
    struct st_usbfs_bufsize result = {0, ep_size};

    /*
     * Writes USB_COUNTn_RX reg fields : bits <14:10> are NUM_BLOCK; bit 15 is BL_SIZE
     * - When (size <= 62), BL_SIZE is set to 0 and NUM_BLOCK set to (size / 2).
     * - When (size > 62), BL_SIZE is set to 1 and NUM_BLOCK=((size / 32) - 1).
     *
     * This algo rounds to the next largest legal buffer size, except 0. Examples:
     *	size =>	BL_SIZE, NUM_BLOCK	=> Actual bufsize
     *	0		0		0			??? "Not allowed" according to RM0091, RM0008
     *	1		0		1			2
     *	61		0		31			62
     *	63		1		1			64
     */
    if (result.bufsize > 62) {
        /* Round up, div by 32 and sub 1 == (size + 31)/32 - 1 == (size-1)/32)*/
        result.bufsize = ((result.bufsize - 1) >> 5) & 0x1F;
        result.realsize = (result.bufsize + 1) << 5;
        /* Set BL_SIZE bit (no macro for this) */
        result.bufsize |= (1<<5);
    } else {
        /* round up and div by 2 */
        result.bufsize = (result.bufsize + 1) >> 1;
        result.realsize = result.bufsize << 1;
    }

    result.bufsize <<= 10;
    return result;
}

static const uint16_t typelookup[] = {
    [USB_ENDPOINT_ATTR_CONTROL] = USB_EP_TYPE_CONTROL,
    [USB_ENDPOINT_ATTR_ISOCHRONOUS] = USB_EP_TYPE_ISO,
    [USB_ENDPOINT_ATTR_BULK] = USB_EP_TYPE_BULK,
    [USB_ENDPOINT_ATTR_INTERRUPT] = USB_EP_TYPE_INTERRUPT
};

static void st_usbfs_ep_setup_single(usbd_device *dev, uint8_t addr, uint8_t type,
        uint16_t max_size,
        void (*callback) (usbd_device *usbd_dev,
        uint8_t ep))
{
    uint8_t dir = addr & 0x80;
    addr &= 0x7f;

    /* Assign address. */
    USB_SET_EP_ADDR(addr, addr);
    USB_SET_EP_TYPE(addr, typelookup[type]);

    if (dir || (addr == 0)) {
        USB_SET_EP_TX_ADDR(addr, dev->pm_top);
        if (callback) {
            dev->user_callback_ctr[addr][USB_TRANSACTION_IN] =
                (void *)callback;
        }
        USB_CLR_EP_TX_DTOG(addr);
        USB_SET_EP_TX_STAT(addr, USB_EP_TX_STAT_NAK);
        dev->pm_top += max_size;
    }

    if (!dir) {
        struct st_usbfs_bufsize bufsize = st_usbfs_calculate_bufsize(max_size);
        USB_SET_EP_RX_ADDR(addr, dev->pm_top);
        USB_SET_EP_RX_COUNT(addr, bufsize.bufsize);
        if (callback) {
            dev->user_callback_ctr[addr][USB_TRANSACTION_OUT] =
                (void *)callback;
        }
        USB_CLR_EP_RX_DTOG(addr);
        USB_SET_EP_RX_STAT(addr, USB_EP_RX_STAT_VALID);
        dev->pm_top += bufsize.realsize;
    }
}

static void st_usbfs_ep_setup_double(usbd_device *dev, uint8_t addr, uint8_t type,
        uint16_t max_size,
        void (*callback) (usbd_device *usbd_dev,
        uint8_t ep))
{
    uint8_t dir = addr & 0x80;
    addr &= 0x7f;

    /* Assign address. */
    USB_SET_EP_ADDR(addr, addr);
    USB_SET_EP_TYPE(addr, typelookup[type]);

    if (dir || (addr == 0)) {
        USB_SET_EP_TX_ADDR(addr, dev->pm_top); // TX0
        dev->pm_top += max_size;
        USB_SET_EP_RX_ADDR(addr, dev->pm_top); // TX1
        dev->pm_top += max_size;
        if (callback) {
            dev->user_callback_ctr[addr][USB_TRANSACTION_IN] =
                (void *)callback;
        }
        USB_CLR_EP_TX_DTOG(addr); // TX0
        USB_CLR_EP_RX_DTOG(addr); // TX1
        USB_SET_EP_TX_STAT(addr, USB_EP_TX_STAT_VALID);
    }

    if (!dir) {
        struct st_usbfs_bufsize bufsize = st_usbfs_calculate_bufsize(max_size);
        USB_SET_EP_TX_ADDR(addr, dev->pm_top); // RX0
        USB_SET_EP_TX_COUNT(addr, bufsize.bufsize);
        dev->pm_top += bufsize.realsize;
        USB_SET_EP_RX_ADDR(addr, dev->pm_top); // RX1
        USB_SET_EP_RX_COUNT(addr, bufsize.bufsize);
        dev->pm_top += bufsize.realsize;
        if (callback) {
            dev->user_callback_ctr[addr][USB_TRANSACTION_OUT] =
                (void *)callback;
        }
        USB_CLR_EP_TX_DTOG(addr); // RX0
        USB_CLR_EP_RX_DTOG(addr); // RX1
        USB_SET_EP_RX_STAT(addr, USB_EP_RX_STAT_VALID);
    }
}

void st_usbfs_ep_setup(usbd_device *dev, uint8_t addr, uint8_t type,
		uint16_t max_size,
		void (*callback) (usbd_device *usbd_dev,
		uint8_t ep))
{
    if (type != USB_ENDPOINT_ATTR_ISOCHRONOUS)
        st_usbfs_ep_setup_single(dev, addr, type, max_size, callback);
    else
        st_usbfs_ep_setup_double(dev, addr, type, max_size, callback);
}

void st_usbfs_endpoints_reset(usbd_device *dev)
{
	int i;

	/* Reset all endpoints. */
	for (i = 1; i < 8; i++) {
		USB_SET_EP_TX_STAT(i, USB_EP_TX_STAT_DISABLED);
		USB_SET_EP_RX_STAT(i, USB_EP_RX_STAT_DISABLED);
	}
	dev->pm_top = USBD_PM_TOP + (2 * dev->desc->bMaxPacketSize0);
}

void st_usbfs_ep_stall_set(usbd_device *dev, uint8_t addr,
				   uint8_t stall)
{
	(void)dev;
	if (addr == 0) {
		USB_SET_EP_TX_STAT(addr, stall ? USB_EP_TX_STAT_STALL :
				   USB_EP_TX_STAT_NAK);
	}

	if (addr & 0x80) {
		addr &= 0x7F;

		USB_SET_EP_TX_STAT(addr, stall ? USB_EP_TX_STAT_STALL :
				   USB_EP_TX_STAT_NAK);

		/* Reset to DATA0 if clearing stall condition. */
		if (!stall) {
			USB_CLR_EP_TX_DTOG(addr);
		}
	} else {
		/* Reset to DATA0 if clearing stall condition. */
		if (!stall) {
			USB_CLR_EP_RX_DTOG(addr);
		}

		USB_SET_EP_RX_STAT(addr, stall ? USB_EP_RX_STAT_STALL :
				   USB_EP_RX_STAT_VALID);
	}
}

uint8_t st_usbfs_ep_stall_get(usbd_device *dev, uint8_t addr)
{
	(void)dev;
	if (addr & 0x80) {
		if ((*USB_EP_REG(addr & 0x7F) & USB_EP_TX_STAT) ==
		    USB_EP_TX_STAT_STALL) {
			return 1;
		}
	} else {
		if ((*USB_EP_REG(addr) & USB_EP_RX_STAT) ==
		    USB_EP_RX_STAT_STALL) {
			return 1;
		}
	}
	return 0;
}

void st_usbfs_ep_nak_set(usbd_device *dev, uint8_t addr, uint8_t nak)
{
	(void)dev;
	/* It does not make sense to force NAK on IN endpoints. */
	if (addr & 0x80) {
		return;
	}

	st_usbfs_force_nak[addr] = nak;

	if (nak) {
		USB_SET_EP_RX_STAT(addr, USB_EP_RX_STAT_NAK);
	} else {
		USB_SET_EP_RX_STAT(addr, USB_EP_RX_STAT_VALID);
	}
}

static uint16_t st_usbfs_ep_write_packet_single(uint8_t addr, const void *buf, uint16_t len)
{
    addr &= 0x7F;
    if ((*USB_EP_REG(addr) & USB_EP_TX_STAT) == USB_EP_TX_STAT_VALID)
        return 0;
    st_usbfs_copy_to_pm(USB_GET_EP_TX_BUFF(addr), buf, len);
    USB_SET_EP_TX_COUNT(addr, len);
    USB_SET_EP_TX_STAT(addr, USB_EP_TX_STAT_VALID);
    return len;
}

static uint16_t st_usbfs_ep_write_packet_isoch(uint8_t addr, const void *buf, uint16_t len)
{
    addr &= 0x7F;
    if ((*USB_EP_REG(addr) & USB_EP_TX_STAT) != USB_EP_TX_STAT_VALID)
        return 0;
    const bool toggled = !(GET_REG(USB_EP_REG(addr)) & USB_EP_TX_DTOG);
    volatile void *pm_buf = (toggled) ? USB_GET_EP_TX_BUFF(addr)
                                      : USB_GET_EP_RX_BUFF(addr);
    if (toggled)
        USB_SET_EP_TX_COUNT(addr, len);
    else
        USB_SET_EP_RX_COUNT(addr, len);
    st_usbfs_copy_to_pm(pm_buf, buf, len);
    return len;
}

uint16_t st_usbfs_ep_write_packet(usbd_device *dev, uint8_t addr,
				     const void *buf, uint16_t len)
{
	(void)dev;

    if ((GET_REG(USB_EP_REG(addr)) & USB_EP_TYPE) == USB_EP_TYPE_ISO)
        return st_usbfs_ep_write_packet_isoch(addr, buf, len);
    return st_usbfs_ep_write_packet_single(addr, buf, len);
}

static uint16_t st_usbfs_ep_read_packet_single(uint8_t addr, void *buf, uint16_t len)
{
    if ((*USB_EP_REG(addr) & USB_EP_RX_STAT) == USB_EP_RX_STAT_VALID)
        return 0;

    len = MIN(USB_GET_EP_RX_COUNT(addr) & 0x3ff, len);
    st_usbfs_copy_from_pm(buf, USB_GET_EP_RX_BUFF(addr), len);
    USB_CLR_EP_RX_CTR(addr);

    if (!st_usbfs_force_nak[addr])
        USB_SET_EP_RX_STAT(addr, USB_EP_RX_STAT_VALID);

    return len;
}

static uint16_t st_usbfs_ep_read_packet_isoch(uint8_t addr, void *buf, uint16_t len)
{
    if ((*USB_EP_REG(addr) & USB_EP_RX_STAT) != USB_EP_RX_STAT_VALID)
        return 0;

    const bool toggled = (GET_REG(USB_EP_REG(addr)) & USB_EP_RX_DTOG);
    const volatile void *pm_buf = (toggled) ? USB_GET_EP_TX_BUFF(addr)
                                            : USB_GET_EP_RX_BUFF(addr);
    const uint16_t pm_len = ((toggled) ? USB_GET_EP_TX_COUNT(addr)
                                       : USB_GET_EP_RX_COUNT(addr)) & 0x3ff;

    len = MIN(pm_len, len);
    st_usbfs_copy_from_pm(buf, pm_buf, len);
    return len;
}

uint16_t st_usbfs_ep_read_packet(usbd_device *dev, uint8_t addr,
					 void *buf, uint16_t len)
{
	(void)dev;

    if ((GET_REG(USB_EP_REG(addr)) & USB_EP_TYPE) == USB_EP_TYPE_ISO)
        return st_usbfs_ep_read_packet_isoch(addr, buf, len);
    return st_usbfs_ep_read_packet_single(addr, buf, len);
}

void st_usbfs_poll(usbd_device *dev)
{
	uint16_t istr = *USB_ISTR_REG;

	if (istr & USB_ISTR_RESET) {
		USB_CLR_ISTR_RESET();
		dev->pm_top = USBD_PM_TOP;
		_usbd_reset(dev);
		return;
	}

	if (istr & USB_ISTR_CTR) {
		uint8_t ep = istr & USB_ISTR_EP_ID;
		uint8_t type;

		if (istr & USB_ISTR_DIR) {
			/* OUT or SETUP? */
			if (*USB_EP_REG(ep) & USB_EP_SETUP) {
				type = USB_TRANSACTION_SETUP;
				st_usbfs_ep_read_packet(dev, ep, &dev->control_state.req, 8);
			} else {
				type = USB_TRANSACTION_OUT;
			}
		} else {
			type = USB_TRANSACTION_IN;
			USB_CLR_EP_TX_CTR(ep);
		}

		if (dev->user_callback_ctr[ep][type]) {
			dev->user_callback_ctr[ep][type] (dev, ep);
		} else {
			USB_CLR_EP_RX_CTR(ep);
		}
	}

	if (istr & USB_ISTR_SUSP) {
		USB_CLR_ISTR_SUSP();
		if (dev->user_callback_suspend) {
			dev->user_callback_suspend();
		}
	}

	if (istr & USB_ISTR_WKUP) {
		USB_CLR_ISTR_WKUP();
		if (dev->user_callback_resume) {
			dev->user_callback_resume();
		}
	}

	if (istr & USB_ISTR_SOF) {
		USB_CLR_ISTR_SOF();
		if (dev->user_callback_sof) {
			dev->user_callback_sof();
		}
	}

	if (dev->user_callback_sof) {
		*USB_CNTR_REG |= USB_CNTR_SOFM;
	} else {
		*USB_CNTR_REG &= ~USB_CNTR_SOFM;
	}
}
