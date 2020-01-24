/* drivers/net/ethernet/micrel/ks8851.c
 *
 * Copyright 2009 Simtec Electronics
 *	http://www.simtec.co.uk/
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DEBUG

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/cache.h>
#include <linux/crc32.h>
#include <linux/mii.h>
#include <linux/eeprom_93cx6.h>
#include <linux/regulator/consumer.h>

#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#include "ks8851.h"

#include <linux/dma-mapping.h>

#define KSZ8851_TX_SPACE (6144 * 3)
#define TX_DMA_BUFFER_SIZE (8192 * 3)
#define RX_DMA_BUFFER_SIZE (2048 * 2)
#define MAX_RXFIFO_SIZE (12 * 1024)
#define CIDER_READ_MAX_ITER 20
#define CIDER_READ_MAX_DELAY 20

/**
 * struct ks8851_rxctrl - KS8851 driver rx control
 * @mchash: Multicast hash-table data.
 * @rxcr1: KS_RXCR1 register setting
 * @rxcr2: KS_RXCR2 register setting
 *
 * Representation of the settings needs to control the receive filtering
 * such as the multicast hash-filter and the receive register settings. This
 * is used to make the job of working out if the receive settings change and
 * then issuing the new settings to the worker that will send the necessary
 * commands.
 */
struct ks8851_rxctrl {
	u16	mchash[4];
	u16	rxcr1;
	u16	rxcr2;
};

/**
 * union ks8851_tx_hdr - tx header data
 * @txb: The header as bytes
 * @txw: The header as 16bit, little-endian words
 *
 * A dual representation of the tx header data to allow
 * access to individual bytes, and to allow 16bit accesses
 * with 16bit alignment.
 */
union ks8851_tx_hdr {
	u8	txb[6];
	__le16	txw[3];
};

/**
 * struct ks8851_net - KS8851 driver private data
 * @netdev: The network device we're bound to
 * @spidev: The spi device we're bound to.
 * @lock: Lock to ensure that the device is not accessed when busy.
 * @statelock: Lock on this structure for tx list.
 * @mii: The MII state information for the mii calls.
 * @rxctrl: RX settings for @rxctrl_work.
 * @tx_work: Work queue for tx packets
 * @rxctrl_work: Work queue for updating RX mode and multicast lists
 * @txq: Queue of packets for transmission.
 * @spi_msg1: pre-setup SPI transfer with one message, @spi_xfer1.
 * @spi_msg2: pre-setup SPI transfer with two messages, @spi_xfer2.
 * @txh: Space for generating packet TX header in DMA-able data
 * @rxd: Space for receiving SPI data, in DMA-able space.
 * @txd: Space for transmitting SPI data, in DMA-able space.
 * @msg_enable: The message flags controlling driver output (see ethtool).
 * @fid: Incrementing frame id tag.
 * @rc_ier: Cached copy of KS_IER.
 * @rc_ccr: Cached copy of KS_CCR.
 * @rc_rxqcr: Cached copy of KS_RXQCR.
 * @eeprom_size: Companion eeprom size in Bytes, 0 if no eeprom
 * @eeprom: 93CX6 EEPROM state for accessing on-board EEPROM.
 * @vdd_reg:	Optional regulator supplying the chip
 * @vdd_io: Optional digital power supply for IO
 * @gpio: Optional reset_n gpio
 *
 * The @lock ensures that the chip is protected when certain operations are
 * in progress. When the read or write packet transfer is in progress, most
 * of the chip registers are not ccessible until the transfer is finished and
 * the DMA has been de-asserted.
 *
 * The @statelock is used to protect information in the structure which may
 * need to be accessed via several sources, such as the network driver layer
 * or one of the work queues.
 *
 * We align the buffers we may use for rx/tx to ensure that if the SPI driver
 * wants to DMA map them, it will not have any problems with data the driver
 * modifies.
 */
struct ks8851_net {
	struct pm_qos_request qos_request;
	struct net_device	*netdev;
	struct spi_device	*spidev;
	struct mutex		lock;
	spinlock_t		statelock;

	union ks8851_tx_hdr	txh ____cacheline_aligned;
	u8			rxd[8];
	u8			txd[8];

	u32			msg_enable ____cacheline_aligned;
	u16			tx_space;
	u8			fid;

	u16			rc_ier;
	u16			rc_rxqcr;
	u16			rc_ccr;
	u16			eeprom_size;

	struct mii_if_info	mii;
	struct ks8851_rxctrl	rxctrl;

	struct work_struct	tx_work;
	struct work_struct	rx_work;
	struct work_struct	rxctrl_work;

	struct sk_buff_head	txq;

	struct spi_message	spi_msg1;
	struct spi_message	spi_msg2;
	struct spi_transfer	spi_xfer1;
	struct spi_transfer	spi_xfer2[2];

	struct eeprom_93cx6	eeprom;
	struct regulator	*vdd_reg;
	struct regulator	*vdd_io;
	int			gpio;
};

static int msg_enable;

/* shift for byte-enable data */
#define BYTE_EN(_x)	((_x) << 2)

/* turn register number and byte-enable mask into data for start of packet */
#define MK_OP(_byteen, _reg) (BYTE_EN(_byteen) | (_reg)  << (8+2) | (_reg) >> 6)

#define KS8851_RX_WORKQUEUE_NAME "ks8851_rx_wq"
#define KS8851_TX_WORKQUEUE_NAME "ks8851_tx_wq"

static struct workqueue_struct *ks8851_rx_wq;
static struct workqueue_struct *ks8851_tx_wq;

/* SPI register read/write calls.
 *
 * All these calls issue SPI transactions to access the chip's registers. They
 * all require that the necessary lock is held to prevent accesses when the
 * chip is busy transferring packet data (RX/TX FIFO accesses).
 */

/**
 * ks8851_wrreg16 - write 16bit register value to chip
 * @ks: The chip state
 * @reg: The register address
 * @val: The value to write
 *
 * Issue a write to put the value @val into the register specified in @reg.
 */
static void ks8851_wrreg16(struct ks8851_net *ks, unsigned reg, unsigned val)
{
	struct spi_transfer *xfer = &ks->spi_xfer1;
	struct spi_message *msg = &ks->spi_msg1;
	__le16 txb[2];
	int ret;

	txb[0] = cpu_to_le16(MK_OP(reg & 2 ? 0xC : 0x03, reg) | KS_SPIOP_WR);
	txb[1] = cpu_to_le16(val);

	xfer->tx_buf = txb;
	xfer->rx_buf = NULL;
	xfer->len = 4;

	ret = spi_sync(ks->spidev, msg);
	if (ret < 0)
		netdev_err(ks->netdev, "spi_sync() failed\n");
}

/**
 * ks8851_wrreg8 - write 8bit register value to chip
 * @ks: The chip state
 * @reg: The register address
 * @val: The value to write
 *
 * Issue a write to put the value @val into the register specified in @reg.
 */
static void ks8851_wrreg8(struct ks8851_net *ks, unsigned reg, unsigned val)
{
	struct spi_transfer *xfer = &ks->spi_xfer1;
	struct spi_message *msg = &ks->spi_msg1;
	__le16 txb[2];
	int ret;
	int bit;

	bit = 1 << (reg & 3);

	txb[0] = cpu_to_le16(MK_OP(bit, reg) | KS_SPIOP_WR);
	txb[1] = val;

	xfer->tx_buf = txb;
	xfer->rx_buf = NULL;
	xfer->len = 3;

	ret = spi_sync(ks->spidev, msg);
	if (ret < 0)
		netdev_err(ks->netdev, "spi_sync() failed\n");
}

