/*
 * Copyright (c) 2001-2008, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _IF_EM_H_
#define _IF_EM_H_

/* Tunables */

/*
 * EM_TXD: Maximum number of Transmit Descriptors
 * Valid Range: 80-256 for 82542 and 82543-based adapters
 *              80-4096 for others
 * Default Value: 256
 *   This value is the number of transmit descriptors allocated by the driver.
 *   Increasing this value allows the driver to queue more transmits. Each
 *   descriptor is 16 bytes.
 *   Since TDLEN should be multiple of 128bytes, the number of transmit
 *   desscriptors should meet the following condition.
 *      (num_tx_desc * sizeof(struct e1000_tx_desc)) % 128 == 0
 */
#define EM_MIN_TXD			80
#define EM_MAX_TXD_82543		256
#define EM_MAX_TXD			4096
#define EM_DEFAULT_TXD			EM_MAX_TXD_82543

/*
 * EM_RXD - Maximum number of receive Descriptors
 * Valid Range: 80-256 for 82542 and 82543-based adapters
 *              80-4096 for others
 * Default Value: 256
 *   This value is the number of receive descriptors allocated by the driver.
 *   Increasing this value allows the driver to buffer more incoming packets.
 *   Each descriptor is 16 bytes.  A receive buffer is also allocated for each
 *   descriptor. The maximum MTU size is 16110.
 *   Since TDLEN should be multiple of 128bytes, the number of transmit
 *   desscriptors should meet the following condition.
 *      (num_tx_desc * sizeof(struct e1000_tx_desc)) % 128 == 0
 */
#define EM_MIN_RXD			80
#define EM_MAX_RXD_82543		256
#define EM_MAX_RXD			4096
#define EM_DEFAULT_RXD			EM_MAX_RXD_82543

/*
 * EM_TIDV - Transmit Interrupt Delay Value
 * Valid Range: 0-65535 (0=off)
 * Default Value: 64
 *   This value delays the generation of transmit interrupts in units of
 *   1.024 microseconds. Transmit interrupt reduction can improve CPU
 *   efficiency if properly tuned for specific network traffic. If the
 *   system is reporting dropped transmits, this value may be set too high
 *   causing the driver to run out of available transmit descriptors.
 */
#define EM_TIDV				64

/*
 * EM_TADV - Transmit Absolute Interrupt Delay Value
 * (Not valid for 82542/82543/82544)
 * Valid Range: 0-65535 (0=off)
 * Default Value: 64
 *   This value, in units of 1.024 microseconds, limits the delay in which a
 *   transmit interrupt is generated. Useful only if EM_TIDV is non-zero,
 *   this value ensures that an interrupt is generated after the initial
 *   packet is sent on the wire within the set amount of time.  Proper tuning,
 *   along with EM_TIDV, may improve traffic throughput in specific
 *   network conditions.
 */
#define EM_TADV				64

/*
 * EM_RDTR - Receive Interrupt Delay Timer (Packet Timer)
 * Valid Range: 0-65535 (0=off)
 * Default Value: 0
 *   This value delays the generation of receive interrupts in units of 1.024
 *   microseconds.  Receive interrupt reduction can improve CPU efficiency if
 *   properly tuned for specific network traffic. Increasing this value adds
 *   extra latency to frame reception and can end up decreasing the throughput
 *   of TCP traffic. If the system is reporting dropped receives, this value
 *   may be set too high, causing the driver to run out of available receive
 *   descriptors.
 *
 *   CAUTION: When setting EM_RDTR to a value other than 0, adapters
 *            may hang (stop transmitting) under certain network conditions.
 *            If this occurs a WATCHDOG message is logged in the system
 *            event log. In addition, the controller is automatically reset,
 *            restoring the network connection. To eliminate the potential
 *            for the hang ensure that EM_RDTR is set to 0.
 */
#define EM_RDTR				0

/*
 * Receive Interrupt Absolute Delay Timer (Not valid for 82542/82543/82544)
 * Valid Range: 0-65535 (0=off)
 * Default Value: 64
 *   This value, in units of 1.024 microseconds, limits the delay in which a
 *   receive interrupt is generated. Useful only if EM_RDTR is non-zero,
 *   this value ensures that an interrupt is generated after the initial
 *   packet is received within the set amount of time.  Proper tuning,
 *   along with EM_RDTR, may improve traffic throughput in specific network
 *   conditions.
 */
#define EM_RADV				64

/*
 * This parameter controls the duration of transmit watchdog timer.
 */
#define EM_TX_TIMEOUT			5

/* One for TX csum offloading desc, the other is reserved */
#define EM_TX_RESERVED			2

/* Large enough for 16K jumbo frame */
#define EM_TX_SPARE			8

/* Interrupt throttle rate */
#define EM_DEFAULT_ITR			10000