/**
 * ks8851_rx_1msg - select whether to use one or two messages for spi read
 * @ks: The device structure
 *
 * Return whether to generate a single message with a tx and rx buffer
 * supplied to spi_sync(), or alternatively send the tx and rx buffers
 * as separate messages.
 *
 * Depending on the hardware in use, a single message may be more efficient
 * on interrupts or work done by the driver.
 *
 * This currently always returns true until we add some per-device data passed
 * from the platform code to specify which mode is better.
 */
static inline bool ks8851_rx_1msg(struct ks8851_net *ks)
{
	return true;
}

/**
 * ks8851_rdreg - issue read register command and return the data
 * @ks: The device state
 * @op: The register address and byte enables in message format.
 * @rxb: The RX buffer to return the result into
 * @rxl: The length of data expected.
 *
 * This is the low level read call that issues the necessary spi message(s)
 * to read data from the register specified in @op.
 */
static void ks8851_rdreg(struct ks8851_net *ks, unsigned op,
			 u8 *rxb, unsigned rxl)
{
	struct spi_transfer *xfer;
	struct spi_message *msg;
	__le16 *txb = (__le16 *)ks->txd;
	u8 *trx = ks->rxd;
	int ret;

	txb[0] = cpu_to_le16(op | KS_SPIOP_RD);

	if (ks8851_rx_1msg(ks)) {
		msg = &ks->spi_msg1;
		xfer = &ks->spi_xfer1;

		xfer->tx_buf = txb;
		xfer->rx_buf = trx;
		xfer->len = rxl + 2;
	} else {
		msg = &ks->spi_msg2;
		xfer = ks->spi_xfer2;

		xfer->tx_buf = txb;
		xfer->rx_buf = NULL;
		xfer->len = 2;

		xfer++;
		xfer->tx_buf = NULL;
		xfer->rx_buf = trx;
		xfer->len = rxl;
	}

	ret = spi_sync(ks->spidev, msg);
	if (ret < 0)
		netdev_err(ks->netdev, "read: spi_sync() failed\n");
	else if (ks8851_rx_1msg(ks))
		memcpy(rxb, trx + 2, rxl);
	else
		memcpy(rxb, trx, rxl);
}

/**
 * ks8851_rdreg16 - read 16 bit register from device
 * @ks: The chip information
 * @reg: The register address
 *
 * Read a 16bit register from the chip, returning the result
*/
static unsigned ks8851_rdreg16(struct ks8851_net *ks, unsigned reg)
{
	__le16 rx = 0;

	ks8851_rdreg(ks, MK_OP(reg & 2 ? 0xC : 0x3, reg), (u8 *)&rx, 2);
	return le16_to_cpu(rx);
}

/**
 * ks8851_rdreg8 - read 8 bit register from device
 * @ks: The chip information
 * @reg: The register address
 *
 * Read a 8bit register from the chip, returning the result
*/
static unsigned ks8851_rdreg8(struct ks8851_net *ks, unsigned reg)
{
	u8 rxb[1];

	ks8851_rdreg(ks, MK_OP(1 << (reg & 3), reg), rxb, 1);
	return rxb[0];
}

static unsigned ks8851_32bitrdreg16(struct ks8851_net *ks, unsigned reg)
{
	int ret;
	struct spi_transfer *xfer = &ks->spi_xfer1;
	struct spi_message *msg = &ks->spi_msg1;
	u32 *txd;
	u32 *rxd;
	unsigned op;

	txd = (u32 *)ks->txd;
	rxd = (u32 *)ks->rxd;
	op = MK_OP(reg & 2 ? 0xC : 0x3, reg);
	txd[0] = htonl(0x0000ffff & (op | KS_SPIOP_RD));

	xfer->bits_per_word = 32;
	xfer->tx_buf = txd;
	xfer->rx_buf = rxd;
	xfer->len = 4;
	ret = spi_sync(ks->spidev, msg);
	xfer->bits_per_word = 8;
	if (ret < 0)
		netdev_err(ks->netdev, "read: spi_sync() failed\n");

	return htons((u16)rxd[0]);
}

/**
 * ks8851_soft_reset - issue one of the soft reset to the device
 * @ks: The device state.
 * @op: The bit(s) to set in the GRR
 *
 * Issue the relevant soft-reset command to the device's GRR register
 * specified by @op.
 *
 * Note, the delays are in there as a caution to ensure that the reset
 * has time to take effect and then complete. Since the datasheet does
 * not currently specify the exact sequence, we have chosen something
 * that seems to work with our device.
 */
static void ks8851_soft_reset(struct ks8851_net *ks, unsigned op)
{
	ks8851_wrreg16(ks, KS_GRR, op);
	mdelay(1);	/* wait a short time to effect reset */
	ks8851_wrreg16(ks, KS_GRR, 0);
	mdelay(1);	/* wait for condition to clear */
}

/**
 * ks8851_set_powermode - set power mode of the device
 * @ks: The device state
 * @pwrmode: The power mode value to write to KS_PMECR.
 *
 * Change the power mode of the chip.
 */
static void ks8851_set_powermode(struct ks8851_net *ks, unsigned pwrmode)
{
	unsigned pmecr;

	netif_dbg(ks, hw, ks->netdev, "setting power mode %d\n", pwrmode);

	pmecr = ks8851_32bitrdreg16(ks, KS_PMECR);
	pmecr &= ~PMECR_PM_MASK;
	pmecr |= pwrmode;

	ks8851_wrreg16(ks, KS_PMECR, pmecr);
}

/**
 * ks8851_write_mac_addr - write mac address to device registers
 * @dev: The network device
 *
 * Update the KS8851 MAC address registers from the address in @dev.
 *
 * This call assumes that the chip is not running, so there is no need to
 * shutdown the RXQ process whilst setting this.
*/
static int ks8851_write_mac_addr(struct net_device *dev)
{
	struct ks8851_net *ks;
	int i;

	ks = netdev_priv(dev);
	mutex_lock(&ks->lock);

	/*
	 * Wake up chip in case it was powered off when stopped; otherwise,
	 * the first write to the MAC address does not take effect.
	 */
	ks8851_set_powermode(ks, PMECR_PM_NORMAL);
	for (i = 0; i < ETH_ALEN; i++)
		ks8851_wrreg8(ks, KS_MAR(i), dev->dev_addr[i]);
	if (!netif_running(dev))
		ks8851_set_powermode(ks, PMECR_PM_SOFTDOWN);

	mutex_unlock(&ks->lock);

	return 0;
}

/**
 * ks8851_read_mac_addr - read mac address from device registers
 * @dev: The network device
 *
 * Update our copy of the KS8851 MAC address from the registers of @dev.
*/
static void ks8851_read_mac_addr(struct net_device *dev)
{
	struct ks8851_net *ks;
	int i;

	ks = netdev_priv(dev);
	mutex_lock(&ks->lock);

	for (i = 0; i < ETH_ALEN; i++)
		dev->dev_addr[i] = ks8851_rdreg8(ks, KS_MAR(i));

	mutex_unlock(&ks->lock);
}

/**
 * ks8851_init_mac - initialise the mac address
 * @ks: The device structure
 *
 * Get or create the initial mac address for the device and then set that
 * into the station address register. If there is an EEPROM present, then
 * we try that. If no valid mac address is found we use eth_random_addr()
 * to create a new one.
 */
static void ks8851_init_mac(struct ks8851_net *ks)
{
	struct net_device *dev;

	dev = ks->netdev;
	/* first, try reading what we've got already */
	if (ks->rc_ccr & CCR_EEPROM) {
		ks8851_read_mac_addr(dev);
		if (is_valid_ether_addr(dev->dev_addr))
			return;

		netdev_err(ks->netdev, "invalid mac address read %pM\n",
				dev->dev_addr);
	}

	eth_hw_addr_random(dev);
	ks8851_write_mac_addr(dev);
}

/**
 * ks8851_rdfifo - read data from the receive fifo
 * @ks: The device state.
 * @buff: The buffer address
 * @len: The length of the data to read
 *
 * Issue an RXQ FIFO read command and read the @len amount of data from
 * the FIFO into the buffer specified by @buff.
 */
static void ks8851_rdfifo(struct ks8851_net *ks, u8 *buff, unsigned len)
{
	struct spi_transfer *xfer;
	struct spi_message *msg;
	u8 txb[1];
	int ret;

	xfer = ks->spi_xfer2;
	msg = &ks->spi_msg2;
	netif_dbg(ks, rx_status, ks->netdev,
		  "%s: %d@%p\n", __func__, len, buff);

	/* set the operation we're issuing */
	txb[0] = KS_SPIOP_RXFIFO;

	xfer->tx_buf = txb;
	xfer->rx_buf = NULL;
	xfer->len = 1;

	xfer++;
	xfer->bits_per_word = 32;
	xfer->rx_buf = buff;
	xfer->tx_buf = NULL;
	xfer->len = len;

	ret = spi_sync(ks->spidev, msg);
	xfer->bits_per_word = 8;
	if (ret < 0)
		netdev_err(ks->netdev, "%s: spi_sync() failed\n", __func__);
}

static int ks8851_rdfifolen(struct ks8851_net *ks, u16 fc)
{
	int ret;
	u16 count;
	u16 rxfifosize = 0;
	u16 tmpfifosize = 0;
	struct spi_transfer *xfer;
	struct spi_message *msg;
	u32 *txd;
	u32 *rxd = kmalloc(4 * fc, GFP_DMA);
	unsigned op;

	if (rxd == NULL)
		return -ENOMEM;

	txd = (u32 *)ks->txd;
	op = MK_OP(KS_RXFHBCR & 2 ? 0xC : 0x3, KS_RXFHBCR);
	txd[0] = htonl(0x0000ffff & (op | KS_SPIOP_RD));

	/*Create new with all the transfers*/
	msg = spi_message_alloc(fc, GFP_DMA);

	if (msg == NULL) {
		kfree(rxd);
		return -ENOMEM;
	}

	/* fill in each transfer */
	count = 0;
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		xfer->bits_per_word = 32;
		xfer->tx_buf = txd;
		xfer->rx_buf = &rxd[count];
		xfer->delay_usecs = 0;
		xfer->len = 4;
		xfer->cs_change = ++count == fc ? 0 : 1;
	}

	ret = spi_sync(ks->spidev, msg);

	for (count = 0, tmpfifosize = 0; count < fc; count++) {
		if (tmpfifosize >= MAX_RXFIFO_SIZE)
			break;
		tmpfifosize += ALIGN((htons((u16)rxd[count]) & 0xfff), 4)+4;
	}
	tmpfifosize += 4;
	rxfifosize = (tmpfifosize >=
		MAX_RXFIFO_SIZE) ? MAX_RXFIFO_SIZE : tmpfifosize;
	if (ret < 0)
		netdev_err(ks->netdev, "read: spi_sync() failed\n");
	spi_message_free(msg);
	kfree(rxd);

	return rxfifosize;
}

/**
 * ks8851_dbg_dumpkkt - dump initial packet contents to debug
 * @ks: The device state
 * @rxpkt: The data for the received packet
 *
 * Dump the initial data from the packet to dev_dbg().
*/
static void ks8851_dbg_dumpkkt(struct ks8851_net *ks, u8 *rxpkt)
{
	netdev_dbg(ks->netdev,
		   "pkt %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
		   rxpkt[4], rxpkt[5], rxpkt[6], rxpkt[7],
		   rxpkt[8], rxpkt[9], rxpkt[10], rxpkt[11],
		   rxpkt[12], rxpkt[13], rxpkt[14], rxpkt[15]);
}

/**
 * ks8851_rx_pkts3 - receive packets from the host
 * @ks: The device information.
 *
 * This is called from the IRQ work queue when the system detects that there
 * are packets in the receive queue. Find out how many packets there are and
 * read them from the FIFO.
 */
static void ks8851_rx_pkts3(struct ks8851_net *ks)
{
	struct sk_buff *skb = 0;
	unsigned rxfc = 0, rxfct = 0;
	int rxfifosize = 0;
	unsigned rxlen = 0;
	unsigned totallen = 0;
	unsigned rxalign = 0;
	u8 *rxpkt = 0;
	u8 *buf = NULL, *buf1 = NULL;
	u32 *buf32 = NULL;
	u16 rxlen32 = 0;
	u16 index32 = 0;

	/* read in fame count from ks8851 reg*/
	rxfc = ks8851_rdreg8(ks, KS_RXFC);
	rxfct = rxfc;

	if (rxfc == 0) {
		pr_debug("ks8851: Frame count is 0 NOP further\n");
		return;
	}

	/* tabulate all frame sizes so we can do one read for all frames */
	rxfifosize = ks8851_rdfifolen(ks, rxfc);
	if (rxfifosize <= 0) {
		pr_debug("ks8851:Memory not available");
		return;
	}

	/* allocate memory for one fifo read */
	buf = kmalloc(rxfifosize, GFP_DMA);
	if (buf == NULL)
		return;

	buf32 = (u32 *)buf;

	/* set dma read address */
	ks8851_wrreg16(ks, KS_RXFDPR, RXFDPR_RXFPAI | 0x00);

	/* start DMA access */
	ks8851_wrreg16(ks, KS_RXQCR, ks->rc_rxqcr | RXQCR_SDA);

	/* read all frames from rx fifo */
	ks8851_rdfifo(ks, buf, rxfifosize);
	/* parse frames */
	for (index32 = 1; rxfc > 0; rxfc--) {
		/* Get packet data length and pointer to packet data */
		rxlen = (htonl(buf32[index32]) >> 16) & 0xfff;
		index32++;
		rxalign = ALIGN(rxlen-4, 4);
		rxlen32 = ALIGN(rxlen, 4)/4;
		rxpkt = (u8 *)&buf32[index32];
		totallen += rxalign;
		if (totallen > rxfifosize) {
			if (rxfifosize >= MAX_RXFIFO_SIZE)
				break;
			buf1 = kzalloc(rxlen, GFP_DMA);
			if (buf1 == NULL)
				return;
			memcpy(buf1, rxpkt, (rxalign -
				(totallen - rxfifosize)));
			ks8851_rdfifo(ks, buf1 + (rxalign -
				(totallen - rxfifosize)),
						totallen - rxfifosize);
			buf32 = (u32 *)buf1;
			index32 = 0;
			rxpkt = (u8 *)&buf32[index32];
		}
		/* swap bytes to make the correct order */
		for (; rxlen32 > 0; rxlen32--, index32++)
			buf32[index32] = htonl(buf32[index32]);

		/* send packet on its way */
		skb = netdev_alloc_skb_ip_align(ks->netdev, rxalign);
		skb_add_data(skb, rxpkt, rxalign);

		if (netif_msg_pktdata(ks))
			ks8851_dbg_dumpkkt(ks, rxpkt);

		skb->protocol = eth_type_trans(skb, ks->netdev);
		skb->tstamp = ktime_get();
		netif_rx_ni(skb);

		/* record packet stats */
		ks->netdev->stats.rx_packets++;
		ks->netdev->stats.rx_bytes += rxlen;
			if (totallen > rxfifosize) {
				kfree(buf1);
				break;
			}
	}
	/* end DMA access and dequeue packet */
	ks8851_wrreg16(ks, KS_RXQCR, ks->rc_rxqcr | RXQCR_RRXEF);
	kfree(buf);
}