/*
 * This parameter controls when the driver calls the routine to reclaim
 * transmit descriptors.
 */
#define EM_TX_CLEANUP_THRESHOLD		(adapter->num_tx_desc / 8)
#define EM_TX_OP_THRESHOLD		(adapter->num_tx_desc / 32)

/*
 * This parameter controls whether or not autonegotation is enabled.
 *              0 - Disable autonegotiation
 *              1 - Enable  autonegotiation
 */
#define DO_AUTO_NEG			1

/*
 * This parameter control whether or not the driver will wait for
 * autonegotiation to complete.
 *              1 - Wait for autonegotiation to complete
 *              0 - Don't wait for autonegotiation to complete
 */
#define WAIT_FOR_AUTO_NEG_DEFAULT	0

/* Tunables -- End */

#define AUTONEG_ADV_DEFAULT		(ADVERTISE_10_HALF | \
					 ADVERTISE_10_FULL | \
					 ADVERTISE_100_HALF | \
					 ADVERTISE_100_FULL | \
					 ADVERTISE_1000_FULL)

#define AUTO_ALL_MODES			0

/* PHY master/slave setting */
#define EM_MASTER_SLAVE			e1000_ms_hw_default

/*
 * Micellaneous constants
 */
#define EM_VENDOR_ID			0x8086

#define EM_BAR_MEM			PCIR_BAR(0)
#define EM_BAR_FLASH			PCIR_BAR(1)

#define EM_JUMBO_PBA			0x00000028
#define EM_DEFAULT_PBA			0x00000030
#define EM_SMARTSPEED_DOWNSHIFT		3
#define EM_SMARTSPEED_MAX		15
#define EM_MAX_INTR			10

#define MAX_NUM_MULTICAST_ADDRESSES	128
#define PCI_ANY_ID			(~0U)
#define EM_FC_PAUSE_TIME		1000
#define EM_EEPROM_APME			0x400;

/*
 * TDBA/RDBA should be aligned on 16 byte boundary. But TDLEN/RDLEN should be
 * multiple of 128 bytes. So we align TDBA/RDBA on 128 byte boundary. This will
 * also optimize cache line size effect. H/W supports up to cache line size 128.
 */
#define EM_DBA_ALIGN			128

#define SPEED_MODE_BIT			(1 << 21) /* On PCI-E MACs only */

/* PCI Config defines */
#define EM_BAR_TYPE(v)			((v) & EM_BAR_TYPE_MASK)
#define EM_BAR_TYPE_MASK		0x00000001
#define EM_BAR_TYPE_MMEM		0x00000000
#define EM_BAR_TYPE_IO			0x00000001
#define EM_BAR_MEM_TYPE(v)		((v) & EM_BAR_MEM_TYPE_MASK)
#define EM_BAR_MEM_TYPE_MASK		0x00000006
#define EM_BAR_MEM_TYPE_32BIT		0x00000000
#define EM_BAR_MEM_TYPE_64BIT		0x00000004

#define EM_MAX_SCATTER			64
#define EM_TSO_SIZE			(65535 + \
					 sizeof(struct ether_vlan_header))
#define EM_MAX_SEGSIZE			4096
#define EM_MSIX_MASK			0x01F00000 /* For 82574 use */
#define ETH_ZLEN			60

#define EM_CSUM_FEATURES		(CSUM_IP | CSUM_TCP | CSUM_UDP)
#define CSUM_OFFLOAD			(CSUM_IP | CSUM_TCP | CSUM_UDP)

/*
 * 82574 has a nonstandard address for EIAC
 * and since its only used in MSIX, and in
 * the em driver only 82574 uses MSIX we can
 * solve it just using this define.
 */
#define EM_EIAC				0x000DC

/* Used in for 82547 10Mb Half workaround */
#define EM_PBA_BYTES_SHIFT		0xA
#define EM_TX_HEAD_ADDR_SHIFT		7
#define EM_PBA_TX_MASK			0xFFFF0000
#define EM_FIFO_HDR			0x10
#define EM_82547_PKT_THRESH		0x3e0

struct adapter;

struct em_int_delay_info {
	struct adapter	*adapter;	/* Back-pointer to the adapter struct */
	int		offset;		/* Register offset to read/write */
	int		value;		/* Current value in usecs */
};

/*
 * Bus dma allocation structure used by
 * e1000_dma_malloc and e1000_dma_free.
 */
struct em_dma_alloc {
	bus_addr_t		dma_paddr;
	void			*dma_vaddr;
	bus_dma_tag_t		dma_tag;
	bus_dmamap_t		dma_map;
};

/* Our adapter structure */
struct adapter {
	struct arpcom		arpcom;
	struct e1000_hw		hw;

	/* DragonFly operating-system-specific structures. */
	struct e1000_osdep	osdep;
	device_t		dev;