/**
 * ks8851_irq - IRQ handler for dealing with interrupt requests
 * @irq: IRQ number
 * @_ks: cookie
 *
 * This handler is invoked when the IRQ line asserts to find out what happened.
 * As we cannot allow ourselves to sleep in HARDIRQ context, this handler runs
 * in thread context.
 *
 * Read the interrupt status, work out what needs to be done and then clear
 * any of the interrupts that are not needed.
 */
static irqreturn_t ks8851_irq(int irq, void *_ks)
{
	struct ks8851_net *ks = _ks;
	unsigned status;
	unsigned handled = 0;
	mutex_lock(&ks->lock);

	status = ks8851_32bitrdreg16(ks, KS_ISR);

	netif_dbg(ks, intr, ks->netdev,
		  "%s: status 0x%04x\n", __func__, status);

	if (status & IRQ_LCI)
		handled |= IRQ_LCI;

	if (status & IRQ_LDI) {
		u16 pmecr = ks8851_32bitrdreg16(ks, KS_PMECR);
		pmecr &= ~PMECR_WKEVT_MASK;
		ks8851_wrreg16(ks, KS_PMECR, pmecr | PMECR_WKEVT_LINK);

		handled |= IRQ_LDI;
	}

	if (status & IRQ_RXPSI)
		handled |= IRQ_RXPSI;

	if (status & IRQ_TXI) {
		handled |= IRQ_TXI;

		/* no lock here, tx queue should have been stopped */

		/* update our idea of how much tx space is available to the
		 * system */
		ks->tx_space = ks8851_rdreg16(ks, KS_TXMIR);
		netif_dbg(ks, intr, ks->netdev,
			  "%s: txspace %d\n", __func__, ks->tx_space);
		/* TX FIFO is empty */
		netif_wake_queue(ks->netdev);
	}

	if (status & IRQ_RXI) {
		handled |= IRQ_RXI;
	}

	if (status & IRQ_SPIBEI) {
		dev_err(&ks->spidev->dev, "%s: spi bus error\n", __func__);
		handled |= IRQ_SPIBEI;
	}

	ks8851_wrreg16(ks, KS_ISR, handled);

	if (status & IRQ_RXI) {
		/* the datasheet says to disable the rx interrupt during
		 * packet read-out, however we're masking the interrupt
		 * from the device so do not bother masking just the RX
		 * from the device. */
		ks8851_rx_pkts3(ks);
	}

	/* if something stopped the rx process, probably due to wanting
	 * to change the rx settings, then do something about restarting
	 * it. */
	if (status & IRQ_RXPSI) {
		struct ks8851_rxctrl *rxc = &ks->rxctrl;

		/* update the multicast hash table */
		ks8851_wrreg16(ks, KS_MAHTR0, rxc->mchash[0]);
		ks8851_wrreg16(ks, KS_MAHTR1, rxc->mchash[1]);
		ks8851_wrreg16(ks, KS_MAHTR2, rxc->mchash[2]);
		ks8851_wrreg16(ks, KS_MAHTR3, rxc->mchash[3]);

		ks8851_wrreg16(ks, KS_RXCR2, rxc->rxcr2);
		ks8851_wrreg16(ks, KS_RXCR1, rxc->rxcr1);
	}

	if (unlikely(status & IRQ_RXOI))
		ks->netdev->stats.rx_over_errors++;

	mutex_unlock(&ks->lock);

	if (status & IRQ_LCI)
		mii_check_link(&ks->mii);
	return IRQ_HANDLED;
}

/**
 * calc_txlen - calculate size of message to send packet
 * @len: Length of data
 *
 * Returns the size of the TXFIFO message needed to send
 * this packet.
 */
static inline unsigned calc_txlen(unsigned len)
{
	return 4+ALIGN(len, 4);
}

/**
 * ks8851_wrpkts3 - write packets to TX FIFO
 *
 * Send the @txp to the chip. This means creating the relevant packet header
 * specifying the length of the packet and the other information the chip
 * needs, such as IRQ on completion. Send the header and the packet data to
 * the device.
 */
static void ks8851_wrpkts3(struct ks8851_net *ks)
{
	struct spi_transfer *xfer = ks->spi_xfer2;
	struct spi_message *msg = &ks->spi_msg2;
	unsigned fid = 0;
	int ret;

	struct net_device *dev = ks->netdev;
	struct sk_buff *txb, *clone = NULL;
	u8 *buf = NULL;
	u16 len = 0;
	u8 count = 0;
	u8 opc[1];
	u16 cpy;
	u16 index = 0;
	u16 len32;
	u32 *buf32;
	bool last;
	struct skb_shared_hwtstamps hwtstamps;

	last = skb_queue_empty(&ks->txq);
	opc[0] = KS_SPIOP_TXFIFO;
	xfer->tx_buf = opc;
	xfer->len = 1;
	xfer->rx_buf = NULL;
	xfer->bits_per_word = 8;
	xfer->cs_change = 0;
	xfer++;

	xfer->rx_buf = NULL;
	xfer->bits_per_word = 32;
	xfer->cs_change = 0;
	xfer->tx_buf = kmalloc(TX_DMA_BUFFER_SIZE, GFP_DMA);
	buf = (u8 *)xfer->tx_buf;
	buf32 = (u32 *)xfer->tx_buf;

	if (buf == NULL) {
		pr_debug("ks8851:memory alloc fail for tx buffer");
		return;
	}
	len = 0;
	len32 = 0;

	for (count = 0; !last && len < 6144; count++) {
		txb = skb_dequeue(&ks->txq);
		last = skb_queue_empty(&ks->txq);

		if (txb != NULL) {
			fid = ks->fid++;
			fid &= TXFR_TXFID_MASK;
			if (last || len >= 6144)
				fid |= TXFR_TXIC;
			buf32[len32] = htonl(cpu_to_le16(txb->len) << 16
						   | cpu_to_le16(fid));
			len32++;
			for (cpy = ALIGN(txb->len, 4)/4, index = 0;
					index < cpy; index++, len32++)
				buf32[len32] = htonl(((u32 *)txb->data)[index]);
			len += 4+ALIGN(txb->len, 4);
			dev->stats.tx_bytes += txb->len;
			dev->stats.tx_packets++;
			txb->tstamp = ktime_get();
			if (skb_shinfo(txb)->tx_flags & SKBTX_SW_TSTAMP &&
			    !(skb_shinfo(txb)->tx_flags & SKBTX_IN_PROGRESS)) {
				clone = skb_clone_sk(txb);
				if (clone)
					skb_complete_tx_timestamp(clone,
								  &hwtstamps);
			}

			dev_kfree_skb(txb);
		}
	}

	xfer->len = len;
	if (len > 0)
		ret = spi_sync(ks->spidev, msg);
	xfer->bits_per_word = 8;
	if (ret < 0)
		netdev_err(ks->netdev, "%s: spi_sync() failed\n", __func__);
	kfree(xfer->tx_buf);
	xfer->tx_buf = NULL;
}