	bus_dma_tag_t		parent_dtag;

	struct resource		*memory;
	int			memory_rid;
	struct resource		*flash;
	int			flash_rid;

	struct resource		*ioport;
	int			io_rid;

	struct resource		*intr_res;
	void			*intr_tag;
	int			intr_rid;

	struct ifmedia		media;
	struct callout		timer;
	struct callout		tx_fifo_timer;
	int			if_flags;
	int			max_frame_size;
	int			min_frame_size;

	/* Management and WOL features */
	int			wol;
	int			has_manage;

	/* Info about the board itself */
	uint8_t			link_active;
	uint16_t		link_speed;
	uint16_t		link_duplex;
	uint32_t		smartspeed;
	struct em_int_delay_info tx_int_delay;
	struct em_int_delay_info tx_abs_int_delay;
	struct em_int_delay_info rx_int_delay;
	struct em_int_delay_info rx_abs_int_delay;
	int			int_throttle_ceil;

	/*
	 * Transmit definitions
	 *
	 * We have an array of num_tx_desc descriptors (handled
	 * by the controller) paired with an array of tx_buffers
	 * (at tx_buffer_area).
	 * The index of the next available descriptor is next_avail_tx_desc.
	 * The number of remaining tx_desc is num_tx_desc_avail.
	 */
	struct em_dma_alloc	txdma;		/* bus_dma glue for tx desc */
	struct e1000_tx_desc	*tx_desc_base;
	uint32_t		next_avail_tx_desc;
	uint32_t		next_tx_to_clean;
	int			num_tx_desc_avail;
	int			num_tx_desc;
	uint32_t		txd_cmd;
	struct em_buffer	*tx_buffer_area;
	bus_dma_tag_t		txtag;		/* dma tag for tx */
	int			spare_tx_desc;

	/* 
	 * Receive definitions
	 *
	 * we have an array of num_rx_desc rx_desc (handled by the
	 * controller), and paired with an array of rx_buffers
	 * (at rx_buffer_area).
	 * The next pair to check on receive is at offset next_rx_desc_to_check
	 */
	struct em_dma_alloc	rxdma;		/* bus_dma glue for rx desc */
	struct e1000_rx_desc	*rx_desc_base;
	uint32_t		next_rx_desc_to_check;
	uint32_t		rx_buffer_len;
	int			num_rx_desc;
	struct em_buffer	*rx_buffer_area;
	bus_dma_tag_t		rxtag;
	bus_dmamap_t		rx_sparemap;

	/*
	 * First/last mbuf pointers, for
	 * collecting multisegment RX packets.
	 */
	struct mbuf		*fmp;
	struct mbuf		*lmp;

	/* Misc stats maintained by the driver */
	unsigned long		dropped_pkts;
	unsigned long		mbuf_alloc_failed;
	unsigned long		mbuf_cluster_failed;
	unsigned long		no_tx_desc_avail1;
	unsigned long		no_tx_desc_avail2;
	unsigned long		no_tx_map_avail;
	unsigned long		no_tx_dma_setup;
	unsigned long		watchdog_events;
	unsigned long		rx_overruns;
	unsigned long		rx_irq;
	unsigned long		tx_irq;
	unsigned long		link_irq;

	/* sysctl tree glue */
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;

	/* 82547 workaround */
	uint32_t		tx_fifo_size;
	uint32_t		tx_fifo_head;
	uint32_t		tx_fifo_head_addr;
	uint64_t		tx_fifo_reset_cnt;
	uint64_t		tx_fifo_wrk_cnt;
	uint32_t		tx_head_addr;

        /* For 82544 PCIX Workaround */
	boolean_t		pcix_82544;
	boolean_t		in_detach;

	struct e1000_hw_stats	stats;
};

struct em_vendor_info {
	uint16_t	vendor_id;
	uint16_t	device_id;
	const char	*desc;
};

struct em_buffer {
	int		next_eop;	/* Index of the desc to watch */
	struct mbuf	*m_head;
	bus_dmamap_t	map;		/* bus_dma map for packet */
};

/* For 82544 PCIX  Workaround */
typedef struct _ADDRESS_LENGTH_PAIR {
	uint64_t	address;
	uint32_t	length;
} ADDRESS_LENGTH_PAIR, *PADDRESS_LENGTH_PAIR;

typedef struct _DESCRIPTOR_PAIR {
	ADDRESS_LENGTH_PAIR descriptor[4];
	uint32_t	elements;
} DESC_ARRAY, *PDESC_ARRAY;

#define EM_IS_OACTIVE(adapter) \
	((adapter)->num_tx_desc_avail < \
	 (adapter)->spare_tx_desc + EM_TX_RESERVED)

#endif /* _IF_EM_H_ */