static void ks8851_rx_work(struct work_struct *work)
{
	struct ks8851_net *ks = container_of(work, struct ks8851_net, rx_work);

	mutex_lock(&ks->lock);
	ks8851_rx_pkts3(ks);
	mutex_unlock(&ks->lock);
}
/**
 * ks8851_tx_work - process tx packet(s)
 * @work: The work strucutre what was scheduled.
 *
 * This is called when a number of packets have been scheduled for
 * transmission and need to be sent to the device.
 */
static void ks8851_tx_work(struct work_struct *work)
{
	struct ks8851_net *ks = container_of(work, struct ks8851_net, tx_work);
	bool last;
	mutex_lock(&ks->lock);
	last = skb_queue_empty(&ks->txq);

	if (!last) {
		ks8851_wrreg16(ks, KS_TXQCR, TXQCR_AETFE);
		ks8851_wrreg16(ks, KS_RXQCR, ks->rc_rxqcr | RXQCR_SDA);
		/* write packets to the queue once, and raise interrupt */
		while (!last) {
			ks8851_wrpkts3(ks);
			last = skb_queue_empty(&ks->txq);
		}
		ks8851_wrreg16(ks, KS_RXQCR, ks->rc_rxqcr);
	}
	mutex_unlock(&ks->lock);
}

/**
 * ks8851_net_open - open network device
 * @dev: The network device being opened.
 *
 * Called when the network device is marked active, such as a user executing
 * 'ifconfig up' on the device.
 */
static int ks8851_net_open(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);

	/* lock the card, even if we may not actually be doing anything
	 * else at the moment */
	mutex_lock(&ks->lock);
	pm_qos_add_request(&ks->qos_request, PM_QOS_CPU_DMA_LATENCY, 1);
	netif_dbg(ks, ifup, ks->netdev, "opening\n");

	/* bring chip out of any power saving mode it was in */
	ks8851_set_powermode(ks, PMECR_PM_NORMAL);

	/* issue a soft reset to the RX/TX QMU to put it into a known
	 * state. */
	ks8851_soft_reset(ks, GRR_QMU);

	/* setup transmission parameters */

	ks8851_wrreg16(ks, KS_TXCR, (TXCR_TXE | /* enable transmit process */
				     TXCR_TXPE | /* pad to min length */
				     TXCR_TXCRC | /* add CRC */
				     TXCR_TXFCE)); /* enable flow control */

	/* auto-increment tx data, reset tx pointer */
	ks8851_wrreg16(ks, KS_TXFDPR, TXFDPR_TXFPAI);

	/* setup receiver control */

	ks8851_wrreg16(ks, KS_RXCR1, (RXCR1_RXPAFMA | /*  from mac filter */
				      RXCR1_RXFCE | /* enable flow control */
				      RXCR1_RXBE | /* broadcast enable */
				      RXCR1_RXUE | /* unicast enable */
				      RXCR1_RXE)); /* enable rx block */

	/* transfer entire frames out in one go */
	ks8851_wrreg16(ks, KS_RXCR2, RXCR2_SRDBL_FRAME);

	/* set receive counter timeouts */
	ks8851_wrreg16(ks, KS_RXDTTR, 1000); /* 1ms after first frame to IRQ */
	ks8851_wrreg16(ks, KS_RXDBCTR, 4096);/* >4Kbytes in buffer to IRQ */
	ks8851_wrreg16(ks, KS_RXFCTR, 10);  /* 10 frames to IRQ */

	ks->rc_rxqcr = (RXQCR_RXFCTE |  /* IRQ on frame count exceeded */
			RXQCR_RXDBCTE | /* IRQ on byte count exceeded */
			RXQCR_RXDTTE);  /* IRQ on time exceeded */

	ks8851_wrreg16(ks, KS_RXQCR, ks->rc_rxqcr);

	/* Set watermarks for proper flow control. */
	ks8851_wrreg16(ks, KS_FCLWR, 0x800);
	ks8851_wrreg16(ks, KS_FCHWR, 0x400);

	/* clear then enable interrupts */

#define STD_IRQ (IRQ_LCI |	/* Link Change */	\
		 IRQ_TXI |	/* TX done */		\
		 IRQ_RXI |	/* RX done */		\
		 IRQ_SPIBEI |	/* SPI bus error */	\
		 IRQ_TXPSI |	/* TX process stop */	\
		 IRQ_RXPSI)	/* RX process stop */

	ks->rc_ier = STD_IRQ;
	ks8851_wrreg16(ks, KS_ISR, STD_IRQ);
	ks8851_wrreg16(ks, KS_IER, STD_IRQ);

	netif_start_queue(ks->netdev);

	netif_dbg(ks, ifup, ks->netdev, "network device up\n");

	mutex_unlock(&ks->lock);
	return 0;
}

/**
 * ks8851_net_stop - close network device
 * @dev: The device being closed.
 *
 * Called to close down a network device which has been active. Cancell any
 * work, shutdown the RX and TX process and then place the chip into a low
 * power state whilst it is not being used.
 */
static int ks8851_net_stop(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);

	netif_info(ks, ifdown, dev, "shutting down\n");

	netif_stop_queue(dev);

	mutex_lock(&ks->lock);
	pm_qos_remove_request(&ks->qos_request);
	/* turn off the IRQs and ack any outstanding */
	ks8851_wrreg16(ks, KS_IER, 0x0000);
	ks8851_wrreg16(ks, KS_ISR, 0xffff);
	mutex_unlock(&ks->lock);

	/* stop any outstanding work */
	flush_work(&ks->tx_work);
	flush_work(&ks->rxctrl_work);

	mutex_lock(&ks->lock);
	/* shutdown RX process */
	ks8851_wrreg16(ks, KS_RXCR1, 0x0000);

	/* shutdown TX process */
	ks8851_wrreg16(ks, KS_TXCR, 0x0000);

	/* set powermode to soft power down to save power */
	ks8851_set_powermode(ks, PMECR_PM_SOFTDOWN);
	mutex_unlock(&ks->lock);

	/* ensure any queued tx buffers are dumped */
	while (!skb_queue_empty(&ks->txq)) {
		struct sk_buff *txb = skb_dequeue(&ks->txq);

		netif_dbg(ks, ifdown, ks->netdev,
			  "%s: freeing txb %p\n", __func__, txb);

		dev_kfree_skb(txb);
	}

	return 0;
}

/**
 * ks8851_start_xmit - transmit packet
 * @skb: The buffer to transmit
 * @dev: The device used to transmit the packet.
 *
 * Called by the network layer to transmit the @skb. Queue the packet for
 * the device and schedule the necessary work to transmit the packet when
 * it is free.
 *
 * We do this to firstly avoid sleeping with the network device locked,
 * and secondly so we can round up more than one packet to transmit which
 * means we can try and avoid generating too many transmit done interrupts.
 */
static netdev_tx_t ks8851_start_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	struct ks8851_net *ks;
	unsigned needed;
	netdev_tx_t ret = NETDEV_TX_OK;

	ks = netdev_priv(dev);
	needed = calc_txlen(skb->len);
	netif_dbg(ks, tx_queued, ks->netdev,
		  "%s: skb %p, %d@%p\n", __func__, skb, skb->len, skb->data);

	spin_lock(&ks->statelock);
	if (needed > ks->tx_space) {
		ret = NETDEV_TX_BUSY;
		netif_stop_queue(dev);
	} else {
		ks->tx_space -= needed;
		skb_queue_tail(&ks->txq, skb);
	}
	spin_unlock(&ks->statelock);
	queue_work(ks8851_tx_wq, &ks->tx_work);
	return ret;
}

/**
 * ks8851_rxctrl_work - work handler to change rx mode
 * @work: The work structure this belongs to.
 *
 * Lock the device and issue the necessary changes to the receive mode from
 * the network device layer. This is done so that we can do this without
 * having to sleep whilst holding the network device lock.
 *
 * Since the recommendation from Micrel is that the RXQ is shutdown whilst the
 * receive parameters are programmed, we issue a write to disable the RXQ and
 * then wait for the interrupt handler to be triggered once the RXQ shutdown is
 * complete. The interrupt handler then writes the new values into the chip.
 */
static void ks8851_rxctrl_work(struct work_struct *work)
{
	struct ks8851_net *ks = container_of(work, struct ks8851_net, rxctrl_work);

	mutex_lock(&ks->lock);

	/* need to shutdown RXQ before modifying filter parameters */
	ks8851_wrreg16(ks, KS_RXCR1, 0x00);

	mutex_unlock(&ks->lock);
}

static void ks8851_set_rx_mode(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);
	struct ks8851_rxctrl rxctrl;

	memset(&rxctrl, 0, sizeof(rxctrl));

	if (dev->flags & IFF_PROMISC) {
		/* interface to receive everything */

		rxctrl.rxcr1 = RXCR1_RXAE | RXCR1_RXINVF;
	} else if (dev->flags & IFF_ALLMULTI) {
		/* accept all multicast packets */

		rxctrl.rxcr1 = (RXCR1_RXME | RXCR1_RXAE |
				RXCR1_RXPAFMA | RXCR1_RXMAFMA);
	} else if (dev->flags & IFF_MULTICAST && !netdev_mc_empty(dev)) {
		struct netdev_hw_addr *ha;
		u32 crc;

		/* accept some multicast */

		netdev_for_each_mc_addr(ha, dev) {
			crc = ether_crc(ETH_ALEN, ha->addr);
			crc >>= (32 - 6);  /* get top six bits */

			rxctrl.mchash[crc >> 4] |= (1 << (crc & 0xf));
		}

		rxctrl.rxcr1 = RXCR1_RXME | RXCR1_RXPAFMA;
	} else {
		/* just accept broadcast / unicast */
		rxctrl.rxcr1 = RXCR1_RXPAFMA;
	}

	rxctrl.rxcr1 |= (RXCR1_RXUE | /* unicast enable */
			 RXCR1_RXBE | /* broadcast enable */
			 RXCR1_RXE | /* RX process enable */
			 RXCR1_RXFCE); /* enable flow control */

	rxctrl.rxcr2 |= RXCR2_SRDBL_FRAME;

	/* schedule work to do the actual set of the data if needed */

	spin_lock(&ks->statelock);

	if (memcmp(&rxctrl, &ks->rxctrl, sizeof(rxctrl)) != 0) {
		memcpy(&ks->rxctrl, &rxctrl, sizeof(ks->rxctrl));
		schedule_work(&ks->rxctrl_work);
	}

	spin_unlock(&ks->statelock);
}

static int ks8851_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *sa = addr;

	if (netif_running(dev))
		return -EBUSY;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(dev->dev_addr, sa->sa_data, ETH_ALEN);
	return ks8851_write_mac_addr(dev);
}

static int ks8851_net_ioctl(struct net_device *dev, struct ifreq *req, int cmd)
{
	struct ks8851_net *ks = netdev_priv(dev);

	if (!netif_running(dev))
		return -EINVAL;

	return generic_mii_ioctl(&ks->mii, if_mii(req), cmd, NULL);
}

static const struct net_device_ops ks8851_netdev_ops = {
	.ndo_open		= ks8851_net_open,
	.ndo_stop		= ks8851_net_stop,
	.ndo_do_ioctl		= ks8851_net_ioctl,
	.ndo_start_xmit		= ks8851_start_xmit,
	.ndo_set_mac_address	= ks8851_set_mac_address,
	.ndo_set_rx_mode	= ks8851_set_rx_mode,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ethtool support */

static void ks8851_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *di)
{
	strlcpy(di->driver, "KS8851", sizeof(di->driver));
	strlcpy(di->version, "1.00", sizeof(di->version));
	strlcpy(di->bus_info, dev_name(dev->dev.parent), sizeof(di->bus_info));
}

static u32 ks8851_get_msglevel(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);
	return ks->msg_enable;
}

static void ks8851_set_msglevel(struct net_device *dev, u32 to)
{
	struct ks8851_net *ks = netdev_priv(dev);
	ks->msg_enable = to;
}

static int ks8851_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct ks8851_net *ks = netdev_priv(dev);
	return mii_ethtool_gset(&ks->mii, cmd);
}

static int ks8851_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct ks8851_net *ks = netdev_priv(dev);
	return mii_ethtool_sset(&ks->mii, cmd);
}

static u32 ks8851_get_link(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);
	return mii_link_ok(&ks->mii);
}

static int ks8851_nway_reset(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);
	return mii_nway_restart(&ks->mii);
}

/* EEPROM support */

static void ks8851_eeprom_regread(struct eeprom_93cx6 *ee)
{
	struct ks8851_net *ks = ee->data;
	unsigned val;

	val = ks8851_32bitrdreg16(ks, KS_EEPCR);

	ee->reg_data_out = (val & EEPCR_EESB) ? 1 : 0;
	ee->reg_data_clock = (val & EEPCR_EESCK) ? 1 : 0;
	ee->reg_chip_select = (val & EEPCR_EECS) ? 1 : 0;
}

static void ks8851_eeprom_regwrite(struct eeprom_93cx6 *ee)
{
	struct ks8851_net *ks = ee->data;
	unsigned val = EEPCR_EESA;	/* default - eeprom access on */

	if (ee->drive_data)
		val |= EEPCR_EESRWA;
	if (ee->reg_data_in)
		val |= EEPCR_EEDO;
	if (ee->reg_data_clock)
		val |= EEPCR_EESCK;
	if (ee->reg_chip_select)
		val |= EEPCR_EECS;

	ks8851_wrreg16(ks, KS_EEPCR, val);
}

/**
 * ks8851_eeprom_claim - claim device EEPROM and activate the interface
 * @ks: The network device state.
 *
 * Check for the presence of an EEPROM, and then activate software access
 * to the device.
 */
static int ks8851_eeprom_claim(struct ks8851_net *ks)
{
	if (!(ks->rc_ccr & CCR_EEPROM))
		return -ENOENT;

	mutex_lock(&ks->lock);

	/* start with clock low, cs high */
	ks8851_wrreg16(ks, KS_EEPCR, EEPCR_EESA | EEPCR_EECS);
	return 0;
}

/**
 * ks8851_eeprom_release - release the EEPROM interface
 * @ks: The device state
 *
 * Release the software access to the device EEPROM
 */
static void ks8851_eeprom_release(struct ks8851_net *ks)
{
	unsigned val = ks8851_32bitrdreg16(ks, KS_EEPCR);

	ks8851_wrreg16(ks, KS_EEPCR, val & ~EEPCR_EESA);
	mutex_unlock(&ks->lock);
}

#define KS_EEPROM_MAGIC (0x00008851)

static int ks8851_set_eeprom(struct net_device *dev,
			     struct ethtool_eeprom *ee, u8 *data)
{
	struct ks8851_net *ks = netdev_priv(dev);
	int offset = ee->offset;
	int len = ee->len;
	u16 tmp;

	/* currently only support byte writing */
	if (len != 1)
		return -EINVAL;

	if (ee->magic != KS_EEPROM_MAGIC)
		return -EINVAL;

	if (ks8851_eeprom_claim(ks))
		return -ENOENT;

	eeprom_93cx6_wren(&ks->eeprom, true);

	/* ethtool currently only supports writing bytes, which means
	 * we have to read/modify/write our 16bit EEPROMs */

	eeprom_93cx6_read(&ks->eeprom, offset/2, &tmp);

	if (offset & 1) {
		tmp &= 0xff;
		tmp |= *data << 8;
	} else {
		tmp &= 0xff00;
		tmp |= *data;
	}

	eeprom_93cx6_write(&ks->eeprom, offset/2, tmp);
	eeprom_93cx6_wren(&ks->eeprom, false);

	ks8851_eeprom_release(ks);

	return 0;
}

static int ks8851_get_eeprom(struct net_device *dev,
			     struct ethtool_eeprom *ee, u8 *data)
{
	struct ks8851_net *ks = netdev_priv(dev);
	int offset = ee->offset;
	int len = ee->len;

	/* must be 2 byte aligned */
	if (len & 1 || offset & 1)
		return -EINVAL;

	if (ks8851_eeprom_claim(ks))
		return -ENOENT;

	ee->magic = KS_EEPROM_MAGIC;

	eeprom_93cx6_multiread(&ks->eeprom, offset/2, (__le16 *)data, len/2);
	ks8851_eeprom_release(ks);

	return 0;
}

static int ks8851_get_eeprom_len(struct net_device *dev)
{
	struct ks8851_net *ks = netdev_priv(dev);

	/* currently, we assume it is an 93C46 attached, so return 128 */
	return ks->rc_ccr & CCR_EEPROM ? 128 : 0;
}

static const struct ethtool_ops ks8851_ethtool_ops = {
	.get_drvinfo	= ks8851_get_drvinfo,
	.get_msglevel	= ks8851_get_msglevel,
	.set_msglevel	= ks8851_set_msglevel,
	.get_settings	= ks8851_get_settings,
	.set_settings	= ks8851_set_settings,
	.get_link	= ks8851_get_link,
	.nway_reset	= ks8851_nway_reset,
	.get_eeprom_len	= ks8851_get_eeprom_len,
	.get_eeprom	= ks8851_get_eeprom,
	.set_eeprom	= ks8851_set_eeprom,
	.get_ts_info	= ethtool_op_get_ts_info,
};

/* MII interface controls */

/**
 * ks8851_phy_reg - convert MII register into a KS8851 register
 * @reg: MII register number.
 *
 * Return the KS8851 register number for the corresponding MII PHY register
 * if possible. Return zero if the MII register has no direct mapping to the
 * KS8851 register set.
 */
static int ks8851_phy_reg(int reg)
{
	switch (reg) {
	case MII_BMCR:
		return KS_P1MBCR;
	case MII_BMSR:
		return KS_P1MBSR;
	case MII_PHYSID1:
		return KS_PHY1ILR;
	case MII_PHYSID2:
		return KS_PHY1IHR;
	case MII_ADVERTISE:
		return KS_P1ANAR;
	case MII_LPA:
		return KS_P1ANLPR;
	}

	return 0x0;
}

/**
 * ks8851_phy_read - MII interface PHY register read.
 * @dev: The network device the PHY is on.
 * @phy_addr: Address of PHY (ignored as we only have one)
 * @reg: The register to read.
 *
 * This call reads data from the PHY register specified in @reg. Since the
 * device does not support all the MII registers, the non-existent values
 * are always returned as zero.
 *
 * We return zero for unsupported registers as the MII code does not check
 * the value returned for any error status, and simply returns it to the
 * caller. The mii-tool that the driver was tested with takes any -ve error
 * as real PHY capabilities, thus displaying incorrect data to the user.
 */
static int ks8851_phy_read(struct net_device *dev, int phy_addr, int reg)
{
	struct ks8851_net *ks = netdev_priv(dev);
	int ksreg;
	int result;

	ksreg = ks8851_phy_reg(reg);
	if (!ksreg)
		return 0x0;	/* no error return allowed, so use zero */

	mutex_lock(&ks->lock);
	result = ks8851_32bitrdreg16(ks, ksreg);
	mutex_unlock(&ks->lock);

	return result;
}

static void ks8851_phy_write(struct net_device *dev,
			     int phy, int reg, int value)
{
	struct ks8851_net *ks = netdev_priv(dev);
	int ksreg;

	ksreg = ks8851_phy_reg(reg);
	if (ksreg) {
		mutex_lock(&ks->lock);
		ks8851_wrreg16(ks, ksreg, value);
		mutex_unlock(&ks->lock);
	}
}

/**
 * ks8851_read_selftest - read the selftest memory info.
 * @ks: The device state
 *
 * Read and check the TX/RX memory selftest information.
 */
static int ks8851_read_selftest(struct ks8851_net *ks)
{
	unsigned both_done = MBIR_TXMBF | MBIR_RXMBF;
	int ret = 0;
	unsigned rd;

	rd = ks8851_32bitrdreg16(ks, KS_MBIR);

	if ((rd & both_done) != both_done) {
		netdev_warn(ks->netdev, "Memory selftest not finished\n");
		return 0;
	}

	if (rd & MBIR_TXMBFA) {
		netdev_err(ks->netdev, "TX memory selftest fail\n");
		ret |= 1;
	}

	if (rd & MBIR_RXMBFA) {
		netdev_err(ks->netdev, "RX memory selftest fail\n");
		ret |= 2;
	}

	return 0;
}

/* driver bus management functions */

#ifdef CONFIG_PM_SLEEP

static int ks8851_suspend(struct device *dev)
{
	struct ks8851_net *ks = dev_get_drvdata(dev);
	struct net_device *netdev = ks->netdev;

	if (netif_running(netdev)) {
		netif_device_detach(netdev);
		ks8851_net_stop(netdev);
	}

	return 0;
}

static int ks8851_resume(struct device *dev)
{
	struct ks8851_net *ks = dev_get_drvdata(dev);
	struct net_device *netdev = ks->netdev;

	if (netif_running(netdev)) {
		ks8851_net_open(netdev);
		netif_device_attach(netdev);
	}

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(ks8851_pm_ops, ks8851_suspend, ks8851_resume);

static int ks8851_probe(struct spi_device *spi)
{
	struct net_device *ndev;
	struct ks8851_net *ks;
	int ret;
	unsigned cider;
	int gpio;
	int iter;

	pr_info("eth: spi KS8851 Probe\n");
	ndev = alloc_etherdev(sizeof(struct ks8851_net));
	if (!ndev)
		return -ENOMEM;

	spi->bits_per_word = 8;

	ks = netdev_priv(ndev);

	ks->netdev = ndev;
	ks->spidev = spi;
	ks->tx_space = KSZ8851_TX_SPACE;


	gpio = of_get_named_gpio_flags(spi->dev.of_node, "reset-gpios",
				       0, NULL);
	if (gpio == -EPROBE_DEFER) {
		pr_debug("eth: spi KS8851 Probe Failed ret:%d\n", gpio);
		ret = gpio;
		goto err_gpio;
	}

	ks->gpio = gpio;
	pr_debug("eth: spi KS8851 Probe reset gpio#:%d\n", gpio);
	if (gpio_is_valid(gpio)) {
		pr_debug("eth: spi KS8851 Going to reset\n");
		ret = devm_gpio_request_one(&spi->dev, gpio,
					    GPIOF_OUT_INIT_LOW, "ks8851_rst_n");
		if (ret) {
			dev_err(&spi->dev, "reset gpio request failed\n");
			goto err_gpio;
		}
	} else {
		pr_debug("eth: spi KS8851 invalid gpio\n");
	}

	ks->vdd_io = devm_regulator_get(&spi->dev, "vdd-io");
	if (IS_ERR(ks->vdd_io)) {
		ret = PTR_ERR(ks->vdd_io);
	}

	ret = regulator_enable(ks->vdd_io);
	if (ret) {
		dev_err(&spi->dev, "regulator vdd_io enable fail: %d\n",
			ret);
	}

	ks->vdd_reg = devm_regulator_get(&spi->dev, "vdd");
	if (IS_ERR(ks->vdd_reg)) {
		ret = PTR_ERR(ks->vdd_reg);
	}

	ret = regulator_enable(ks->vdd_reg);
	if (ret) {
		dev_err(&spi->dev, "regulator vdd enable fail: %d\n",
			ret);
	}

	if (gpio_is_valid(gpio)) {
		pr_debug("eth: spi reset GPIO set to 1\n");
		ret = gpio_direction_output(gpio, 0x1);
		pr_debug("ks8851:return value for reset is %d\n", ret);
	} else {
		pr_debug("[%s:]eth: spi reset GPIO is invalid\n", __func__);
	}
	mutex_init(&ks->lock);
	spin_lock_init(&ks->statelock);

	INIT_WORK(&ks->tx_work, ks8851_tx_work);
	INIT_WORK(&ks->rxctrl_work, ks8851_rxctrl_work);
	INIT_WORK(&ks->rx_work, ks8851_rx_work);


	ks8851_rx_wq = create_singlethread_workqueue(
			KS8851_RX_WORKQUEUE_NAME);
	if (!ks8851_rx_wq) {
		pr_debug("workqueue creation failed\n");
		return -ENOMEM;
	}
	ks8851_tx_wq = create_singlethread_workqueue(
			KS8851_TX_WORKQUEUE_NAME);
	if (!ks8851_tx_wq) {
		pr_debug("tx work queue creation failed");
		return -ENOMEM;
	}
	/* initialise pre-made spi transfer messages */

	spi_message_init(&ks->spi_msg1);
	spi_message_add_tail(&ks->spi_xfer1, &ks->spi_msg1);

	spi_message_init(&ks->spi_msg2);
	spi_message_add_tail(&ks->spi_xfer2[0], &ks->spi_msg2);
	spi_message_add_tail(&ks->spi_xfer2[1], &ks->spi_msg2);

	ks->spi_xfer1.delay_usecs = 0;
	ks->spi_xfer2[0].delay_usecs = 0;
	ks->spi_xfer2[1].delay_usecs = 0;

	/* setup EEPROM state */
	ks->eeprom.data = ks;
	ks->eeprom.width = PCI_EEPROM_WIDTH_93C46;
	ks->eeprom.register_read = ks8851_eeprom_regread;
	ks->eeprom.register_write = ks8851_eeprom_regwrite;

	/* setup mii state */
	ks->mii.dev = ndev;
	ks->mii.phy_id = 1,
	ks->mii.phy_id_mask = 1;
	ks->mii.reg_num_mask = 0xf;
	ks->mii.mdio_read = ks8851_phy_read;
	ks->mii.mdio_write = ks8851_phy_write;

	dev_info(&spi->dev, "message enable is %d\n", msg_enable);

	/* set the default message enable */
	ks->msg_enable = netif_msg_init(msg_enable, (NETIF_MSG_DRV |
						     NETIF_MSG_PROBE |
						     NETIF_MSG_LINK));

	skb_queue_head_init(&ks->txq);

	ndev->ethtool_ops = &ks8851_ethtool_ops;
	SET_NETDEV_DEV(ndev, &spi->dev);

	spi_set_drvdata(spi, ks);

	ndev->if_port = IF_PORT_100BASET;
	ndev->netdev_ops = &ks8851_netdev_ops;
	ndev->irq = spi->irq;

	/* issue a global soft reset to reset the device. */
	ks8851_soft_reset(ks, GRR_GSR);
	/* Set SCLK for falling edge MISO (Chip Rev A3 only) */
	/*ks8851_wrreg16(ks, KS_OBCR, 0x08);*/
	/* simple check for a valid chip being connected to the bus */
	for (iter = 0; iter <= CIDER_READ_MAX_ITER; iter++) {
		cider = ks8851_32bitrdreg16(ks, KS_CIDER);
		if ((cider & ~CIDER_REV_MASK) == CIDER_ID) {
			pr_debug("value for cider received as %08X\n", cider);
			break;
		}
		msleep(CIDER_READ_MAX_DELAY);
	}
	pr_debug("###################################\n");
	pr_debug("## eth: spi Chip ID Ox:%08X  ##\n", cider);
	pr_debug("###################################\n");
	if ((cider & ~CIDER_REV_MASK) != CIDER_ID) {
		dev_err(&spi->dev, "failed to read device ID\n");
		ret = -ENODEV;
		goto err_id;
	}

	/* cache the contents of the CCR register for EEPROM, etc. */
	ks->rc_ccr = ks8851_32bitrdreg16(ks, KS_CCR);

	if (ks->rc_ccr & CCR_EEPROM)
		ks->eeprom_size = 128;
	else
		ks->eeprom_size = 0;

	ks->eeprom_size = 0;
	ks8851_read_selftest(ks);
	ks8851_init_mac(ks);

	ret = request_threaded_irq(spi->irq, NULL, ks8851_irq,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				   ndev->name, ks);
	if (ret < 0) {
		dev_err(&spi->dev, "failed to get irq\n");
		goto err_irq;
	}

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(&spi->dev, "failed to register network device\n");
		goto err_netdev;
	}

	netdev_info(ndev, "revision %d, MAC %pM, IRQ %d, %s EEPROM\n",
		    CIDER_REV_GET(cider), ndev->dev_addr, ndev->irq,
		    ks->rc_ccr & CCR_EEPROM ? "has" : "no");

	return 0;


err_netdev:
	free_irq(ndev->irq, ks);

err_irq:
	if (gpio_is_valid(gpio))
		gpio_set_value(gpio, 0);
err_id:

err_gpio:
	free_netdev(ndev);
	return ret;
}

static int ks8851_remove(struct spi_device *spi)
{
	struct ks8851_net *priv = spi_get_drvdata(spi);

	if (netif_msg_drv(priv))
		dev_info(&spi->dev, "remove\n");

	unregister_netdev(priv->netdev);
	free_irq(spi->irq, priv);
	if (gpio_is_valid(priv->gpio))
		gpio_set_value(priv->gpio, 0);
	regulator_disable(priv->vdd_reg);
	regulator_disable(priv->vdd_io);
	free_netdev(priv->netdev);

	return 0;
}

static const struct of_device_id ks8851_match_table[] = {
	{ .compatible = "micrel,ks8851" },
	{ }
};

static struct spi_driver ks8851_driver = {
	.driver = {
		.name = "ks8851",
		.of_match_table = ks8851_match_table,
		.owner = THIS_MODULE,
		.pm = &ks8851_pm_ops,
	},
	.probe = ks8851_probe,
	.remove = ks8851_remove,
};
module_spi_driver(ks8851_driver);

MODULE_DESCRIPTION("KS8851 Network driver");
MODULE_AUTHOR("Ben Dooks <ben@simtec.co.uk>");
MODULE_LICENSE("GPL");

module_param_named(message, msg_enable, int, 0);
MODULE_PARM_DESC(message, "Message verbosity level (0=none, 31=all)");
MODULE_ALIAS("spi:ks8851");
