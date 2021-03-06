/*******************************************************************************
 *
 *   Copyright (c) 2009 Cavium Networks 
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by the Free
 *   Software Foundation; either version 2 of the License, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful, but WITHOUT
 *   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *   more details.
 *
 *   You should have received a copy of the GNU General Public License along with
 *   this program; if not, write to the Free Software Foundation, Inc., 59
 *   Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *   The full GNU General Public License is included in this distribution in the
 *   file called LICENSE.
 *
 ********************************************************************************/

#include <linux/module.h>
#include <mach/cns3xxx.h>

#include "cns3xxx.h"
#include "cns3xxx_tool.h"
#include "cns3xxx_config.h"
#include <linux/cns3xxx/pse_init_common.h>

#ifdef CONFIG_SUSPEND
#include <linux/suspend.h>
#endif

#if defined (CONFIG_CNS3XXX_SPPE)
#include <linux/cns3xxx/sppe.h>
#define PACKET_REASON_TO_CPU (0x2C)
#endif

#if defined (CONFIG_CNS3XXX_ETHADDR_IN_FLASH)
#include <linux/mtd/mtd.h>
#endif


#if defined(CONFIG_MACH_TS43XX)
extern unsigned long *getFlashOTP(void);
#endif

extern NetDevicePriv net_device_prive[10];

#define RX_SDP_ALIGN 64

ushort MAX_PACKET_LEN = 1536;
#define CPU_CACHE_BYTES         64
#define CPU_CACHE_ALIGN(X)      (((X) + (CPU_CACHE_BYTES-1)) & ~(CPU_CACHE_BYTES-1))


#define QUEUE_WEIGHT_SET(port, ctl) \
{ \
	MAC##port##_PRI_CTRL_REG &= ~(0x3ffff); \
	MAC##port##_PRI_CTRL_REG |= (ctl.sch_mode << 16); \
	MAC##port##_PRI_CTRL_REG |= (ctl.q0_w); \
	MAC##port##_PRI_CTRL_REG |= (ctl.q1_w << 4); \
	MAC##port##_PRI_CTRL_REG |= (ctl.q2_w << 8); \
	MAC##port##_PRI_CTRL_REG |= (ctl.q3_w << 12); \
}

#define QUEUE_WEIGHT_GET(port, ctl) \
{ \
	ctl.sch_mode = ((MAC##port##_PRI_CTRL_REG >> 16 ) & 0x3); \
	ctl.q0_w = ((MAC##port##_PRI_CTRL_REG >> 0 ) & 0x7); \
	ctl.q1_w = ((MAC##port##_PRI_CTRL_REG >> 4 ) & 0x7); \
	ctl.q2_w = ((MAC##port##_PRI_CTRL_REG >> 8 ) & 0x7); \
	ctl.q3_w = ((MAC##port##_PRI_CTRL_REG >> 12 ) & 0x7); \
}

#ifdef CONFIG_CPU_FREQ
#include <linux/cpufreq.h>
atomic_t cpufreq_pse_flag;
#endif

extern struct proc_dir_entry *cns3xxx_proc_dir;


#if defined (CONFIG_CNS3XXX_ETHADDR_IN_FLASH)

#define MTD_READ(mtd, args...) (*(mtd->read))(mtd, args)

#ifdef CONFIG_CNS3XXX_MAC_IN_SPI_FLASH
#define ENV_OFFSET 0x30000
#define PARTITION_NAME "SPI-UBoot"	/* refer to cns3xxx_spi_flash_partitions */
#else
#define ENV_OFFSET 0x0
#define PARTITION_NAME "UBootEnv"
#endif

#define MTD_READ_LEN 1024

static char mtd_str[MTD_READ_LEN], ethaddr[12]={0};

static int init_mtd_env(void)
{
	struct mtd_info *mtd;
	size_t retlen=0;

	mtd = get_mtd_device_nm(PARTITION_NAME);

	if (IS_ERR(mtd)) { return -ENODEV; }
	
	MTD_READ(mtd, ENV_OFFSET, MTD_READ_LEN, &retlen, mtd_str);

	return 0;
}

int fmg_get(const char *name, int *ret_len)
{
	int i,j,x,z;
	int nlen = strlen(name);
	char tmp_str[20];

	memset(ethaddr,0x0,12);

	for(i=0;i<MTD_READ_LEN-nlen;i++) {
		z=0;
		for(x=0;x<nlen;x++){if(mtd_str[i+x]==name[x]){z++;}}
		/* printk("z = %d , nlen = %d\n",z,nlen); */
		if(z==nlen) {
			memcpy(tmp_str,mtd_str+i+nlen,17);
			tmp_str[17] = '\0';
			/* printk("tmp_str = [%s]\n",tmp_str); */
			for(j=0;j<17;j++){if(tmp_str[j]!=':'){sprintf(ethaddr,"%s%c",ethaddr,tmp_str[j]);}}
			*ret_len = strlen(ethaddr);
			return 0;
		}
	}
	return -1;
}

int mac_str_to_int(const char *mac_str, int mac_str_len, u8 *mac_int, int mac_len)
{
	int i=0,j=0;
	char mac_s[3]={0,0,0};

	for (i=0 ; i < mac_str_len ; i+=2) {
		mac_s[0] = mac_str[i];
		mac_s[1] = mac_str[i+1];
		mac_int[j++] = simple_strtol(mac_s, NULL, 16);
	}
	return 0;
}

#endif






int cns3xxx_send_packet(struct sk_buff *skb, struct net_device *netdev);
static int install_isr_rc = 0;
static int rc_setup_rx_tx = 0; // rc means reference counting.
static struct net_device *intr_netdev;
struct net_device *net_dev_array[NETDEV_SIZE]; 
spinlock_t tx_lock;
spinlock_t rx_lock;
u8 fast_bridge_en=1;
u8 show_rx_proc=0;
u8 show_tx_proc=0;

const int MAX_RX_DESC_SIZE = 512;
const int MAX_TX_DESC_SIZE = 512;
int RX_DESC_SIZE = 128;
int TX_DESC_SIZE = 120;
module_param(RX_DESC_SIZE, int, 0);
module_param(TX_DESC_SIZE, int, 0);

u8 ring_index=0; // 0 or 1

#ifdef CNS3XXX_DELAYED_INTERRUPT
static u32 max_pend_int_cnt=MAX_PEND_INT_CNT, max_pend_time=MAX_PEND_TIME;
#endif

#ifdef CONFIG_CNS3XXX_NAPI
struct net_device *napi_dev;
	#ifdef CNS3XXX_DOUBLE_RX_RING
	struct net_device *r1_napi_dev; // ring1 napi dev
	#endif
#endif


//RXRing *rx_ring;
//TXRing *tx_ring;

// only for debug (proc)
RingInfo g_ring_info; 

int MSG_LEVEL = NORMAL_MSG;

#ifdef CNS3XXX_STATUS_ISR
const char *cns3xxx_gsw_status_tbl[] = {
	"\nMAC0_Q_FULL\n",
	"\nMAC1_Q_FULL\n",
	"\nCPU_Q_FULL\n",
	"\nHNAT_Q_FULL\n",
	"\nMAC2_Q_FULL\n",
	"\nMAC0_Q_EXT_FULL\n",
	"\nGLOBAL_Q_FULL\n",
	"\nBUFFER_FULL\n",
	"\nMIB_COUNTER_TH\n",
	"\n", // 9
	"\nMAC0_INTRUDER\n",
	"\nMAC1_INTRUDER\n",
	"\nCPU_INTRUDER\n",
	"\nMAC2_INTRUDER\n",
	"\nMAC0_STATUS_CHG\n",
	"\nMAC1_STATUS_CHG\n",
	"\nMAC2_STATUS_CHG\n",
	"\nMAC0_NO_LINK_DROP\n",
	"\nMAC1_NO_LINK_DROP\n",
	"\nMAC2_NO_LINK_DROP\n",
	"\nMAC0_RX_ERROR_DROP\n",
	"\nMAC1_RX_ERROR_DROP\n",
	"\nMAC2_RX_ERROR_DROP\n",
	"\nMAC0_NO_DESTINATION_DROP\n",
	"\nMAC1_NO_DESTINATION_DROP\n",
	"\nMAC2_NO_DESTINATION_DROP\n",
	"\nMAC0_RMC_PAUSE_DROP\n",
	"\nMAC1_RMC_PAUSE_DROP\n",
	"\nMAC2_RMC_PAUSE_DROP\n",
	"\nMAC0_LOCAL_DROP\n",
	"\nMAC1_LOCAL_DROP\n",
	"\nMAC2_LOCAL_DROP\n",
};
#endif

#define MIN_PACKET_LEN 14

void cns3xxx_write_pri_mask(u8 pri_mask);

static int cns3xxx_notify_reboot(struct notifier_block *nb, unsigned long event, void *ptr);

static struct notifier_block cns3xxx_notifier_reboot = {
	.notifier_call	= cns3xxx_notify_reboot,
	.next		= NULL,
	.priority	= 0
};

#if defined(CNS3XXX_VLAN_8021Q)
void cns3xxx_vlan_rx_kill_vid(struct net_device *dev, unsigned short vid);
void cns3xxx_vlan_rx_register(struct net_device *dev, struct vlan_group *grp);
#endif

void __take_off_vlan_header(struct sk_buff *skb)
{
	// take off VLAN header
	memmove(skb->data + 4, skb->data, 12);
#if 0
	//skb_ptr->data += 4; 
	skb_reserve(skb, 4);
#else
	skb->data += 4;
#endif
	skb->len -= 4; // minus 4 byte vlan tag
}

#if 0
int rx_port_base(struct sk_buff *skb, RXDesc *rx_desc_ptr, const struct CNS3XXXPrivate_ *priv)
{
	if (skb->data[12] == 0x81 && skb->data[13] == 0x00)  // VLAN header
	{
		printk("__take_off_vlan_header\n");
		__take_off_vlan_header(skb);
		print_packet(skb->data, skb->len);
	}
	return 0;
}

int rx_vlan_base(struct sk_buff *skb, RXDesc *rx_desc_ptr, const struct CNS3XXXPrivate_ *priv)
{
	return 0;
}

int tx_port_base(TXDesc *tx_desc_ptr, const struct CNS3XXXPrivate_ *priv, struct sk_buff *skb)
{
#if defined(CNS3XXX_VLAN_8021Q) && defined (CNS3XXX_8021Q_HW_TX)
        if (skb && priv->vlgrp != NULL && vlan_tx_tag_present(skb)) 
	{
		tx_desc_ptr->c_vid = cpu_to_le16(vlan_tx_tag_get(skb));
		tx_desc_ptr->ctv=1;
		tx_desc_ptr->fr	= 0;

        }
	else 
#endif
	{
		tx_desc_ptr->ctv = 0;
		tx_desc_ptr->pmap = priv->net_device_priv->pmap;
		tx_desc_ptr->fr	= 1;
	}

	return 0;
}


int tx_vlan_base(TXDesc *tx_desc_ptr, const struct CNS3XXXPrivate_ *priv, struct sk_buff *skb)
{
#if defined(CNS3XXX_VLAN_8021Q)

        if (skb && priv->vlgrp != NULL && vlan_tx_tag_present(skb)) {
		tx_desc_ptr->c_vid = cpu_to_le16(vlan_tx_tag_get(skb));
        }
#else
	tx_desc_ptr->c_vid = priv->net_device_priv->vlan_tag;

#endif
	tx_desc_ptr->ctv=1;
	tx_desc_ptr->fr	= 0;

	return 0;
}

#if defined (CONFIG_CNS3XXX_SPPE)
int fp_port_base(TXDesc *tx_desc_ptr, const struct CNS3XXXPrivate_ *priv, struct sk_buff *skb)
{
#if 1
	tx_desc_ptr->fr = 1;
	tx_desc_ptr->pmap = 0x8;
#else
	tx_desc_ptr->fr = 0;
	tx_desc_ptr->ctv = 1;
	tx_desc_ptr->c_vid = 80;
#endif
    return 0;
}
#endif
#endif

static struct sk_buff *cns3xxx_alloc_skb(void)
{
	struct sk_buff *skb;
	u32 align_64;

	skb = dev_alloc_skb(MAX_PACKET_LEN + 2 + RX_SDP_ALIGN);

	if (unlikely(!skb)) {
		printk("\n dev_alloc_skb fail!! while allocate RFD ring !!\n");
		return NULL;
	}
//	pci_dma_sync_single_for_device(NULL, virt_to_phys(skb->data), MAX_PACKET_LEN+2+RX_SDP_ALIGN, PCI_DMA_FROMDEVICE);

	//printk("skb->data: %x\n", skb->data);
	align_64=CPU_CACHE_ALIGN((u32)skb->data);
	//printk("align_64: %x\n", align_64);
	//printk("align_64-(u32)skb->data = %x\n", align_64-(u32)skb->data);
	skb_reserve(skb, align_64-(u32)skb->data);	/* 16 bytes alignment */

#ifndef CNS3XXX_4N
	skb_reserve(skb, NET_IP_ALIGN);	/* 16 bytes alignment */
#endif
	//printk("xxx skb->data: %x\n", skb->data);



	return skb;
}

static int free_rx_skb(RXRing *rx_ring)
{
	int i=0;
	RXBuffer *rx_buffer = rx_ring->head;
        //RXDesc *rx_desc = rx_ring.rx_desc_head_vir_addr;

	for (i=0 ; i < get_rx_ring_size(rx_ring) ; ++i) {
		if (rx_buffer->skb) {
			dma_unmap_single(NULL,
					 rx_buffer->rx_desc->sdp,
					 rx_buffer->rx_desc->sdl,
					 DMA_FROM_DEVICE);
			dev_kfree_skb(rx_buffer->skb);
			rx_buffer->skb=0;
		}
		++rx_buffer;
	}
	return 0;
}

int cns3xxx_setup_all_rx_resources(RXRing *rx_ring, u8 ring_num)
{
	int i=0;
	RXBuffer *rx_buffer = 0;
        RXDesc *rx_desc = 0;
	dma_addr_t dmap;

#ifdef NCNB_TEST
	ncnb_buf = dma_alloc_coherent(NULL, 2*1024* get_rx_ring_size(rx_ring), &ncnb_buf_phy, GFP_KERNEL);
	printk("NCB_BUF: %08X PHY: %08X \n", ncnb_buf, ncnb_buf_phy);
	
#endif

	// alloc RXDesc array
	rx_ring->rx_desc_head_vir_addr = dma_alloc_coherent(NULL, sizeof(RXDesc) * (get_rx_ring_size(rx_ring)), &rx_ring->rx_desc_head_phy_addr, GFP_KERNEL);
	if (!rx_ring->rx_desc_head_vir_addr) {
		printk("rx_ring->rx_desc_head_vir_addr alloc memory fail!\n");
		return -ENOMEM;
	}

	memset(rx_ring->rx_desc_head_vir_addr, 0, sizeof(RXDesc) * get_rx_ring_size(rx_ring));

	// alloc RXBuffer array
	rx_ring->head = kmalloc(sizeof(RXBuffer) * get_rx_ring_size(rx_ring), GFP_KERNEL);
	
	if (!rx_ring->head) {
		printk("rx_ring.head alloc memory fail!\n");
		return -ENOMEM;
	}

	rx_buffer = rx_ring->head;
	for (i=0 ; i < get_rx_ring_size(rx_ring) ; ++i) {
		rx_buffer->skb=0;
		++rx_buffer;
	}

	rx_buffer = rx_ring->head;
        rx_desc = rx_ring->rx_desc_head_vir_addr;
	for (i=0 ; i < get_rx_ring_size(rx_ring) ; ++i, ++rx_buffer, ++rx_desc) {
		rx_buffer->rx_desc = rx_desc;
		rx_buffer->skb = cns3xxx_alloc_skb();

		if (!rx_buffer->skb) {
			
			free_rx_skb(rx_ring);
			kfree(rx_ring->head);
			dma_free_coherent(NULL, sizeof(RXDesc) * get_rx_ring_size(rx_ring), rx_ring->rx_desc_head_vir_addr, rx_ring->rx_desc_head_phy_addr);
			printk("cannot alloc rx skb!!");
			return -ENOMEM;
		}

#ifdef CONFIG_SWITCH_BIG_ENDIAN
	{
		RXDesc tmp_rx_desc;

                memset(&tmp_rx_desc, 0, sizeof(RXDesc));
//                tmp_rx_desc.sdp = (u32)virt_to_phys(rx_buffer->skb->data);
                tmp_rx_desc.sdl = MAX_PACKET_LEN;
                if (i == (get_rx_ring_size(rx_ring)-1) ){
                        tmp_rx_desc.eor = 1;
                }
                tmp_rx_desc.fsd = 1;
                tmp_rx_desc.lsd = 1;
                swap_rx_desc(&tmp_rx_desc, rx_buffer->rx_desc);
	}

#else
//		rx_buffer->rx_desc->sdp = (u32)virt_to_phys(rx_buffer->skb->data);
		rx_buffer->rx_desc->sdl = MAX_PACKET_LEN;
		if (i == (get_rx_ring_size(rx_ring)-1) ){
			rx_buffer->rx_desc->eor = 1;
		}
		rx_buffer->rx_desc->fsd = 1;
		rx_buffer->rx_desc->lsd = 1;
#endif

		dmap = dma_map_single(NULL, rx_buffer->skb->data, MAX_PACKET_LEN, DMA_FROM_DEVICE);
		rx_buffer->rx_desc->sdp = dmap;

	}
	rx_ring->cur_index = 0 ;

	if (ring_num == 0){
		FS_DESC_PTR0_REG = rx_ring->rx_desc_head_phy_addr;
		FS_DESC_BASE_ADDR0_REG = rx_ring->rx_desc_head_phy_addr;

	} else if (ring_num == 1){
		FS_DESC_PTR1_REG = rx_ring->rx_desc_head_phy_addr;
		FS_DESC_BASE_ADDR1_REG = rx_ring->rx_desc_head_phy_addr;
	}

	return CAVM_OK;
}

static int cns3xxx_setup_all_tx_resources(TXRing *tx_ring, u8 ring_num)
{
	int i=0;
	TXBuffer *tx_buffer = 0;
	TXDesc *tx_desc = 0;

	spin_lock_init(&(tx_ring->tx_lock));

	tx_ring->tx_desc_head_vir_addr = dma_alloc_coherent(NULL, sizeof(TXDesc) * get_tx_ring_size(tx_ring), &tx_ring->tx_desc_head_phy_addr, GFP_KERNEL);
	if (!tx_ring->tx_desc_head_vir_addr) {
		printk("tx_ring->tx_desc_head_vir_addr alloc memory fail!\n");
		return -ENOMEM;
	}

	memset(tx_ring->tx_desc_head_vir_addr, 0, sizeof(TXDesc) * get_tx_ring_size(tx_ring));
	tx_ring->head = kmalloc(sizeof(TXBuffer) * get_tx_ring_size(tx_ring), GFP_KERNEL);

	tx_buffer = tx_ring->head;
	tx_desc = tx_ring->tx_desc_head_vir_addr;

	for (i=0 ; i < get_tx_ring_size(tx_ring) ; ++i, ++tx_buffer, ++tx_desc) {
		//printk("%d : ## tx_buffer %p ## tx_desc : %p\n", i, tx_buffer, tx_desc);
		tx_buffer->tx_desc = tx_desc;

		tx_buffer->tx_desc->cown = 1;
		tx_buffer->skb = 0;
		if (i == (get_tx_ring_size(tx_ring)-1) ){
			tx_buffer->tx_desc->eor = 1;
		}
#ifdef CONFIG_SWITCH_BIG_ENDIAN
		swap_tx_desc(tx_buffer->tx_desc, tx_buffer->tx_desc);
#endif

	}
	tx_ring->num_free_desc = get_tx_ring_size(tx_ring);
	tx_ring->cur_index = 0 ;

	if (ring_num == 0){
		TS_DESC_PTR0_REG = tx_ring->tx_desc_head_phy_addr;
		TS_DESC_BASE_ADDR0_REG = tx_ring->tx_desc_head_phy_addr;
	} else if (ring_num == 1){
		TS_DESC_PTR1_REG = tx_ring->tx_desc_head_phy_addr;
		TS_DESC_BASE_ADDR1_REG = tx_ring->tx_desc_head_phy_addr;
	}
	return CAVM_OK;
}

int cns3xxx_free_all_rx_resources(RXRing *rx_ring)
{
	free_rx_skb(rx_ring);
	kfree(rx_ring->head);
	dma_free_coherent(NULL, sizeof(RXDesc) * get_rx_ring_size(rx_ring), rx_ring->rx_desc_head_vir_addr, rx_ring->rx_desc_head_phy_addr);
	return 0;
}

static int free_tx_skb(TXRing *tx_ring)
{
	int i=0;
	TXBuffer *tx_buffer = tx_ring->head;

	for (i=0 ; i < get_tx_ring_size(tx_ring) ; ++i) {
		if (tx_buffer->skb) {
			dma_unmap_single(NULL,
					 tx_buffer->tx_desc->sdp,
					 tx_buffer->tx_desc->sdl,
					 DMA_TO_DEVICE);

			dev_kfree_skb(tx_buffer->skb);
			tx_buffer->skb = 0;
		}
		++tx_buffer;
	}
	return 0;
}

int cns3xxx_free_all_tx_resources(TXRing *tx_ring)
{
	free_tx_skb(tx_ring);
	kfree(tx_ring->head);
	dma_free_coherent(NULL, sizeof(TXDesc) * get_tx_ring_size(tx_ring), tx_ring->tx_desc_head_vir_addr, tx_ring->tx_desc_head_phy_addr);
	return 0;
}

static int cns3xxx_free_rx_tx_res(CNS3XXXPrivate *priv)
{
	int i=0;

	--rc_setup_rx_tx;
	if (rc_setup_rx_tx == 0) {
		printk("free tx/rx resource\n");
		enable_port(3, 0); // disable cpu port

		for (i=0 ; i < priv->num_rx_queues ; ++i) {
			// stop RX dma
			enable_rx_dma(i, 0);
			cns3xxx_free_all_rx_resources(priv->rx_ring+i);
			memset(priv->rx_ring + i, 0, sizeof(RXRing));
		}

		for (i=0 ; i < priv->num_tx_queues ; ++i) {
			// stop TX dma
			enable_tx_dma(i, 0);
			cns3xxx_free_all_tx_resources(priv->tx_ring+i);
			memset(priv->tx_ring + i, 0, sizeof(TXRing));
		}
		
	}
	return 0;
}


static int cns3xxx_setup_rx_tx_res(CNS3XXXPrivate *priv)
{
	int i=0;

	if (rc_setup_rx_tx == 0) {
		printk("alloc tx/rx resource\n");
		clear_fs_dma_state(1);
		FS_DESC_PTR0_REG = 0;
		FS_DESC_BASE_ADDR0_REG = 0;
		FS_DESC_PTR1_REG = 0;
		FS_DESC_BASE_ADDR1_REG = 0;
		TS_DESC_PTR0_REG = 0;
		TS_DESC_BASE_ADDR0_REG = 0;
		TS_DESC_PTR1_REG = 0;
		TS_DESC_BASE_ADDR1_REG = 0;

		for (i=0 ; i < priv->num_tx_queues ; ++i) {
			spin_lock_init(&((priv->tx_ring+i)->tx_lock));
			(priv->tx_ring+i)->max_ring_size = MAX_TX_DESC_SIZE;
			(priv->tx_ring+i)->ring_size = TX_DESC_SIZE;
			if (cns3xxx_setup_all_tx_resources(priv->tx_ring+i, i) != CAVM_OK)
				return CAVM_ERR;
		}

		for (i=0 ; i < priv->num_rx_queues ; ++i) {
			(priv->rx_ring+i)->max_ring_size = MAX_RX_DESC_SIZE;
			(priv->rx_ring+i)->ring_size = RX_DESC_SIZE;
			if (cns3xxx_setup_all_rx_resources(priv->rx_ring+i, i) != CAVM_OK)
				return CAVM_ERR;
		
		}
		clear_fs_dma_state(0);
	}
	++rc_setup_rx_tx;
	return CAVM_OK;
}

static int free_tx_desc_skb(TXRing *tx_ring, u8 ring_num)
{
	int i;
	//u32 tssd_current=0;
	TXBuffer *tx_buffer;
	u32 tx_ring_size = get_tx_ring_size(tx_ring);
	// check curent hw index previous tx descriptor
	u32 cur_index = cns3xxx_get_tx_hw_index(ring_num) - 1; 
	TXDesc *tx_desc_ptr;
#ifdef CONFIG_SWTICH_BIG_ENDIAN
	TXDesc tmp_tx_desc;
#endif
	
	tx_buffer = get_tx_buffer_by_index(tx_ring, cur_index);

	for (i=0 ; i < tx_ring_size ; ++i) {
		//printk("cur_index : %d\n", cur_index);
		if (tx_buffer->tx_desc->cown == 1 && tx_buffer->skb) {
			//printk("tx_buffer: %p ## free skb\n", tx_buffer);
#ifdef CONFIG_SWTICH_BIG_ENDIAN
			tx_desc_ptr = &tmp_tx_desc;
			swap_tx_desc(tx_buffer->tx_desc, tx_desc_ptr);
#else
			tx_desc_ptr = tx_buffer->tx_desc;
#endif
			dma_unmap_single(NULL,
					 tx_desc_ptr->sdp,
					 tx_desc_ptr->sdl,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(tx_buffer->skb);
			tx_buffer->skb=0;
			//tx_buffer->tx_desc->cown == 1;
			tx_ring->num_free_desc++;
		} else {
			//printk("tx_buffer: %p ## don't free skb\n", tx_buffer);
			break;
		}
		//printk("tssd_index: %d\n", tssd_index);
		// --tx_desc_pair_ptr
		--cur_index;
		tx_buffer = get_tx_buffer_by_index(tx_ring, cur_index);
	}
	return 0;
}

void do_arl_lookup(void)
{
}

void assign_netdev(RXBuffer volatile *rx_buffer)
{
	RXDesc * rx_desc=0;
#ifdef CONFIG_SWITCH_BIG_ENDIAN
	RXDesc tmp_rx_desc;
	rx_desc = &tmp_rx_desc;
	swap_rx_desc(rx_buffer->rx_desc, rx_desc);
#else
	rx_desc = rx_buffer->rx_desc;
#endif

//#if defined(CONFIG_CNS3XXX_PORT_BASE) || defined(CNS3XXX_VLAN_8021Q)
#if defined(CNS3XXX_VLAN_8021Q)
	// sp:
	// 0 - mac port 0
	// 1 - mac port 1
	// 4 - mac port 2

	switch (rx_desc->sp)
	{
		case 0:
		{
			rx_buffer->skb->dev = PORT0_NETDEV;
			break;
		}
		case 1:
		{
			rx_buffer->skb->dev = PORT1_NETDEV;
			break;
		}
		case 4:
		{
			rx_buffer->skb->dev = PORT2_NETDEV;
			break;
		}

	}
#else
	if(is_config_cns3xxx_port_base()) {
		switch (rx_desc->sp)
		{
			case 0:
			{
				rx_buffer->skb->dev = PORT0_NETDEV;
				break;
			}
			case 1:
			{
				rx_buffer->skb->dev = PORT1_NETDEV;
				break;
			}
			case 4:
			{
				rx_buffer->skb->dev = PORT2_NETDEV;
				break;
			}

		}
	}
#endif

//#ifdef CONFIG_CNS3XXX_VLAN_BASE
	if(is_config_cns3xxx_vlan_base())
	{
		u16 vlan_tag;

		vlan_tag = rx_desc->c_vid;
		rx_buffer->skb->dev = net_dev_array[vlan_tag];
	}
//#endif

}

#if 0
#if defined(CNS3XXX_VLAN_8021Q)
static int cns3xxx_vlan_rx(CNS3XXXPrivate *priv, struct sk_buff *skb, u16 vlan_tag)
{                       
        return vlan_hwaccel_receive_skb(skb, priv->vlgrp, vlan_tag);
}
#endif
#endif

// old_priv has ring index information, current version only uses the information.
static int cns3xxx_get_rfd_buff(RXDesc *rx_desc, RXBuffer volatile *rx_buffer, CNS3XXXPrivate *old_priv)
{
	CNS3XXXPrivate *priv=0;
	//RXDesc volatile *rxdesc_ptr = rx_buffer->rx_desc;
	struct sk_buff *skb;
	//unsigned char *data;
	u32 len;
//	RXDesc *rx_desc;
//	u8 *data_ptr;

//#ifdef CONFIG_SWITCH_BIG_ENDIAN
//
//	RXDesc tmp_rx_desc;
//
//        rx_desc = &tmp_rx_desc;
//        swap_rx_desc(rx_buffer->rx_desc, rx_desc);
//
//#else
//        rx_desc = rx_buffer->rx_desc;
//#endif

	//rxdesc_ptr = rxring.vir_addr + index;
	skb = rx_buffer->skb;
	len = rx_desc->sdl;

#ifdef DEBUG_RX
	if (MSG_LEVEL == DUMP_RX_PKT_INFO) {
		printk("rx\n");
		print_packet(skb->data, len);	
	}
#endif

	dma_unmap_single(NULL,
		 rx_desc->sdp,
		 rx_desc->sdl,
		 DMA_FROM_DEVICE);

//	pci_dma_sync_single_for_device(NULL, virt_to_phys(skb->data), len, PCI_DMA_FROMDEVICE);
#if defined (CONFIG_CNS3XXX_SPPE)
	if (PACKET_REASON_TO_CPU == rx_desc->hr) {
		if (sppe_pci_fp_ready) {
			SPPE_PARAM param;
			int pci_dev_index;
			struct iphdr *iph;
			struct ethhdr *ethh;
	
			skb_put(skb, len);
			ethh = (struct ethhdr *)(skb->data);

			if (ntohs(ETH_P_PPP_SES) == ethh->h_proto) {
				/* Remove PPPoE Header */
				memmove(skb->data+8, skb->data, 12); 
				skb->data+=8; 
				skb->len-=8;
				ethh = (struct ethhdr *)(skb->data);
				ethh->h_proto = htons(ETH_P_IP);
			}
			iph = (struct iphdr *)(skb->data + sizeof(struct ethhdr));

			memset(&param, 0, sizeof(SPPE_PARAM));
			param.cmd = SPPE_CMD_ARP;
			param.op = SPPE_OP_GET;
			param.data.sppe_arp.ip[0] = iph->daddr;
			if (SPPE_RESULT_SUCCESS != sppe_func_hook(&param)) {
				printk("<%s>read arp fail\n", __FUNCTION__);
				goto NOT_IN_PCI_FP;
			} else {
				pci_dev_index = param.data.sppe_arp.unused_1;
			}
			param.cmd = SPPE_CMD_PCI_FP_DEV;
			param.op = SPPE_OP_GET;
			param.data.sppe_pci_fp_dev.dev = NULL;
			param.data.sppe_pci_fp_dev.index = pci_dev_index;
			if (SPPE_RESULT_SUCCESS != sppe_pci_fp_hook(&param)) {
				printk("<%s>get dev fail\n", __FUNCTION__);
				goto NOT_IN_PCI_FP;
			} else {
				skb->dev = param.data.sppe_pci_fp_dev.dev;
			}
			dev_queue_xmit(skb);

			return 0;
		}
	}
NOT_IN_PCI_FP:
	/*printk("hr: %x ## sp: %d\n", rx_desc->hr, rx_desc->sp);*/
#endif

#if defined (CONFIG_CPU_FREQ)
	if (unlikely(atomic_read(&cpufreq_pse_flag)))
		goto freepacket;	
#endif

//#ifdef CNS3XXX_NON_NIC_MODE_8021Q
	if (is_cns3xxx_non_nic_mode_8021q()) {
		if (cns3xxx_is_untag_packet(rx_desc) == 1)
			__take_off_vlan_header(skb);
	}
//#endif
	
//#ifdef CONFIG_CNS3XXX_PORT_BASE
	if(is_config_cns3xxx_port_base()) {
		assign_netdev(rx_buffer);

		if (rx_buffer->skb->dev) // if skb->dev is 0, means VLAN base
			goto determine_dev_ok;
	}
//#endif /* CONFIG_CNS3XXX_PORT_BASE */


//#ifdef CONFIG_CNS3XXX_VLAN_BASE
if(is_config_cns3xxx_vlan_base()) {

//#ifdef CONFIG_HAVE_VLAN_TAG
if(is_config_have_vlan_tag()) {

#if defined(CNS3XXX_VLAN_8021Q)
	//printk("rx CNS3XXX_VLAN_8021Q\n");
	// some funcion need netdev like eth_type_trans(), so need to assign it.
	skb->dev = intr_netdev;
	// 8021Q module will determine right netdev by vlan tag.
#else  // defined(CNS3XXX_VLAN_8021Q)
	{ 
		assign_netdev(rx_buffer);

		__take_off_vlan_header(skb);
		if (MSG_LEVEL == 5)
			print_packet(skb->data, 32);

		if ( rx_buffer->skb->dev == 0){
			goto freepacket;
		}
	}
	
#endif // CNS3XXX_VLAN_8021Q

//#else  /* CONFIG_HAVE_VLAN_TAG */
} else {

#ifdef CNS3XXX_RX_DESC_VLAN_INFO
// get VLAN information by RX descriptor field

#endif

//#endif // CONFIG_HAVE_VLAN_TAG
}

}
//#endif // CONFIG_CNS3XXX_VLAN_BASE


//#ifdef CONFIG_CNS3XXX_PORT_BASE
determine_dev_ok:
//#endif

	skb_put(skb, len);

	if (skb->dev) {
		priv = netdev_priv(skb->dev);
	}
	else{
		DEBUG_MSG(WARNING_MSG, "skb_ptr->dev==NULL\n");
		goto freepacket;
	}

#ifdef CNS3XXX_RX_HW_CHECKSUM
	switch (rx_desc->prot)
	{
		case 1 :
		case 2 :
		case 5 :
		case 6 :
		{
			if ( rx_desc->l4f == 0) { // tcp/udp checksum is correct
				skb->ip_summed = CHECKSUM_UNNECESSARY;
				//printk("CHECKSUM_UNNECESSARY\n");
			} else {
				skb->ip_summed = CHECKSUM_NONE; 
				//printk("CHECKSUM_NONE\n");
			}
			break;
		}
		default:
		{
			skb->ip_summed = CHECKSUM_NONE; 
			//printk("xx CHECKSUM_NONE\n");
			break;
		}
	}
#else
	skb->ip_summed = CHECKSUM_NONE; 
#endif // CNS3XXX_RX_HW_CHECKSUM


	// this line must, if no, packet will not send to network layer
#ifdef CONFIG_FAST_BRIDGE
	if (fast_bridge_en == 0) 
#endif
	skb->protocol = eth_type_trans(skb, skb->dev);
	
	skb->dev->last_rx = jiffies;
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += len;

#ifdef CONFIG_FAST_BRIDGE
	if (fast_bridge_en == 1) {

        skb->ip_summed = CHECKSUM_NONE;
	if ( skb->dev == PORT0_NETDEV) {
		skb->dev = PORT1_NETDEV;
	} else if ( skb->dev == PORT1_NETDEV) {
		skb->dev = PORT0_NETDEV;
	}
       	//skb->dev->hard_start_xmit(skb, skb->dev);
	cns3xxx_send_packet(skb, skb->dev);
	} else {
#endif // #ifdef CONFIG_FAST_BRIDGE


//#if defined(CNS3XXX_VLAN_8021Q)
#if 0
	if (priv->vlgrp != NULL)
	{
		//cns3xxx_vlan_rx(priv, skb, rx_desc->c_vid);
		cns3xxx_vlan_rx(priv, skb, rx_desc->c_vid);
		//cns3xxx_vlan_rx(priv, skb, swab16(le32_to_cpu(rx_desc->c_vid)) );
	}
	else
#else
	#ifdef CONFIG_CNS3XXX_NAPI
	netif_receive_skb(skb);
	#else
	netif_rx(skb);
	#endif
#endif

#ifdef CONFIG_FAST_BRIDGE
	}
#endif

	//vlan_hwaccel_receive_skb(skb, priv->vlgrp, 1);

	return 0;

freepacket:
	//DEBUG_MSG(NORMAL_MSG, "freepacket\n");
	dev_kfree_skb_any(skb);
	return 0;
}

// index from 1
u32 get_rx_hw_index(CNS3XXXPrivate *priv)
{
	printk("get_rx_head_phy_addr(&RX_RING0(priv): %x\n", get_rx_head_phy_addr(&RX_RING0(priv)) );
	printk("FS_DESC_PTR0_REG: %x\n", FS_DESC_PTR0_REG);

	return ((FS_DESC_PTR0_REG - get_rx_head_phy_addr(&RX_RING0(priv))) / sizeof(RXDesc) );
}

int get_rx_hw_index_by_reg(u8 ring_num)
{
	if (ring_num == 0 ) {
		return ((FS_DESC_PTR0_REG - FS_DESC_BASE_ADDR0_REG) / sizeof(RXDesc) );
	} else if (ring_num == 1 ) {
		return ((FS_DESC_PTR1_REG - FS_DESC_BASE_ADDR1_REG) / sizeof(RXDesc) );
	}

	return CAVM_FAIL;
}

void dump_rxring(void)
{
	int j=0;
	RXBuffer *rx_buffer = 0;

	rx_buffer = get_rx_ring_head(g_ring_info.rx_ring+0);
	for (j=0 ; j < get_rx_ring_size(g_ring_info.rx_ring+0); ++j, ++rx_buffer) {
		printk("[%d] ## rx_buffer->rx_desc->cown: %d\n", j, rx_buffer->rx_desc->cown);
	}
}

#ifdef CONFIG_CNS3XXX_NAPI
void cns3xxx_receive_packet(CNS3XXXPrivate *priv, int mode, int *work_done, int work_to_do)
#else
void cns3xxx_receive_packet(CNS3XXXPrivate *priv, int mode)
#endif
{
	register int fssd_index;
	//int fssd_current;
	register RXBuffer volatile *rx_buffer = 0;
	register RXDesc volatile *rx_desc=0;
	register struct sk_buff *skb;
#ifndef CONFIG_CNS3XXX_NAPI
	register int fsqf = 0; // Queue Full Mode =0
#endif
	register int i, rxcount = 0;
	register u8 queue_index = priv->ring_index;
	register dma_addr_t dmap;

	register RXDesc volatile *rx_desc_ori_ptr = 0;
	volatile RXDesc rx_desc_tmp;

#ifdef CONFIG_SWITCH_BIG_ENDIAN
	RXDesc tmp_rx_desc;
#endif

	rx_buffer = get_cur_rx_buffer(&(priv->rx_ring[queue_index]));

#ifdef CONFIG_SWITCH_BIG_ENDIAN
	rx_desc = &tmp_rx_desc;
	swap_rx_desc(rx_buffer->rx_desc, rx_desc);
#else
	rx_desc = rx_buffer->rx_desc;
#endif

	fssd_index = get_rx_hw_index_by_reg(queue_index);

	if (fssd_index > get_rx_cur_index(&priv->rx_ring[queue_index]) ) {
		rxcount = fssd_index - get_rx_cur_index(&priv->rx_ring[queue_index]);
	} else if (fssd_index < get_rx_cur_index(&priv->rx_ring[queue_index])) {
		rxcount = (get_rx_ring_size(&priv->rx_ring[queue_index]) - get_rx_cur_index(&priv->rx_ring[queue_index]) ) + fssd_index;
	} else { // fssd_index == rxring.cur_index
		if (rx_desc->cown == 0) { // if rx_desc->cown is 1, we can receive the RX descriptor.
#if defined (CONFIG_CPU_FREQ)
#if 0
			if (unlikely(atomic_read(&cpufreq_pse_flag)))
				goto receive_packet_exit;
#endif
#endif
			enable_rx_dma(0, 1);

			goto receive_packet_exit;
		} else {
			// Queue Full
#ifndef CONFIG_CNS3XXX_NAPI
			fsqf = 1;
#endif
			rxcount = get_rx_ring_size(&priv->rx_ring[queue_index]);
		}
	}

#ifndef CONFIG_CNS3XXX_NAPI
	if (mode == 1) {
		fsqf = 1;
		rxcount = get_rx_ring_size(&priv->rx_ring[queue_index]);
	}
#endif
	
#ifdef CNS3XXX_FREE_TX_IN_RX_PATH
	free_tx_desc_skb(priv->tx_ring + 0, 0);
#ifdef CNS3XXX_DOUBLE_TX_RING
	free_tx_desc_skb(priv->tx_ring + 1, 1);
#endif
#endif

	for (i = 0; i < rxcount; i++) {

		if (rx_desc->cown != 0) { // start to get packet
			// Alloc New skb_buff 
			skb = cns3xxx_alloc_skb();
			// Check skb_buff
			if (skb) {
				rx_desc_ori_ptr = rx_desc;
				rx_desc = &rx_desc_tmp;
				rx_desc_tmp = *rx_desc_ori_ptr;

				cns3xxx_get_rfd_buff((RXDesc *)rx_desc, rx_buffer, priv);

				rx_buffer->skb = skb;
#ifndef NCNB_TEST
//				rx_desc->sdp = (u32)virt_to_phys(skb->data);
#endif
				dmap = dma_map_single(NULL, skb->data, MAX_PACKET_LEN, DMA_FROM_DEVICE);
				
				rx_desc->sdp = dmap;
				rx_desc->sdl = MAX_PACKET_LEN;
				rx_desc->fsd = 1;
				rx_desc->lsd = 1;

				*rx_desc_ori_ptr = rx_desc_tmp;
				rx_desc = rx_desc_ori_ptr;

				rx_desc->cown = 0; // set cbit to 0 

#ifdef CONFIG_SWITCH_BIG_ENDIAN
				swap_rx_desc(rx_desc, rx_buffer->rx_desc);
#endif

#ifdef CONFIG_CNS3XXX_NAPI
				++(*work_done);
				if (*work_done >= work_to_do) {
					rx_index_next(&priv->rx_ring[queue_index]); // rx_ring.cur_index points to next
					rx_buffer = get_cur_rx_buffer(&priv->rx_ring[queue_index]);
					rx_desc = rx_buffer->rx_desc;
					break;
				}
#endif
			} else {
				// I will add dev->lp.stats->rx_dropped, it will effect the performance
				//PDEBUG("%s: Alloc sk_buff fail, reuse the buffer\n", __FUNCTION__);
				printk("%s: Alloc sk_buff fail, reuse the buffer\n", __FUNCTION__);
				rx_desc->cown = 0; // set cbit to 0 

#ifdef CONFIG_SWITCH_BIG_ENDIAN
				swap_rx_desc(rx_desc, rx_buffer->rx_desc);
#endif
				return;
			}
		} else { // cown is 0, no packets
			//*work_done = 0;
#ifdef CONFIG_SWITCH_BIG_ENDIAN
			swap_rx_desc(rx_desc, rx_buffer->rx_desc);
#endif
			return;
		}

		rx_index_next(&priv->rx_ring[queue_index]); // rx_ring.cur_index points to next
		rx_buffer = get_cur_rx_buffer(&priv->rx_ring[queue_index]);
		rx_desc = rx_buffer->rx_desc;
	} // end for (i = 0; i < rxcount; i++) 

#ifndef CONFIG_CNS3XXX_NAPI
	if (fsqf) {
		priv->rx_ring[queue_index].cur_index = fssd_index;
		mb();
		enable_rx_dma(0, 1);
	}
#endif

	//spin_unlock(&rx_lock);
receive_packet_exit:
	return;
}

irqreturn_t cns3xxx_fsrc_ring0_isr(int irq, void *dev_id)
{
	struct net_device *netdev = dev_id;
	CNS3XXXPrivate *priv = netdev_priv(netdev);

	priv->ring_index=0;

#ifdef CONFIG_CNS3XXX_NAPI
{
	CNS3XXXPrivate *priv = netdev_priv(napi_dev);
	priv->ring_index=0;

#ifdef CNS3XXX_USE_MASK
	cns3xxx_write_pri_mask(0xb0);
#else
	cns3xxx_disable_irq(FSRC_RING0_INTERRUPT_ID);
#endif

        //if (likely(netif_rx_schedule_prep(napi_dev, &priv->napi))) {
        if (likely(napi_schedule_prep(&priv->napi))) {
                //__netif_rx_schedule(napi_dev, &priv->napi);
                __napi_schedule(&priv->napi);
	} else {
#ifdef CNS3XXX_USE_MASK
		cns3xxx_write_pri_mask(0xf0);
#else
                cns3xxx_enable_irq(FSRC_RING0_INTERRUPT_ID);
#endif
        }
}
#else // !CONFIG_CNS3XXX_NAPI

#ifdef CNS3XXX_USE_MASK
	cns3xxx_write_pri_mask(0xb0);
#else
	cns3xxx_disable_irq(FSRC_RING0_INTERRUPT_ID);
	cns3xxx_disable_irq(FSQF_RING0_INTERRUPT_ID);
#endif

	cns3xxx_receive_packet(priv, 0); // Receive Once

#ifdef CNS3XXX_USE_MASK
	cns3xxx_write_pri_mask(0xf0);
#else
	cns3xxx_enable_irq(FSRC_RING0_INTERRUPT_ID);
	cns3xxx_enable_irq(FSQF_RING0_INTERRUPT_ID);
#endif
	enable_rx_dma(0, 1);
#endif

	return IRQ_HANDLED;
}


#if defined(CNS3XXX_DOUBLE_RX_RING)
irqreturn_t cns3xxx_fsrc_ring1_isr(int irq, void *dev_id)
{
	struct net_device *netdev = dev_id;
	CNS3XXXPrivate *priv = netdev_priv(netdev);
	priv->ring_index=1;


#if defined(CONFIG_CNS3XXX_NAPI) && defined(CNS3XXX_DOUBLE_RX_RING)
{
	CNS3XXXPrivate *priv = netdev_priv(r1_napi_dev);
	priv->ring_index=1;

	cns3xxx_disable_irq(FSRC_RING1_INTERRUPT_ID);

        if (likely(napi_schedule_prep(&priv->napi))) {
                __napi_schedule(&priv->napi);
	} else {
                cns3xxx_enable_irq(FSRC_RING1_INTERRUPT_ID);
        }
}
#else

	cns3xxx_disable_irq(CNS3XXX_FSRC_RING1_INTERRUPT_ID);
	cns3xxx_disable_irq(CNS3XXX_FSQF_RING1_INTERRUPT_ID);
	cns3xxx_receive_packet(priv, 0); // Receive Once
	enable_rx_dma(1, 1);

	cns3xxx_enable_irq(CNS3XXX_FSRC_RING1_INTERRUPT_ID);
	cns3xxx_enable_irq(CNS3XXX_FSQF_RING1_INTERRUPT_ID);
#endif

	return IRQ_HANDLED;
}
#endif

static int cns3xxx_check_enough_tx_descriptor(TXRing *tx_ring, int need_free_tx_desc)
{
#if 0
	if(tx_ring->num_free_desc < need_free_tx_desc)
		return 0;
#else
	int i=0;
	TXDesc *tx_desc=0;
	u32 cur_index = get_tx_cur_index(tx_ring);
	TXBuffer *tx_buffer = get_tx_buffer_by_index(tx_ring, cur_index);

#ifdef CONFIG_SWITCH_BIG_ENDIAN
	TXDesc tmp_tx_desc;
	tx_desc = &tmp_tx_desc;
	swap_tx_desc(tx_buffer->tx_desc, tx_desc);
#else
	tx_desc = tx_buffer->tx_desc;
#endif
     
	for (i=0 ; i < need_free_tx_desc ; ++i) {
		if ( tx_desc->cown == 0 ) {
			return 0; // no free TX descriptor
		}
		tx_buffer = get_tx_buffer_by_index(tx_ring, ++cur_index);
#ifdef CONFIG_SWITCH_BIG_ENDIAN
		tx_desc = &tmp_tx_desc;
		swap_tx_desc(tx_buffer->tx_desc, tx_desc);
#else
		tx_desc = tx_buffer->tx_desc;
#endif
	}
#endif
	return 1;
}

// if return CAVM_ERR, means pad is fail, the packet cannot send by switch.

static int fill_a_skb_to_tx_desc(TXBuffer * tx_buffer, u8 *data, int len, struct sk_buff *skb, const struct CNS3XXXPrivate_ *priv, int sg, int fsd, int lsd)
{
	//TXDesc *tx_desc_ptr = tx_buffer->tx_desc;
	//	static int tt=0;
	register dma_addr_t dmap;
	register TXRing *tx_ring;

	register TXDesc *tx_desc_ptr = 0;
	register TXDesc *tx_desc_ori_ptr = 0;
	TXDesc tx_desc_tmp;
  
#ifdef CONFIG_SWTICH_BIG_ENDIAN
	TXDesc tmp_tx_desc;
	tx_desc_ptr = &tmp_tx_desc;
	swap_tx_desc(tx_buffer->tx_desc, tx_desc_ptr);
#else
	tx_desc_ptr = tx_buffer->tx_desc;
#endif

	tx_desc_ori_ptr = tx_desc_ptr;
	tx_desc_ptr = &tx_desc_tmp;
	tx_desc_tmp = *tx_desc_ori_ptr;
	

	if (tx_buffer->skb) {	 
		dma_unmap_single(NULL,
				tx_desc_ptr->sdp,
				tx_desc_ptr->sdl,
				DMA_TO_DEVICE);
		dev_kfree_skb_any(tx_buffer->skb); 
		tx_buffer->skb = 0 ;
		//printk("xx tx_ring cur index: %d\n", tx_ring.cur_index);
	} else { 
		//++tx_ring.non_free_tx_skb; 
		//printk("yy tx_ring cur index: %d\n", tx_ring.cur_index);
	} 

	tx_buffer->skb = skb;  /* for free skb */ 
	//tx_desc_ptr->sdp = virt_to_phys(data); 
	//tx_buffer->j = tt;
	tx_buffer->tx_index = cns3xxx_get_tx_hw_index(0);
	//++tt;

#if 0
		{
			static u16 previous_sn_num=10;
			u16 sn_num=0;
			u16 e_type=0;

			printk("skb: %p\n", skb) ;
			print_packet(skb->data, skb->len);

			memcpy(&e_type, skb->data + 12, 2);
			//printk("e_type : %04x\n", e_type);
			e_type = be16_to_cpu(e_type);
			//printk("xxx e_type : %04x\n", e_type);

			if (e_type == 0x0800) {
			printk("previous_sn_num : %d\n", previous_sn_num);
			memcpy(&sn_num, skb->data + 0x28, 2);
			//printk("sn_num : %d\n", sn_num);
			sn_num = be16_to_cpu(sn_num);
			printk("xxx sn_num : %d\n", sn_num);

			if ( previous_sn_num == sn_num)
				printk("dup\n");

			previous_sn_num = sn_num;
			}

		}
#endif

 
#ifdef CNS3XXX_TX_HW_CHECKSUM 
	tx_desc_ptr->ico = 1; 
	tx_desc_ptr->uco = 1; 
	tx_desc_ptr->tco = 1; 
#else 
	tx_desc_ptr->ico = 0; 
	tx_desc_ptr->uco = 0; 
	tx_desc_ptr->tco = 0; 
#endif 
		// Wake interrupt
#ifdef CNS3XXX_TSTC_RING0_ISR
	tx_desc_ptr->interrupt = 1;
#else
	tx_desc_ptr->interrupt = 0;
#endif
 
	/* fill 0 to MIN_PACKET_LEN size */ 
	// can change MIN_PACKET_LEN to 14
	if (sg==0 && len < MIN_PACKET_LEN) { 
		if (skb_padto(skb, MIN_PACKET_LEN)) {
			printk("padding skb error.\n");
			return CAVM_ERR;
		}

		//memset(skb->data + len, 0, MIN_PACKET_LEN - len); 
		//skb->len = MIN_PACKET_LEN;
		tx_desc_ptr->sdl = MIN_PACKET_LEN; 
	} else { 
		tx_desc_ptr->sdl = len; 
	} 

	//dma_cache_maint(data, tx_desc_ptr->sdl, PCI_DMA_TODEVICE); 
	dmap = dma_map_single(NULL, data, len, DMA_TO_DEVICE);
	//should check return value.
	//if (unlikely(dma_mapping_error(NULL, dmap))) {
	//	/* Failed to DMA map this SKB, give it back for now */
	//	return NETDEV_TX_BUSY;
	//}
	tx_desc_ptr->sdp = dmap;

	/* VLAN base or port base function to set TX descriptor */ 
	/* reference: tx_//port_base(), tx_vlan_base() */ 
	priv->net_device_priv->tx_func(tx_desc_ptr, priv, skb); 
	tx_desc_ptr->fsd = fsd;
	tx_desc_ptr->lsd = lsd;

	*tx_desc_ori_ptr = tx_desc_tmp;
	tx_desc_ptr = tx_desc_ori_ptr;

#ifndef CNS3XXX_SG_DISABLE
	/* NOT SG packet */
	if (!sg) 
#endif
		tx_desc_ptr->cown = 0;

#ifdef CONFIG_SWITCH_BIG_ENDIAN
	swap_tx_desc(tx_desc_ptr, tx_buffer->tx_desc);
#endif

	tx_ring = priv->tx_ring + ring_index;

	if (tx_ring->num_free_desc)
		tx_ring->num_free_desc--;

	return CAVM_OK;
} 

int cns3xxx_send_packet(struct sk_buff *skb, struct net_device *netdev)
{
	register CNS3XXXPrivate *priv = netdev_priv(netdev);
	register TXBuffer *tx_buffer = 0;
	register unsigned long flags;
#ifndef CNS3XXX_SG_DISABLE
	register int nr_frags =skb_shinfo(skb)->nr_frags;
	TXDesc *tx_desc[10]; // FIXME: ensure to maximum sg size
	register int     tx_desc_count=0;
	register int i=0;
#endif

	spin_lock_irqsave(&tx_lock, flags);

#if defined (CONFIG_CPU_FREQ)
	if (unlikely(atomic_read(&cpufreq_pse_flag))) {
		spin_unlock_irqrestore(&tx_lock, flags);
		return NETDEV_TX_OK;
		return NETDEV_TX_BUSY;
	}
#endif

#ifdef DEBUG_TX
	if (MSG_LEVEL == DUMP_TX_PKT_INFO) {
		printk("tx\n");
		print_packet(tx_buffer->skb->data, tx_buffer->tx_desc->sdl);
		//dump_tx_desc(tx_buffer->tx_desc);
	}
#endif

#ifndef CNS3XXX_SG_DISABLE
	if (cns3xxx_check_enough_tx_descriptor(priv->tx_ring + ring_index, (nr_frags==0 ) ? 1 : nr_frags) == 0) { 
		// no enough tx descriptor
        	spin_unlock_irqrestore(&tx_lock, flags);
		// re-queue the skb
		return NETDEV_TX_BUSY;
	} 
	tx_buffer = get_cur_tx_buffer(priv->tx_ring + ring_index);
#else
	tx_buffer = get_cur_tx_buffer(priv->tx_ring + ring_index);

	if (0 == tx_buffer->tx_desc->cown)
		return NETDEV_TX_BUSY;
#endif

#ifndef CNS3XXX_SG_DISABLE
	if (nr_frags == 0) { // non scatter/gather I/O
#endif
		fill_a_skb_to_tx_desc(tx_buffer, skb->data, skb->len, skb, priv, 0, 1, 1);
		tx_index_next(priv->tx_ring + ring_index);
#ifndef CNS3XXX_SG_DISABLE
	} else { // scatter/gather I/O
		struct skb_frag_struct *frag = 0;

		fill_a_skb_to_tx_desc(tx_buffer, skb->data, skb->len - skb->data_len, 0, priv, 1, 1, 0);
		tx_desc[tx_desc_count++] = tx_buffer->tx_desc;
		tx_index_next(priv->tx_ring + ring_index);
		tx_buffer = get_cur_tx_buffer(priv->tx_ring + ring_index);

		for (i=0 ; i < nr_frags-1 ; ++i) {
			frag = &skb_shinfo(skb)->frags[i];

			fill_a_skb_to_tx_desc(tx_buffer, page_address(frag->page.p) + frag->page_offset, frag->size, 0, priv, 1, 0, 0);
			tx_desc[tx_desc_count++] = tx_buffer->tx_desc;

			tx_index_next(priv->tx_ring + ring_index);
			tx_buffer = get_cur_tx_buffer(priv->tx_ring + ring_index);
		}
		frag = &skb_shinfo(skb)->frags[nr_frags-1];

		// last fragment
		fill_a_skb_to_tx_desc(tx_buffer, page_address(frag->page.p) + frag->page_offset, frag->size, skb, priv, 1, 0, 1);
		tx_desc[tx_desc_count++] = tx_buffer->tx_desc;

		tx_index_next(priv->tx_ring + ring_index);
		tx_buffer = get_cur_tx_buffer(priv->tx_ring + ring_index);
		
		for (i = 0; i < tx_desc_count ; i++ )
			tx_desc[i]->cown = 0 ; 
	}
#endif
	mb();

	enable_tx_dma(ring_index, 1);

	priv->stats.tx_packets++;
	priv->stats.tx_bytes += skb->len;
	netdev->trans_start = jiffies;

	spin_unlock_irqrestore(&tx_lock, flags);

	return NETDEV_TX_OK;
}


#ifdef CNS3XXX_FSQF_RING0_ISR
irqreturn_t cns3xxx_fsqf_ring0_isr(int irq, void *dev_id)
{
#ifndef CONFIG_CNS3XXX_NAPI
	struct net_device *netdev = dev_id;
	CNS3XXXPrivate *priv = netdev_priv(netdev);
#endif

#ifdef CONFIG_CNS3XXX_NAPI
{
	CNS3XXXPrivate *priv = netdev_priv(napi_dev);
	// because in normal state, fsql only invoke once and set_bit is atomic function.
	// so I don't mask it.
	set_bit(0, &priv->is_qf);
}
#else
#ifdef CNS3XXX_USE_MASK
	cns3xxx_write_pri_mask(0xb0);
#else
	cns3xxx_disable_irq(FSRC_RING0_INTERRUPT_ID);
	cns3xxx_disable_irq(FSQF_RING0_INTERRUPT_ID);
#endif


	cns3xxx_receive_packet(priv, 1); // Receive at Queue Full Mode

#ifdef CNS3XXX_USE_MASK
	cns3xxx_write_pri_mask(0xf0);
#else
	cns3xxx_enable_irq(FSRC_RING0_INTERRUPT_ID);
	cns3xxx_enable_irq(FSQF_RING0_INTERRUPT_ID);
#endif

	enable_rx_dma(0, 1);
#endif // CONFIG_CNS3XXX_NAPI

	return IRQ_HANDLED;
}
#endif


#if defined(CNS3XXX_DOUBLE_RX_RING)
#ifdef CNS3XXX_FSQF_RING1_ISR
irqreturn_t cns3xxx_fsqf_ring1_isr(int irq, void *dev_id)
{
	struct net_device *netdev = dev_id;
	CNS3XXXPrivate *priv = netdev_priv(netdev);
	//INTC_CLEAR_EDGE_TRIGGER_INTERRUPT(INTC_GSW_FSQF_BIT_INDEX);

#ifdef CONFIG_CNS3XXX_NAPI
{
	CNS3XXXPrivate *priv = netdev_priv(r1_napi_dev);
	// because in normal state, fsqf only invoke once and set_bit is atomic function.
	// so don't mask it.
	set_bit(0, &priv->is_qf);
}
#else
	cns3xxx_disable_irq(FSRC_RING1_INTERRUPT_ID);
	cns3xxx_disable_irq(FSQF_RING1_INTERRUPT_ID);

	cns3xxx_receive_packet(priv, 1); // Receive at Queue Full Mode
	enable_rx_dma(1, 1);

	cns3xxx_enable_irq(FSRC_RING1_INTERRUPT_ID);
	cns3xxx_enable_irq(FSQF_RING1_INTERRUPT_ID);
#endif
	return IRQ_HANDLED;
}
#endif
#endif //#if defined(CNS3XXX_DOUBLE_RX_RING)


#ifdef CNS3XXX_STATUS_ISR
irqreturn_t cns3xxx_status_isr(int irq, void *dev_id)
{
	u32 int_status = INTR_STAT_REG;
	u32 i=0;

	cns3xxx_disable_irq(STATUS_INTERRUPT_ID);
	//printk("INTR_STAT_REG : %#x\n", INTR_STAT_REG);
#ifdef CNS3XXX_SHOW_LINK_STATUS
	for (i=14 ; i <= 16 ; ++i) {
		u32 cfg=0;
		u8 mac_cfg[]={0xc, 0x10, 0x18};

		if (((int_status >> i) & 1)) {
			//printk("mac %d: link %s\n", i-14, (SWITCH_REG_VALUE(mac_cfg[i-14]) & 1)? "up" :"down");
			if ((SWITCH_REG_VALUE(mac_cfg[i-14]) & 1)){ // link up

      				// GMII2 high speed drive strength
       				IOCDA_REG &= (~(3 << 10));
				if (((SWITCH_REG_VALUE(mac_cfg[i-14]) >> 2) &0x3) == 2) { // 1000 Mbps
				        IOCDA_REG |= (2 << 10);
				}

			}

			//printk("%x", SWITCH_REG_VALUE(mac_cfg[i-14]));

		}
	}
#else
	for (i = 0; i < 32; i++) {
		if (int_status & (1 << i)) {
			PRINT_INFO(cns3xxx_gsw_status_tbl[i]);
		}
	}
#endif
	INTR_STAT_REG = 0xffffffff; // write 1 for clear.
	cns3xxx_enable_irq(STATUS_INTERRUPT_ID);
	return IRQ_HANDLED;
}
#endif


#ifdef CNS3XXX_TSTC_RING0_ISR
irqreturn_t cns3xxx_tstc_ring0_isr(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}
#endif


static int cns3xxx_install_isr(struct net_device *dev)
{
	int retval;
	CNS3XXXPrivate *priv = netdev_priv(dev);
	
	if (install_isr_rc == 0) {

		retval = request_irq(FSRC_RING0_INTERRUPT_ID, cns3xxx_fsrc_ring0_isr, IRQF_SHARED, "FSRC_RING0", intr_netdev);

		if (retval) {
			printk("%s: unable to get IRQ %d (irqval=%d).\n", "FSRC_RING0",FSRC_RING0_INTERRUPT_ID, retval);
			return 1;
		}

#ifdef CNS3XXX_FSQF_RING0_ISR
		retval = request_irq(FSQF_RING0_INTERRUPT_ID, cns3xxx_fsqf_ring0_isr, IRQF_SHARED, "FSQF_RING0", intr_netdev);

		if (retval) {
			PRINT_INFO("%s: unable to get IRQ %d (irqval=%d).\n", "FSQF_RING0", FSQF_RING0_INTERRUPT_ID, retval);
			return 2;
		}
#endif	

#ifdef CNS3XXX_TSTC_RING0_ISR
		retval = request_irq(TSTC_RING0_INTERRUPT_ID, cns3xxx_tstc_ring0_isr, IRQF_SHARED, "TSTC_RING0", intr_netdev);

		if (retval) {
			PRINT_INFO("%s: unable to get IRQ %d (irqval=%d).\n", "TSTC_RING0", FSQF_RING0_INTERRUPT_ID, retval);
			return 3;
		}

#endif


	if (priv->num_rx_queues == 2) {
#if defined(CNS3XXX_DOUBLE_RX_RING)
		retval = request_irq(FSRC_RING1_INTERRUPT_ID, cns3xxx_fsrc_ring1_isr, IRQF_SHARED, "FSRC_RING1", intr_netdev);
		printk("xx intr_netdev->name: %s\n", intr_netdev->name);

		if (retval) {
			printk("%s: unable to get IRQ %d (irqval=%d).\n", "FSRC_RING1",FSRC_RING1_INTERRUPT_ID, retval);
			return 1;
		}

#ifdef CNS3XXX_FSQF_RING1_ISR
		retval = request_irq(FSQF_RING1_INTERRUPT_ID, cns3xxx_fsqf_ring1_isr, IRQF_SHARED, "FSQF_RING1", intr_netdev);

		if (retval) {
			PRINT_INFO("%s: unable to get IRQ %d (irqval=%d).\n", "FSQF_RING1", FSQF_RING1_INTERRUPT_ID, retval);
			return 2;
		}
#endif	

#endif
	}

#ifdef CNS3XXX_STATUS_ISR
#ifdef CNS3XXX_SHOW_LINK_STATUS

	INTR_MASK_REG=0xffffffff;
	// only enable mac link status
	INTR_MASK_REG &= (~(1 << 14));
	INTR_MASK_REG &= (~(1 << 15));
	INTR_MASK_REG &= (~(1 << 16));

#endif
		retval = request_irq(STATUS_INTERRUPT_ID, cns3xxx_status_isr, IRQF_SHARED, "GSW_STATUS", intr_netdev);

		if (retval) {
			PRINT_INFO("%s: unable to get IRQ %d (irqval=%d).\n", "GSW STATUS INT", STATUS_INTERRUPT_ID, retval);
			return 3;
		}
		INTR_MASK_REG = 0;
#endif






#ifdef CONFIG_CNS3XXX_NAPI
{
	CNS3XXXPrivate *sp = netdev_priv(napi_dev);    
        napi_enable(&sp->napi);
        netif_start_queue(napi_dev);

#ifdef CNS3XXX_DOUBLE_RX_RING
	sp = netdev_priv(r1_napi_dev);    
        napi_enable(&sp->napi);
        netif_start_queue(r1_napi_dev);
#endif
}
#endif
	// enable cpu port
	enable_port(3, 1);

	} // end if (install_isr_rc == 0) 

	++install_isr_rc;

	return 0;
}


int cns3xxx_open(struct net_device *dev)
{
	CNS3XXXPrivate *priv = netdev_priv(dev);
	//static int init_state=0;
	
	if (cns3xxx_setup_rx_tx_res(priv) != CAVM_OK) {
		printk("alloc rx/tx ring fail!!\n");	
		return -1;
	}

	netif_start_queue(dev);

	cns3xxx_install_isr(dev);

	enable_rx_dma(0, 1);

	if (priv->num_rx_queues == 2) 
		enable_rx_dma(1, 1);

	netif_carrier_on(dev);
	priv->net_device_priv->open();

	return 0;
}

static int cns3xxx_uninstall_isr(struct net_device *dev)
{
	CNS3XXXPrivate *priv = netdev_priv(dev);
	--install_isr_rc;
	if (install_isr_rc == 0) {
		enable_port(3, 0);
		free_irq(FSRC_RING0_INTERRUPT_ID, intr_netdev);
#ifdef CNS3XXX_STATUS_ISR
		free_irq(STATUS_INTERRUPT_ID, intr_netdev);
#endif

#ifdef CNS3XXX_FSQF_RING0_ISR
		free_irq(FSQF_RING0_INTERRUPT_ID, intr_netdev);
#endif

#ifdef CNS3XXX_TSTC_RING0_ISR
		free_irq(TSTC_RING0_INTERRUPT_ID, intr_netdev);
#endif

	if (priv->num_rx_queues == 2) {
		free_irq(FSRC_RING1_INTERRUPT_ID, intr_netdev);

#ifdef CNS3XXX_FSQF_RING1_ISR
		free_irq(FSQF_RING1_INTERRUPT_ID, intr_netdev);
#endif
	}



#ifdef CONFIG_CNS3XXX_NAPI
{
	CNS3XXXPrivate *sp = netdev_priv(napi_dev);

	napi_disable(&sp->napi);
	netif_stop_queue(napi_dev);
#ifdef CNS3XXX_DOUBLE_RX_RING
	sp = netdev_priv(r1_napi_dev);

	napi_disable(&sp->napi);
	netif_stop_queue(r1_napi_dev);
#endif
}
#endif


	}

	return 0;
}

int cns3xxx_close(struct net_device *dev)
{
	CNS3XXXPrivate *priv = netdev_priv(dev);

	enable_rx_dma(0, 0);
	enable_tx_dma(0, 0);

	if (priv->num_rx_queues == 2) 
		enable_tx_dma(1, 0);

	if (priv->num_tx_queues == 2) 
		enable_rx_dma(1, 0);

        netif_stop_queue(dev);

	priv->net_device_priv->close();
	cns3xxx_uninstall_isr(dev);
	cns3xxx_free_rx_tx_res(priv);
	netif_carrier_off(dev);
	return 0;
}



//#define MAC_PORT(p) MAC##p##_CFG_REG

void broadcast_storm_cfg(u8 port, u8 boradcast, u8 multicast, u8 unknown)
{
	switch (port)
	{
		case 0:
		{
			(boradcast == 1) ? (MAC0_CFG_REG |= (1 << 30)) : (MAC0_CFG_REG &= (~(1 << 30))) ;
			(multicast == 1) ? (MAC0_CFG_REG |= (1 << 29)) : (MAC0_CFG_REG &= (~(1 << 29))) ;
			(unknown == 1) ? (MAC0_CFG_REG |= (1 << 28)) : (MAC0_CFG_REG &= (~(1 << 28))) ;
			break;
		}
		case 1:
		{
			(boradcast == 1) ? (MAC1_CFG_REG |= (1 << 30)) : (MAC1_CFG_REG &= (~(1 << 30))) ;
			(multicast == 1) ? (MAC1_CFG_REG |= (1 << 29)) : (MAC1_CFG_REG &= (~(1 << 29))) ;
			(unknown == 1) ? (MAC1_CFG_REG |= (1 << 28)) : (MAC1_CFG_REG &= (~(1 << 28))) ;
			break;
		}
		case 2:
		{
			(boradcast == 1) ? (MAC2_CFG_REG |= (1 << 30)) : (MAC2_CFG_REG &= (~(1 << 30))) ;
			(multicast == 1) ? (MAC2_CFG_REG |= (1 << 29)) : (MAC2_CFG_REG &= (~(1 << 29))) ;
			(unknown == 1) ? (MAC2_CFG_REG |= (1 << 28)) : (MAC2_CFG_REG &= (~(1 << 28))) ;
			break;
		}
	}
}

void broadcast_storm_rate(u8 rate)
{
	TC_CTRL_REG &= (~(0xf << 24));
	TC_CTRL_REG |= (rate << 24);
}

static int cns3xxx_set_mac_addr(struct net_device *dev, void *p)
{
	//struct sockaddr *sock_addr = addr;
	CNS3XXXPrivate *priv = netdev_priv(dev);

        struct sockaddr *addr= p;


	spin_lock_irq(&priv->lock);


        if (!is_valid_ether_addr(addr->sa_data))
                return -EADDRNOTAVAIL;

	// 1. delete old arl mac entry
	// 2. add new arl mac entry
	// 3. copy new mac to netdev field

	if (priv->net_device_priv->arl_table_entry) {
		cns3xxx_arl_table_invalid(priv->net_device_priv->arl_table_entry);
		memcpy(priv->net_device_priv->arl_table_entry->mac, addr->sa_data, dev->addr_len);
		//print_arl_table_entry(priv->net_device_priv->arl_table_entry);
		cns3xxx_arl_table_add(priv->net_device_priv->arl_table_entry);
	}
        memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);

	spin_unlock_irq(&priv->lock);
	return 0;
}


int set_fc_rls(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	FC_GLOB_THRS_REG &= (~(0x1ff << 16));
	FC_GLOB_THRS_REG |= (ctl.val << 16);
	return CAVM_OK;
}

int get_fc_rls(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 

	ctl.val = ((FC_GLOB_THRS_REG >> 16) & 0x1ff);

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int set_fc_set(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	FC_GLOB_THRS_REG &= (~0x1ff);
	FC_GLOB_THRS_REG |= ctl.val;
	return CAVM_OK;
}

int get_fc_set(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 

	ctl.val = ((FC_GLOB_THRS_REG) & 0x1ff);

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}


int set_sarl_rls(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	SARL_CTRL_REG &= (~(0x1ff << 12));
	SARL_CTRL_REG |= (ctl.val << 12);
	return CAVM_OK;
}

int get_sarl_rls(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 

	ctl.val = ((SARL_CTRL_REG >> 12) & 0x1ff);

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int set_sarl_enable(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	SARL_CTRL_REG &= (~(0x1 << 31));
	SARL_CTRL_REG |= (ctl.val << 31);
	return CAVM_OK;
}

int get_sarl_enable(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	ctl.val = ((SARL_CTRL_REG >> 31 ) & 0x1);
	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}
int set_sarl_set(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	SARL_CTRL_REG &= (~0x1ff);
	SARL_CTRL_REG |= ctl.val;
	return CAVM_OK;
}

int get_sarl_set(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 

	ctl.val = ((SARL_CTRL_REG) & 0x1ff);

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int set_sarl_oq(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;
	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 

	switch (ctl.gyr)
	{
		case 0: // green
		{
			SARL_OQ_GTH_REG &= (~(0xff << ctl.tc*8));
			SARL_OQ_GTH_REG |= (ctl.val << ctl.tc*8);
			break;
		}
		case 1: // yellow
		{
			SARL_OQ_YTH_REG &= (~(0xff << ctl.tc*8));
			SARL_OQ_YTH_REG |= (ctl.val << ctl.tc*8);
			break;
		}
		case 2: // red
		{
			SARL_OQ_RTH_REG &= (~(0xff << ctl.tc*8));
			SARL_OQ_RTH_REG |= (ctl.val << ctl.tc*8);
			break;
		}
	}
	return CAVM_OK;
}

int get_sarl_oq(struct ifreq *ifr)
{
	CNS3XXXSARLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 

	switch (ctl.gyr)
	{
		case 0: // green
		{
			ctl.val = ((SARL_OQ_GTH_REG >> ctl.tc*8) & 0xff);
			break;
		}
		case 1: // yellow
		{
			ctl.val = ((SARL_OQ_YTH_REG >> ctl.tc*8) & 0xff);
			break;
		}
		case 2: // red
		{
			ctl.val = ((SARL_OQ_RTH_REG >> ctl.tc*8) & 0xff);
			break;
		}
	}

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXSARLEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int set_queue_weight(struct ifreq *ifr)
{
	CNS3XXXQueueWeightEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXQueueWeightEntry)) )  
		return -EFAULT; 
	switch (ctl.which_port)
	{
		case 0:
		{
			QUEUE_WEIGHT_SET(0, ctl)
			return 0;
		}
		case 1:
		{
			QUEUE_WEIGHT_SET(1, ctl)
			return 0;
		}
		case 2:
		{
			QUEUE_WEIGHT_SET(2, ctl)
			return 0;
		}
		case 3: // cpu port 
		{
			CPU_PRI_CTRL_REG &= ~(0x3ffff);
			CPU_PRI_CTRL_REG |= (ctl.sch_mode << 16);
			CPU_PRI_CTRL_REG |= (ctl.q0_w);
			CPU_PRI_CTRL_REG |= (ctl.q1_w << 4);
			CPU_PRI_CTRL_REG |= (ctl.q2_w << 8);
			CPU_PRI_CTRL_REG |= (ctl.q3_w << 12);
			return 0;
		}
		case 4: // PPE port 
		{
			HNAT_PRI_CTRL_REG &= ~(0x3ffff);
			HNAT_PRI_CTRL_REG |= (ctl.sch_mode << 16);
			HNAT_PRI_CTRL_REG |= (ctl.q0_w);
			HNAT_PRI_CTRL_REG |= (ctl.q1_w << 4);
			HNAT_PRI_CTRL_REG |= (ctl.q2_w << 8);
			HNAT_PRI_CTRL_REG |= (ctl.q3_w << 12);
			return 0;
		}
		default:
		{
			return -EFAULT;
		}
	}
}

int get_queue_weight(struct ifreq *ifr)
{
	CNS3XXXQueueWeightEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXQueueWeightEntry)) )  
		return -EFAULT; 

	switch (ctl.which_port)
	{
		case 0:
		{
			QUEUE_WEIGHT_GET(0, ctl)
			break;
		}
		case 1:
		{
			QUEUE_WEIGHT_GET(1, ctl)
			break;
		}
		case 2:
		{
			QUEUE_WEIGHT_GET(2, ctl)
			break;
		}
		case 3:
		{
			ctl.sch_mode = ((CPU_PRI_CTRL_REG >> 16 ) & 0x3); 
			ctl.q0_w = ((CPU_PRI_CTRL_REG >> 0 ) & 0x7); 
			ctl.q1_w = ((CPU_PRI_CTRL_REG >> 4 ) & 0x7);
			ctl.q2_w = ((CPU_PRI_CTRL_REG >> 8 ) & 0x7); 
			ctl.q3_w = ((CPU_PRI_CTRL_REG >> 12 ) & 0x7); 
			break;
		}
		case 4:
		{
			ctl.sch_mode = ((HNAT_PRI_CTRL_REG >> 16 ) & 0x3); 
			ctl.q0_w = ((HNAT_PRI_CTRL_REG >> 0 ) & 0x7); 
			ctl.q1_w = ((HNAT_PRI_CTRL_REG >> 4 ) & 0x7);
			ctl.q2_w = ((HNAT_PRI_CTRL_REG >> 8 ) & 0x7); 
			ctl.q3_w = ((HNAT_PRI_CTRL_REG >> 12 ) & 0x7); 
			break;
		}
	}

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXQueueWeightEntry)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int set_rate_limit(struct ifreq *ifr)
{
	CNS3XXXRateLimitEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXRateLimitEntry)) )  
		return -EFAULT; 
	switch (ctl.which_port)
	{
		case 0:
		{
			RATE_CTRL_REG &= (~(0x7f << 8));
			RATE_CTRL_REG |= ( ctl.band_width << 8);
			RATE_CTRL_REG &= (~(0x3));
			RATE_CTRL_REG |= ctl.base_rate;
			return 0;
		}
		case 1:
		{
			RATE_CTRL_REG &= (~(0x7f << 16));
			RATE_CTRL_REG |= ( ctl.band_width << 16);
			RATE_CTRL_REG &= (~(0x3 << 2));
			RATE_CTRL_REG |= (ctl.base_rate << 2);
			return 0;
		}
		case 2:
		{
			RATE_CTRL_REG &= (~(0x7f << 24));
			RATE_CTRL_REG |= ( ctl.band_width << 24);
			RATE_CTRL_REG &= (~(0x3 << 4));
			RATE_CTRL_REG |= (ctl.base_rate << 4);
			return 0;
		}
		case 3: // port 0 extra dma
		{
			TC_CTRL_REG &= (~0x7f);
			TC_CTRL_REG |= ctl.band_width;
			RATE_CTRL_REG &= (~(0x3 << 6));
			RATE_CTRL_REG |= (ctl.base_rate << 6);
			return 0;
		}
		default:
		{
			return -EFAULT;
		}
	}
}

int get_rate_limit(struct ifreq *ifr)
{
	CNS3XXXRateLimitEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXRateLimitEntry)) )  
		return -EFAULT; 
	switch (ctl.which_port)
	{
		case 0:
		{
			ctl.band_width = (RATE_CTRL_REG >> 8) & 0x7f;
			ctl.base_rate = RATE_CTRL_REG & 0x3;
			break;
		}
		case 1:
		{
			ctl.band_width = (RATE_CTRL_REG >> 16) & 0x7f;
			ctl.base_rate = (RATE_CTRL_REG >> 2) & 0x3;
			break;
		}
		case 2:
		{
			ctl.band_width = (RATE_CTRL_REG >> 24) & 0x7f;
			ctl.base_rate = (RATE_CTRL_REG >> 4) & 0x3;
			break;
		}
		case 3: // port 0 extra dma
		{
			ctl.band_width = (TC_CTRL_REG) & 0x7f;
			ctl.base_rate = (RATE_CTRL_REG >> 6) & 0x3;
			break;
		}
		default:
		{
			return -EFAULT;
		}
	}

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXRateLimitEntry)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int set_fc(struct ifreq *ifr)
{
	CNS3XXXFCEntry ctl;
	u32 port_offset[]={0x0c, 0x10, 0x18, 0x14}; // 0x14 is cpu port offset
	u32 val=0;


	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXFCEntry)) )  
		return -EFAULT; 

	val = SWITCH_REG_VALUE(port_offset[ctl.port]);
	if (ctl.port == 3) { // cpu port, only can set rx fc
		val &= (~(1 << 31));
		if (ctl.fc_en) 
			val |= (1 << 31);
	} else {
		val &= (~(1 << 11)); // disable rx fc
		val &= (~(1 << 12)); // disable tx fc
		val |= (ctl.fc_en << 11);
	}

	SWITCH_REG_VALUE(port_offset[ctl.port]) = val;
	return CAVM_OK;
}

int get_fc(struct ifreq *ifr)
{
	CNS3XXXFCEntry ctl;
	u32 port_offset[]={0x0c, 0x10, 0x18, 0x14}; // 0x14 is cpu port offset
	u32 val=0;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXFCEntry)) )  
		return -EFAULT; 

	val = SWITCH_REG_VALUE(port_offset[ctl.port]);
	printk("port_offset[%d]: %x\n", ctl.port, port_offset[ctl.port]);
	printk("val: %x\n", val);
	if (ctl.port == 3) { // cpu port, only can set rx fc
		ctl.fc_en = ((val >> 31) & 1);
	} else {
		ctl.fc_en = ((val >> 11) & 3);

	}

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXFCEntry)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int set_ivl(struct ifreq *ifr)
{
	CNS3XXXIVLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXIVLEntry)) )  
		return -EFAULT; 

	cns3xxx_ivl(ctl.enable);

	return CAVM_OK;
}

int get_ivl(struct ifreq *ifr)
{
	CNS3XXXIVLEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXIVLEntry)) )  
		return -EFAULT; 

	ctl.enable = ((MAC_GLOB_CFG_REG >> 7) & 0x1);

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXIVLEntry)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int set_wan_port(struct ifreq *ifr)
{
	CNS3XXXWANPortEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXWANPortEntry)) )  
		return -EFAULT; 
	VLAN_CFG &= (~(0x1f << 8));
	VLAN_CFG |= (ctl.wan_port << 8);

	return CAVM_OK;
}
int get_wan_port(struct ifreq *ifr)
{
	CNS3XXXWANPortEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXWANPortEntry)) )  
		return -EFAULT; 

	ctl.wan_port = ((VLAN_CFG >> 8) & 0x1f);

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXWANPortEntry)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int set_pvid(struct ifreq *ifr)
{
	CNS3XXXPVIDEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXPVIDEntry)) )  
		return -EFAULT; 
	cns3xxx_set_pvid(ctl.which_port, ctl.pvid);

	return CAVM_OK;
}

int get_pvid(struct ifreq *ifr)
{
	CNS3XXXPVIDEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXPVIDEntry)) )  
		return -EFAULT; 

	ctl.pvid = cns3xxx_get_pvid(ctl.which_port);
	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXPVIDEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int set_qa(struct ifreq *ifr)
{
	CNS3XXXQAEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXQAEntry)) )  
		return -EFAULT; 

	MAC_GLOB_CFG_EXT_REG &= ~(0x7 << 27);
	MAC_GLOB_CFG_EXT_REG |= (ctl.qa << 27);
	//printk("[kernel mode] ctl.qa: %d\n", ctl.qa);
	
	return CAVM_OK;
}

int get_qa(struct ifreq *ifr)
{
	CNS3XXXQAEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXQAEntry)) )  
		return -EFAULT; 

	ctl.qa = (MAC_GLOB_CFG_EXT_REG >> 27) & 0x7;

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXQAEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int get_packet_max_len(struct ifreq *ifr)
{
	CNS3XXXMaxLenEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXMaxLenEntry)) )  
		return -EFAULT; 

	ctl.max_len = (PHY_AUTO_ADDR_REG >> 30) & 0x3;

	if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXMaxLenEntry)) )  
		return -EFAULT; 
	return CAVM_OK;
}

int set_packet_max_len(struct ifreq *ifr)
{
	CNS3XXXMaxLenEntry ctl;

	if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXMaxLenEntry)) )  
		return -EFAULT; 

	PHY_AUTO_ADDR_REG &= (~(3 << 30));
	PHY_AUTO_ADDR_REG |= (ctl.max_len << 30);

	return CAVM_OK;
}

int set_udp_range(struct ifreq *ifr)
{
	CNS3XXXUdpRangeEtypeControl conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXUdpRangeEtypeControl)) )  
		return -EFAULT; 

	switch (conf.udp_range_num)
	{
		case 0:
		{
			UDP_RANGE0_REG = 0;
			UDP_RANGE0_REG |= conf.port_start;
			UDP_RANGE0_REG |= (conf.port_end << 16);

			PRIO_ETYPE_UDP_REG &= (~(7 << 16));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 16);
			break;
		}
		case 1:
		{
			UDP_RANGE1_REG = 0;
			UDP_RANGE1_REG |= conf.port_start;
			UDP_RANGE1_REG |= (conf.port_end << 16);

			PRIO_ETYPE_UDP_REG &= (~(7 << 20));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 20);
			break;
		}
		case 2:
		{
			UDP_RANGE2_REG = 0;
			UDP_RANGE2_REG |= conf.port_start;
			UDP_RANGE2_REG |= (conf.port_end << 16);

			PRIO_ETYPE_UDP_REG &= (~(7 << 24));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 24);
			break;
		}
		case 3:
		{
			UDP_RANGE3_REG = 0;
			UDP_RANGE3_REG |= conf.port_start;
			UDP_RANGE3_REG |= (conf.port_end << 16);

			PRIO_ETYPE_UDP_REG &= (~(7 << 28));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 28);
			break;
		}
	}

	return CAVM_OK;
}

int get_udp_range(struct ifreq *ifr)
{
	CNS3XXXUdpRangeEtypeControl conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXUdpRangeEtypeControl)) )  
		return -EFAULT; 

	switch (conf.udp_range_num)
	{
		case 0:
		{
			conf.port_start = (UDP_RANGE0_REG & 0xffff);
			conf.port_end = ((UDP_RANGE0_REG >> 16 )& 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 16) & 0x7);
			break;
		}
		case 1:
		{
			conf.port_start = (UDP_RANGE1_REG & 0xffff);
			conf.port_end = ((UDP_RANGE1_REG >> 16 )& 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 20) & 0x7);
			break;
		}
		case 2:
		{
			conf.port_start = (UDP_RANGE2_REG & 0xffff);
			conf.port_end = ((UDP_RANGE2_REG >> 16 )& 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 24) & 0x7);
			break;
		}
		case 3:
		{
			conf.port_start = (UDP_RANGE3_REG & 0xffff);
			conf.port_end = ((UDP_RANGE3_REG >> 16 )& 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 28) & 0x7);
			break;
		}
	}

	if (copy_to_user(ifr->ifr_data, &conf, sizeof(CNS3XXXUdpRangeEtypeControl)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int get_etype(struct ifreq *ifr)
{
	CNS3XXXEtypeControl conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXEtypeControl)) )  
		return -EFAULT; 
	switch (conf.etype_num)
	{
		case 0:
		{
			conf.val = (ETYPE1_ETYPE0_REG & 0xffff);
			conf.pri = (PRIO_ETYPE_UDP_REG & 0x7);
			break;
		}
		case 1:
		{
			conf.val = ((ETYPE1_ETYPE0_REG >> 16 )& 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 4) & 0x7);
			break;
		}
		case 2:
		{
			conf.val = (ETYPE3_ETYPE2_REG & 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 8) & 0x7);
			break;
		}
		case 3:
		{
			conf.val = ((ETYPE3_ETYPE2_REG >> 16 )& 0xffff);
			conf.pri = ((PRIO_ETYPE_UDP_REG >> 12) & 0x7);
			break;
		}
	}
	if (copy_to_user(ifr->ifr_data, &conf, sizeof(CNS3XXXEtypeControl)) )  
		return -EFAULT; 

	return CAVM_OK;
}

int set_etype(struct ifreq *ifr)
{
	CNS3XXXEtypeControl conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXEtypeControl)) )  
		return -EFAULT; 
	switch (conf.etype_num)
	{
		case 0:
		{
			ETYPE1_ETYPE0_REG &= (~0xffff);
			ETYPE1_ETYPE0_REG |= conf.val;

			PRIO_ETYPE_UDP_REG &= (~7);
			PRIO_ETYPE_UDP_REG |= (conf.pri);
			break;
		}
		case 1:
		{
			ETYPE1_ETYPE0_REG &= (~(0xffff << 16));
			ETYPE1_ETYPE0_REG |= (conf.val << 16);

			PRIO_ETYPE_UDP_REG &= (~(7 << 4));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 4);
			break;
		}
		case 2:
		{
			ETYPE3_ETYPE2_REG &= (~0xffff);
			ETYPE3_ETYPE2_REG |= conf.val;

			PRIO_ETYPE_UDP_REG &= (~(7 << 8));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 8);
			break;
		}
		case 3:
		{
			ETYPE3_ETYPE2_REG &= (~(0xffff << 16));
			ETYPE3_ETYPE2_REG |= (conf.val << 16);

			PRIO_ETYPE_UDP_REG &= (~(7 << 12));
			PRIO_ETYPE_UDP_REG |= (conf.pri << 12);
			break;
		}
	}
	return CAVM_OK;
}

int get_pri_ip_dscp(struct ifreq *ifr)
{
	CNS3XXXPriIpDscpControl conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXPriIpDscpControl)) )  
		return -EFAULT; 

	if ( 0 <= conf.ip_dscp_num && conf.ip_dscp_num <= 7) {
		conf.pri = ((PRIO_IPDSCP_7_0_REG >> (conf.ip_dscp_num * 4)) & 0x7);
	} else if ( 8 <= conf.ip_dscp_num && conf.ip_dscp_num <= 15) {
			conf.pri = ((PRIO_IPDSCP_15_8_REG >> ((conf.ip_dscp_num-8) * 4)) & 0x7);
		 } else if ( 16 <= conf.ip_dscp_num && conf.ip_dscp_num <= 23) {
				conf.pri = ((PRIO_IPDSCP_23_16_REG >> ((conf.ip_dscp_num-16) * 4)) & 0x7);
		 	} else if ( 24 <= conf.ip_dscp_num && conf.ip_dscp_num <= 31) {
					conf.pri = ((PRIO_IPDSCP_31_24_REG >> ((conf.ip_dscp_num-24) * 4)) & 0x7);
		 		} else if ( 32 <= conf.ip_dscp_num && conf.ip_dscp_num <= 39) {
						conf.pri = ((PRIO_IPDSCP_39_32_REG >> ((conf.ip_dscp_num-32) * 4)) & 0x7);
		 			} else if ( 40 <= conf.ip_dscp_num && conf.ip_dscp_num <= 47) {
							conf.pri = ((PRIO_IPDSCP_47_40_REG >> ((conf.ip_dscp_num-40) * 4)) & 0x7);
		 				} else if ( 48 <= conf.ip_dscp_num && conf.ip_dscp_num <= 55) {
								conf.pri = ((PRIO_IPDSCP_55_48_REG >> ((conf.ip_dscp_num-48) * 4)) & 0x7);
		 					} else if ( 56 <= conf.ip_dscp_num && conf.ip_dscp_num <= 63) {
									conf.pri = ((PRIO_IPDSCP_63_56_REG >> ((conf.ip_dscp_num-56) * 4)) & 0x7);
								} else {
									return CAVM_ERR;
									}


	if (copy_to_user(ifr->ifr_data, &conf, sizeof(CNS3XXXPriIpDscpControl)) )  
		return -EFAULT; 
	return CAVM_OK;
}


int set_pri_ip_dscp(struct ifreq *ifr)
{
	CNS3XXXPriIpDscpControl conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXPriIpDscpControl)) )  
		return -EFAULT; 

	if ( 0 <= conf.ip_dscp_num && conf.ip_dscp_num <= 7) {
		PRIO_IPDSCP_7_0_REG &=  (~(0x7 << (conf.ip_dscp_num * 4) ) );
		PRIO_IPDSCP_7_0_REG |= (conf.pri << (conf.ip_dscp_num * 4));
	} else if ( 8 <= conf.ip_dscp_num && conf.ip_dscp_num <= 15) {
			PRIO_IPDSCP_15_8_REG &=  (~(0x7 << ((conf.ip_dscp_num-8) * 4) ) );
			PRIO_IPDSCP_15_8_REG |= (conf.pri << ((conf.ip_dscp_num-8) * 4));
		 } else if ( 16 <= conf.ip_dscp_num && conf.ip_dscp_num <= 23) {
				PRIO_IPDSCP_23_16_REG &=  (~(0x7 << ((conf.ip_dscp_num-16) * 4) ) );
				PRIO_IPDSCP_23_16_REG |= (conf.pri << ((conf.ip_dscp_num-16) * 4));

		 	} else if ( 24 <= conf.ip_dscp_num && conf.ip_dscp_num <= 31) {
					PRIO_IPDSCP_31_24_REG &=  (~(0x7 << ((conf.ip_dscp_num-24) * 4) ) );
					PRIO_IPDSCP_31_24_REG |= (conf.pri << ((conf.ip_dscp_num-24) * 4));

		 		} else if ( 32 <= conf.ip_dscp_num && conf.ip_dscp_num <= 39) {
						PRIO_IPDSCP_39_32_REG &=  (~(0x7 << ((conf.ip_dscp_num-32) * 4) ) );
						PRIO_IPDSCP_39_32_REG |= (conf.pri << ((conf.ip_dscp_num-32) * 4));

		 			} else if ( 40 <= conf.ip_dscp_num && conf.ip_dscp_num <= 47) {
							PRIO_IPDSCP_47_40_REG &=  (~(0x7 << ((conf.ip_dscp_num-40) * 4) ) );
							PRIO_IPDSCP_47_40_REG |= (conf.pri << ((conf.ip_dscp_num-40) * 4));
		 				} else if ( 48 <= conf.ip_dscp_num && conf.ip_dscp_num <= 55) {
								PRIO_IPDSCP_55_48_REG &=  (~(0x7 << ((conf.ip_dscp_num-48) * 4) ) );
								PRIO_IPDSCP_55_48_REG |= (conf.pri << ((conf.ip_dscp_num-48) * 4));
		 					} else if ( 56 <= conf.ip_dscp_num && conf.ip_dscp_num <= 63) {
									PRIO_IPDSCP_63_56_REG &=  (~(0x7 << ((conf.ip_dscp_num-56) * 4) ) );
									PRIO_IPDSCP_63_56_REG |= (conf.pri << ((conf.ip_dscp_num-56) * 4));
								} else {
									return CAVM_ERR;
									}
	return CAVM_OK;
}


#if (defined(CONFIG_VB) || defined (CONFIG_VB_2))

extern int bcm53115M_reg_read(int page, int offset, u8 *buf, int len);

int bcm53115M_reg_read_ioctl(struct ifreq *ifr)
{
	CNS3XXXBCM53115M conf;
	int __init_or_module gpio_direction_output(unsigned int pin, unsigned int state);


	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXBCM53115M)) )  
		return -EFAULT; 
	printk("conf.page: %x\n", conf.page);
	printk("conf.offset: %x\n", conf.offset);
	printk("conf.data_len: %x\n", conf.data_len);
	switch (conf.data_len)
	{
		case 1:
		{
			bcm53115M_reg_read(conf.page, conf.offset, (u8 *)&conf.u8_val, 1);
			printk("conf.u8_val: %x\n", conf.u8_val);
			break;
		}
		case 2:
		{
			bcm53115M_reg_read(conf.page, conf.offset, (u8 *)&conf.u16_val, 2);
			printk("conf.u16_val: %x\n", conf.u16_val);
			break;
		}
		case 4:
		{
			bcm53115M_reg_read(conf.page, conf.offset, (u8 *)&conf.u32_val, 4);
			printk("conf.u32_val: %x\n", conf.u32_val);
			break;
		}
		default:
		{
			printk("[kernel mode]: don't support date length: %d\n", conf.data_len);
		}
	}



	if (copy_to_user(ifr->ifr_data, &conf, sizeof(CNS3XXXBCM53115M)) )  
		return -EFAULT; 
	return CAVM_OK;
}

extern int bcm53115M_reg_write(int page, int offset, u8 *buf, int len);

int bcm53115M_reg_write_ioctl(struct ifreq *ifr)
{
	CNS3XXXBCM53115M conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXBCM53115M)) )  
		return -EFAULT; 

	switch (conf.data_len)
	{
		case 1:
		{
        		bcm53115M_reg_write(conf.page, conf.offset, (u8 *)&conf.u8_val, 1);
			break;
		}
		case 2:
		{
			bcm53115M_reg_write(conf.page, conf.offset, (u8 *)&conf.u16_val, 2);
			break;
		}
		case 4:
		{
			bcm53115M_reg_write(conf.page, conf.offset, (u8 *)&conf.u32_val, 4);
			break;
		}
		default:
		{
			printk("[kernel mode]: don't support date length: %d\n", conf.data_len);
		}
	}
	return CAVM_OK;
}
#endif

#if 0
int get_rxring(struct ifreq *ifr)
{
	CNS3XXXRingStatus conf;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXRingStatus)) )  
		return -EFAULT; 
	conf.rx_ring=g_ring_info.rx_ring;
	conf.tx_ring=0;
	if (copy_to_user(ifr->ifr_data, &conf, sizeof(CNS3XXXRingStatus)) )  
		return -EFAULT; 
}
#endif

int dump_mib_counter(struct ifreq *ifr)
{
	CNS3XXXMIBCounter conf;
	int addr=0,i=0;

	if (copy_from_user(&conf, ifr->ifr_data, sizeof(CNS3XXXMIBCounter)) )  
		return -EFAULT; 

	for (addr=0x300; addr <= 0x334 ; addr+=4)
		conf.mib[i++]=SWITCH_REG_VALUE(addr);
	for (addr=0x400; addr <= 0x434 ; addr+=4)
		conf.mib[i++]=SWITCH_REG_VALUE(addr);
	for (addr=0x600; addr <= 0x634 ; addr+=4)
		conf.mib[i++]=SWITCH_REG_VALUE(addr);
	// cpu mib counter
	for (addr=0x500; addr <= 0x528 ; addr+=4)
		conf.mib[i++]=SWITCH_REG_VALUE(addr);
	conf.mib_len=i;
	if (copy_to_user(ifr->ifr_data, &conf, sizeof(CNS3XXXMIBCounter)) )  
		return -EFAULT; 
	return 0;
}

// reference e100.c
int cns3xxx_do_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	CNS3XXXIoctlCmd ioctl_cmd;

	//printk("cns3xxx_do_ioctl begin\n");

        if (cmd != SIOCDEVPRIVATE) {
                return -EOPNOTSUPP;
        }
        if (copy_from_user(&ioctl_cmd, ifr->ifr_data, sizeof(CNS3XXXIoctlCmd))) 
                return -EFAULT;

	//printk("ioctl_cmd: %d\n", ioctl_cmd);
	switch (ioctl_cmd) {
		case CNS3XXX_ARP_REQUEST_SET:
		{
			CNS3XXXArpRequestControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXArpRequestControl)) ) 
				return -EFAULT;

			(ctl.val==0) ? (MAC_GLOB_CFG_REG &= (~(1 << 23)) ): (MAC_GLOB_CFG_REG |= (1 << 23) );

		}

		case CNS3XXX_ARP_REQUEST_GET:
		{
			CNS3XXXArpRequestControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXArpRequestControl)) ) 
				return -EFAULT;

			ctl.val = ((MAC_GLOB_CFG_REG >> 23) & 1);

			if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXArpRequestControl)) )  
				return -EFAULT; 
			return CAVM_OK;
		}
	
		case CNS3XXX_HOL_PREVENT_SET:
		{
			CNS3XXXHOLPreventControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXHOLPreventControl)) ) 
				return -EFAULT;
			(ctl.enable == 1) ? (TC_CTRL_REG |= (1 << 29)) : (TC_CTRL_REG &= (~(1 << 29))) ;

			return CAVM_OK;
		}
		case CNS3XXX_HOL_PREVENT_GET:
		{
			CNS3XXXHOLPreventControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXHOLPreventControl)) ) 
				return -EFAULT;

			ctl.enable = ((TC_CTRL_REG >> 29) & 0x1);

			if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXHOLPreventControl)) )  
				return -EFAULT; 
			return CAVM_OK;
		}

		// for S component or C conponent
		case CNS3XXX_BRIDGE_SET:
		{
			CNS3XXXBridgeControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXBridgeControl)) ) 
				return -EFAULT;
			(ctl.type == 1) ? (VLAN_CFG |= (1 << 1)) : (VLAN_CFG &= (~(1 << 1))) ;


		}
		case CNS3XXX_BRIDGE_GET:
		{
			CNS3XXXBridgeControl ctl;

			ctl.type = ((VLAN_CFG >> 1) & 0x1);
			printk("[kernel mode] ctl.type: %d\n", ctl.type);

			if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXBridgeControl)) )  
				return -EFAULT; 

			return CAVM_OK;
		}

		case CNS3XXX_PORT_NEIGHBOR_SET:
		{
			CNS3XXXPortNeighborControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXPortNeighborControl)) ) 
				return -EFAULT;
			switch (ctl.which_port)
			{
				case 0:
				{
					(ctl.type == 1) ? (VLAN_CFG |= (1 << 4)) : (VLAN_CFG &= (~(1 << 4))) ;
					return 0;
				}
				case 1:
				{
					(ctl.type == 1) ? (VLAN_CFG |= (1 << 5)) : (VLAN_CFG &= (~(1 << 5))) ;
					return 0;
				}
				case 2:
				{
					(ctl.type == 1) ? (VLAN_CFG |= (1 << 7)) : (VLAN_CFG &= (~(1 << 7))) ;
					return 0;
				}
				case 3: // cpu port
				{
					(ctl.type == 1) ? (VLAN_CFG |= (1 << 6)) : (VLAN_CFG &= (~(1 << 6))) ;
					return 0;
				}
				default:
					return -EFAULT;
			}

		}

		case CNS3XXX_PORT_NEIGHBOR_GET:
		{
			CNS3XXXPortNeighborControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXPortNeighborControl)) ) 
				return -EFAULT;
			switch (ctl.which_port)
			{
				case 0:
				{
					ctl.type = ((VLAN_CFG >> 4 ) & 0x1);
					break;
				}
				case 1:
				{
					ctl.type = ((VLAN_CFG >> 5 ) & 0x1);
					break;
				}
				case 2:
				{
					ctl.type = ((VLAN_CFG >> 7 ) & 0x1);
					break;
				}
				case 3: // cpu port
				{
					ctl.type = ((VLAN_CFG >> 6 ) & 0x1);
					break;
				}
			}

			if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXPortNeighborControl)) )  
				return -EFAULT; 

			return CAVM_OK;
		}

		case CNS3XXX_VLAN_TABLE_LOOKUP:
		{
			CNS3XXXVLANTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXVLANTableEntry)) ) 
				return -EFAULT;
			if (cns3xxx_vlan_table_lookup(&ctl.entry) ==  CAVM_NOT_FOUND) {
				return CAVM_NOT_FOUND;
			}

		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXVLANTableEntry)))
		                return -EFAULT;

			return CAVM_FOUND;
		}
		case CNS3XXX_VLAN_TABLE_READ:
		{
			CNS3XXXVLANTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXVLANTableEntry)) ) 
			{
				return -EFAULT;
			}
			cns3xxx_vlan_table_read(&ctl.entry);
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXVLANTableEntry)))
		                return -EFAULT;

			return 0;
		}
		case CNS3XXX_VLAN_TABLE_ADD:
		{	
			CNS3XXXVLANTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXVLANTableEntry)) ) 
				return -EFAULT;
			cns3xxx_vlan_table_add(&ctl.entry);
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXVLANTableEntry)))
		                return -EFAULT;

			return 0;
		}

		case CNS3XXX_ARL_TABLE_ADD:
		{	
			CNS3XXXARLTableEntry ctl;

			printk("[kernel mode] CNS3XXX_ARL_TABLE_ADD\n");
		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			cns3xxx_arl_table_add(&ctl.entry);
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return 0;
		}


		case CNS3XXX_ARL_TABLE_DEL:
		{	
			CNS3XXXARLTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			cns3xxx_arl_table_invalid(&ctl.entry);
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return 0;
		}
		case CNS3XXX_VLAN_TABLE_DEL:
		{	
			CNS3XXXARLTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			cns3xxx_arl_table_invalid(&ctl.entry);

		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return CAVM_FOUND;
		}

		case CNS3XXX_ARL_TABLE_SEARCH:
		{	
			CNS3XXXARLTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			if (cns3xxx_arl_table_search(&ctl.entry) == CAVM_NOT_FOUND){
				printk("[kernel mode] not found\n");
				return CAVM_NOT_FOUND;
			}
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return CAVM_FOUND;
		}
		case CNS3XXX_ARL_IS_TABLE_END:
		{	
			CNS3XXXARLTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			if (cns3xxx_is_arl_table_end() == CAVM_ERR)
				return CAVM_ERR;
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return CAVM_OK;
		}

		case CNS3XXX_ARL_TABLE_SEARCH_AGAIN:
		{	
			CNS3XXXARLTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			if (cns3xxx_arl_table_search_again(&ctl.entry) == CAVM_NOT_FOUND)
				return CAVM_NOT_FOUND;
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return CAVM_FOUND;
		}

		case CNS3XXX_ARL_TABLE_FLUSH:
		{	
			CNS3XXXARLTableEntry ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;

			cns3xxx_arl_table_flush();

		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return CAVM_FOUND;
		}



		case CNS3XXX_ARL_TABLE_LOOKUP:
		{	
			CNS3XXXARLTableEntry ctl;

		
			printk("[kernel mode] in CNS3XXX_ARL_TABLE_LOOKUP\n");
		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXARLTableEntry)) ) 
				return -EFAULT;
			if (cns3xxx_arl_table_lookup(&ctl.entry) == CAVM_NOT_FOUND)
				return CAVM_NOT_FOUND;
		        if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXARLTableEntry)))
		                return -EFAULT;

			return CAVM_FOUND;
		}

		case CNS3XXX_TC_SET:
		{	
			CNS3XXXTrafficClassControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXTrafficClassControl)) ) 
				return -EFAULT;
			TC_CTRL_REG &= (~(0x3 << 30));
			TC_CTRL_REG |= (ctl.tc << 30);
			return CAVM_OK;
		}
		case CNS3XXX_TC_GET:
		{
			CNS3XXXTrafficClassControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXTrafficClassControl)) ) 
				return -EFAULT;

			ctl.tc = ((TC_CTRL_REG >> 30) & 0x3);

			if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXTrafficClassControl)) )  
				return -EFAULT; 

			return CAVM_OK;
		}

		case CNS3XXX_PRI_CTRL_SET:
		{
			CNS3XXXPriCtrlControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXPriCtrlControl)) ) 
				return -EFAULT;

			switch (ctl.which_port)
			{
				case 0:
				{
					MAC0_PRI_CTRL_REG &= (~(0x7 << 24));
					MAC0_PRI_CTRL_REG &= (~(0xf << 18));
					
					MAC0_PRI_CTRL_REG |= (ctl.port_pri << 24);

					MAC0_PRI_CTRL_REG |= (ctl.ether_pri_en << 18);
					MAC0_PRI_CTRL_REG |= (ctl.vlan_pri_en << 19);
					MAC0_PRI_CTRL_REG |= (ctl.dscp_pri_en << 20);
					MAC0_PRI_CTRL_REG |= (ctl.udp_pri_en << 21);
					break;
				}
				case 1:
				{
					MAC1_PRI_CTRL_REG &= (~(0x7 << 24));
					MAC1_PRI_CTRL_REG &= (~(0xf << 18));
					
					MAC1_PRI_CTRL_REG |= (ctl.port_pri << 24);

					MAC1_PRI_CTRL_REG |= (ctl.ether_pri_en << 18);
					MAC1_PRI_CTRL_REG |= (ctl.vlan_pri_en << 19);
					MAC1_PRI_CTRL_REG |= (ctl.dscp_pri_en << 20);
					MAC1_PRI_CTRL_REG |= (ctl.udp_pri_en << 21);
					break;
				}
				case 2:
				{
					MAC2_PRI_CTRL_REG &= (~(0x7 << 24));
					MAC2_PRI_CTRL_REG &= (~(0xf << 18));
					
					MAC2_PRI_CTRL_REG |= (ctl.port_pri << 24);

					MAC2_PRI_CTRL_REG |= (ctl.ether_pri_en << 18);
					MAC2_PRI_CTRL_REG |= (ctl.vlan_pri_en << 19);
					MAC2_PRI_CTRL_REG |= (ctl.dscp_pri_en << 20);
					MAC2_PRI_CTRL_REG |= (ctl.udp_pri_en << 21);
					break;
				}
				case 3: // cpu
				{
					printk("[kernel mode] CPU_PRI_CTRL_REG: %#x\n", CPU_PRI_CTRL_REG);
					CPU_PRI_CTRL_REG &= (~(0x7 << 24));
					CPU_PRI_CTRL_REG &= (~(0xf << 18));
					
					CPU_PRI_CTRL_REG |= (ctl.port_pri << 24);

					CPU_PRI_CTRL_REG |= (ctl.ether_pri_en << 18);
					CPU_PRI_CTRL_REG |= (ctl.vlan_pri_en << 19);
					CPU_PRI_CTRL_REG |= (ctl.dscp_pri_en << 20);
					CPU_PRI_CTRL_REG |= (ctl.udp_pri_en << 21);
					break;
				}
			}

			return CAVM_OK;
		}

		case CNS3XXX_PRI_CTRL_GET:
		{
			CNS3XXXPriCtrlControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXPriCtrlControl)) ) 
				return -EFAULT;

			switch (ctl.which_port)
			{
				case 0:
				{
					ctl.port_pri = (unsigned char)((MAC0_PRI_CTRL_REG >> 24) & 0x7);
					ctl.ether_pri_en = (unsigned char)((MAC0_PRI_CTRL_REG >> 18) & 0x1);
					ctl.vlan_pri_en = (unsigned char)((MAC0_PRI_CTRL_REG >> 19) & 0x1);
					ctl.dscp_pri_en = (unsigned char)((MAC0_PRI_CTRL_REG >> 20) & 0x1);
					ctl.udp_pri_en = (unsigned char)((MAC0_PRI_CTRL_REG >> 21) & 0x1);
					break;
				}
				case 1:
				{
					ctl.port_pri = (unsigned char)((MAC1_PRI_CTRL_REG >> 24) & 0x7);
					ctl.ether_pri_en = (unsigned char)((MAC1_PRI_CTRL_REG >> 18) & 0x1);
					ctl.vlan_pri_en = (unsigned char)((MAC1_PRI_CTRL_REG >> 19) & 0x1);
					ctl.dscp_pri_en = (unsigned char)((MAC1_PRI_CTRL_REG >> 20) & 0x1);
					ctl.udp_pri_en = (unsigned char)((MAC1_PRI_CTRL_REG >> 21) & 0x1);
					break;
				}
				case 2:
				{
					ctl.port_pri = (unsigned char)((MAC2_PRI_CTRL_REG >> 24) & 0x7);
					ctl.ether_pri_en = (unsigned char)((MAC2_PRI_CTRL_REG >> 18) & 0x1);
					ctl.vlan_pri_en = (unsigned char)((MAC2_PRI_CTRL_REG >> 19) & 0x1);
					ctl.dscp_pri_en = (unsigned char)((MAC2_PRI_CTRL_REG >> 20) & 0x1);
					ctl.udp_pri_en = (unsigned char)((MAC2_PRI_CTRL_REG >> 21) & 0x1);
					break;
				}
				case 3:
				{
					ctl.port_pri = (unsigned char)((CPU_PRI_CTRL_REG >> 24) & 0x7);
					ctl.ether_pri_en = (unsigned char)((CPU_PRI_CTRL_REG >> 18) & 0x1);
					ctl.vlan_pri_en = (unsigned char)((CPU_PRI_CTRL_REG >> 19) & 0x1);
					ctl.dscp_pri_en = (unsigned char)((CPU_PRI_CTRL_REG >> 20) & 0x1);
					ctl.udp_pri_en = (unsigned char)((CPU_PRI_CTRL_REG >> 21) & 0x1);
					break;
				}
			}

			if (copy_to_user(ifr->ifr_data, &ctl, sizeof(CNS3XXXPriCtrlControl)) )  
				return -EFAULT; 

			return CAVM_OK;
		}

		case CNS3XXX_DMA_RING_CTRL_SET:
		{
			CNS3XXXDmaRingCtrlControl ctl;

		        if (copy_from_user(&ctl, ifr->ifr_data, sizeof(CNS3XXXDmaRingCtrlControl)) ) 
				return -EFAULT;

			(ctl.ts_double_ring_en == 0) ? DMA_RING_CTRL_REG &= (~(0x1 << 16)) : (DMA_RING_CTRL_REG |= (ctl.ts_double_ring_en << 16));
			(ctl.fs_double_ring_en == 0) ? DMA_RING_CTRL_REG &= (~(0x1 << 0)) : (DMA_RING_CTRL_REG |= (ctl.fs_double_ring_en << 0));
			(ctl.fs_pkt_allocate == 0) ? DMA_RING_CTRL_REG &= (~(0x1 << 1)) : (DMA_RING_CTRL_REG |= (ctl.fs_pkt_allocate << 1));
		}

		case CNS3XXX_PRI_IP_DSCP_SET:
		{
			return set_pri_ip_dscp(ifr);
		}
		case CNS3XXX_PRI_IP_DSCP_GET:
		{
			return get_pri_ip_dscp(ifr);
		}

		case CNS3XXX_ETYPE_SET:
		{
			return set_etype(ifr);
		}
		case CNS3XXX_ETYPE_GET:
		{
			return get_etype(ifr);
		}

		case CNS3XXX_UDP_RANGE_SET:
		{
			return set_udp_range(ifr);
		}
		case CNS3XXX_UDP_RANGE_GET:
		{
			return get_udp_range(ifr);
		}

		case CNS3XXX_RATE_LIMIT_SET:
		{
			return set_rate_limit(ifr);
		}
		case CNS3XXX_RATE_LIMIT_GET:
		{
			return get_rate_limit(ifr);
		}
		case CNS3XXX_QUEUE_WEIGHT_SET:
		{
			return set_queue_weight(ifr);
		}
		case CNS3XXX_QUEUE_WEIGHT_GET:
		{
			return get_queue_weight(ifr);
		}

		case CNS3XXX_FC_RLS_SET:
		{
			return set_fc_rls(ifr);
		}
		case CNS3XXX_FC_RLS_GET:
		{
			return get_fc_rls(ifr);
		}

		case CNS3XXX_FC_SET_SET:
		{
			return set_fc_set(ifr);
		}
		case CNS3XXX_FC_SET_GET:
		{
			return get_fc_set(ifr);
		}

		case CNS3XXX_SARL_RLS_SET:
		{
			return set_sarl_rls(ifr);
		}
		case CNS3XXX_SARL_RLS_GET:
		{
			return get_sarl_rls(ifr);
		}

		case CNS3XXX_SARL_SET_SET:
		{
			return set_sarl_set(ifr);
		}
		case CNS3XXX_SARL_SET_GET:
		{
			return get_sarl_set(ifr);
		}

		case CNS3XXX_SARL_OQ_SET:
		{
			return set_sarl_oq(ifr);
		}
		case CNS3XXX_SARL_OQ_GET:
		{
			return get_sarl_oq(ifr);
		}

		case CNS3XXX_SARL_ENABLE_SET:
		{
			return set_sarl_enable(ifr);
		}
		case CNS3XXX_SARL_ENABLE_GET:
		{
			return get_sarl_enable(ifr);
		}

		case CNS3XXX_FC_SET:
		{
			return set_fc(ifr);
		}
		case CNS3XXX_FC_GET:
		{
			return get_fc(ifr);
		}

		case CNS3XXX_IVL_SET:
		{
			return set_ivl(ifr);
		}
		case CNS3XXX_IVL_GET:
		{
			return get_ivl(ifr);
		}

		case CNS3XXX_WAN_PORT_SET:
		{
			return set_wan_port(ifr);
		}
		case CNS3XXX_WAN_PORT_GET:
		{
			return get_wan_port(ifr);
		}

		case CNS3XXX_PVID_SET:
		{
			return set_pvid(ifr);
		}
		case CNS3XXX_PVID_GET:
		{
			return get_pvid(ifr);
		}

		case CNS3XXX_QA_GET:
		{
			return get_qa(ifr);
		}
		case CNS3XXX_QA_SET:
		{
			return set_qa(ifr);
		}

		case CNS3XXX_PACKET_MAX_LEN_GET:
		{
			return get_packet_max_len(ifr);
		}
		case CNS3XXX_PACKET_MAX_LEN_SET:
		{
			return set_packet_max_len(ifr);
		}

#if (defined(CONFIG_VB) || defined (CONFIG_VB_2))
		case CNS3XXX_BCM53115M_REG_READ:
		{
			return bcm53115M_reg_read_ioctl(ifr);
		}
		case CNS3XXX_BCM53115M_REG_WRITE:
		{
			return bcm53115M_reg_write_ioctl(ifr);
		}

#endif

#if 0
		case CNS3XXX_RXRING_STATUS:
		{
			return get_rxring(ifr);
		}
#endif
		case CNS3XXX_DUMP_MIB_COUNTER:
		{
			return dump_mib_counter(ifr);
		}
	

		default:
		{
			printk("[kernel mode] don't match any command\n");
			break;
		}

	} // end switch (ioctl_cmd) 
	return 0;
}

#ifdef CONFIG_CNS3XXX_NAPI
static int cns3xxx_poll(struct napi_struct *napi, int budget)
{

	CNS3XXXPrivate *sp = container_of(napi, CNS3XXXPrivate, napi);
	int work_done = 0;
	int work_to_do = budget; // define minima value

	//printk("sp->ring_index: %d\n", sp->ring_index);
	cns3xxx_receive_packet(sp, 0, &work_done, work_to_do);

	budget -= work_done;
	//printk("xx budget: %d ## work_done: %d\n", budget, work_done);
	//printk("=================\n");

    if (work_done) {
		if (test_bit(0, (unsigned long *)&sp->is_qf) == 1){
			//printk("qf\n");
			clear_bit(0, (unsigned long *)&sp->is_qf);
			//printk("sp->ring_index: %d\n", sp->ring_index);
			enable_rx_dma(sp->ring_index, 1);
			return work_done;
       	}
    } else {
		//netif_rx_complete(napi_dev, &sp->napi);
		napi_complete(napi);
#ifdef CNS3XXX_USE_MASK
		cns3xxx_write_pri_mask(0xf0);
#else
		if (sp->ring_index == 0)
			cns3xxx_enable_irq(FSRC_RING0_INTERRUPT_ID);
		else
			cns3xxx_enable_irq(FSRC_RING1_INTERRUPT_ID);
#endif
			//printk("exit poll\n");
            return 0;
       }

	return work_done;
}
#endif

static struct net_device_stats *cns3xxx_get_stats(struct net_device *dev)
{
	CNS3XXXPrivate *priv = netdev_priv(dev);

	return &priv->stats;
}

static int cns3xxx_change_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu < cns3xxx_min_mtu() || new_mtu > cns3xxx_max_mtu())
		return -EINVAL;

	dev->mtu = new_mtu;

	return 0;
}

static void cns3xxx_timeout(struct net_device *dev)
{
	printk("%s:cns3xxx gsw timeout\n", dev->name);
	//star_gsw_enable(dev);
	netif_wake_queue(dev);
	dev->trans_start = jiffies;
}

#ifdef LINUX2631
static const struct net_device_ops cns3xxx_netdev_ops = {
        .ndo_open               = cns3xxx_open,
        .ndo_stop               = cns3xxx_close,
        .ndo_start_xmit         = cns3xxx_send_packet,
        //.ndo_validate_addr      = eth_validate_addr,
        //.ndo_set_multicast_list = cns3xxx_set_multicast_list,
        .ndo_set_mac_address    = cns3xxx_set_mac_addr,
        .ndo_change_mtu         = cns3xxx_change_mtu,
        .ndo_do_ioctl           = cns3xxx_do_ioctl,
        .ndo_tx_timeout         = cns3xxx_timeout,
	.ndo_get_stats		= cns3xxx_get_stats,

#if defined(CNS3XXX_VLAN_8021Q)

      //  .ndo_vlan_rx_register   = cns3xxx_vlan_rx_register,
        .ndo_vlan_rx_add_vid    = e1000_vlan_rx_add_vid,
        .ndo_vlan_rx_kill_vid   = cns3xxx_vlan_rx_kill_vid,
#endif

#ifdef CONFIG_NET_POLL_CONTROLLER
        .ndo_poll_controller    = cns3xxx_netpoll,
#endif
};
#endif // LINUX2631

static int __init cns3xxx_probe(RingInfo ring_info)
{
#if defined(CONFIG_MACH_TS43XX)
   unsigned long *flashOTP;
   int j;
#endif
	void cns3xxx_set_ethtool_ops(struct net_device *netdev);

//	int netdev_size = sizeof(net_device_prive)/sizeof(NetDevicePriv);
	int netdev_size = num_net_dev_priv;
	int i=0, err=0;
	struct net_device *netdev=0;
	CNS3XXXPrivate *priv=0;
	struct sockaddr sock_addr;
#ifdef CONFIG_CNS3XXX_ETHADDR_IN_FLASH
    u8 mac_int[6];
    int val_len;
    int do_new_mac = 0;
    if (0 == init_mtd_env()) {
        do_new_mac = 1;
    }
#endif

	for (i=0 ; i < netdev_size ; ++i) {

		netdev = alloc_etherdev(sizeof(CNS3XXXPrivate));
		if (!netdev) {
			err = -ENOMEM;
			goto err_alloc_etherdev;
		}
		if (net_device_prive[i].name)
			strcpy(netdev->name, net_device_prive[i].name);


		net_dev_array[net_device_prive[i].vlan_tag] = netdev;
		//printk("\tnet_dev_array[%d]: %x\n", net_device_prive[i].vlan_tag , netdev);
		if (intr_netdev==0)
			intr_netdev = netdev;

		SET_NETDEV_DEV(netdev, NULL);
		priv = netdev_priv(netdev);
		spin_lock_init(&priv->lock);
		memset(priv, 0, sizeof(CNS3XXXPrivate));

#if 1
		priv->num_rx_queues = ring_info.num_rx_queues;
		priv->num_tx_queues = ring_info.num_tx_queues;
		priv->rx_ring = ring_info.rx_ring;
		priv->tx_ring = ring_info.tx_ring;
#endif 

		priv->net_device_priv = &net_device_prive[i];
		//printk("net_device_prive[i].pmap : %x\n", net_device_prive[i].pmap);

		// set netdev MAC address
#if defined (CONFIG_CNS3XXX_ETHADDR_IN_FLASH)
		if (do_new_mac) {
			char name[20];
			sprintf(name, "ethaddr%d=", i);
			if (0 == fmg_get(name,&val_len)) {
				mac_str_to_int(ethaddr, val_len,mac_int, 6);
				memcpy(sock_addr.sa_data, mac_int, 6);
			} else {
				memcpy(sock_addr.sa_data, net_device_prive[i].mac, 6);
			}
		} else {
			memcpy(sock_addr.sa_data, net_device_prive[i].mac, 6);
		}
#else
#if defined(CONFIG_MACH_TS43XX)          
      net_device_prive[i].mac[0] = 0;
      net_device_prive[i].mac[1] = 0xD0;
      net_device_prive[i].mac[2] = 0x69;
      flashOTP = getFlashOTP(); /* defined in arch/arm/mach-cns3xxx/ts43xx.c */
      if ((flashOTP[0] & 0x00FFFFFF) == 0x00FFFFFF) {
         unsigned long x;
         printk("TS-43XX flash OTP blank; Selecting a random MAC (eth%d) ...", i);

         get_random_bytes(&x, sizeof(x));
         net_device_prive[i].mac[3] = (unsigned char)(x >> 16);
         net_device_prive[i].mac[4] = (unsigned char)(x >> 8);
         net_device_prive[i].mac[5] = (unsigned char)x;
         
      } else {
         printk("TS-43XX MAC (eth%d) = ", i);
         net_device_prive[i].mac[3] = (unsigned char)(flashOTP[0] >> 16);
         net_device_prive[i].mac[4] = (unsigned char)(flashOTP[0] >> 8);
         net_device_prive[i].mac[5] = (unsigned char)flashOTP[0];
         if (i > 0) {            
            net_device_prive[i].mac[5] += i;
            if (net_device_prive[i].mac[5]==0) { /* we wrapped from 0xFF to 0*/
               net_device_prive[i].mac[4]++;
               if (net_device_prive[i].mac[4]==0)
                  net_device_prive[i].mac[3]++;
            }
         }
      }
      
      for(j=0; j < 6; j++)
         printk("%02x:", net_device_prive[i].mac[j]);
      printk("\n");
      
      memcpy(sock_addr.sa_data, net_device_prive[i].mac, 6);         
#else
		memcpy(sock_addr.sa_data, net_device_prive[i].mac, 6);
#endif		
#endif

		cns3xxx_set_mac_addr(netdev, &sock_addr);

#ifdef LINUX2631
		netdev->netdev_ops = &cns3xxx_netdev_ops;
#endif

		cns3xxx_set_ethtool_ops(netdev);
#ifdef LINUX2627
		//netdev->base_addr		= IO_ADDRESS(GSW_BASE_ADDR);
		netdev->base_addr = 0;
		netdev->open = cns3xxx_open;
		netdev->stop = cns3xxx_close;
		netdev->hard_start_xmit	= cns3xxx_send_packet;
		//netdev->hard_start_xmit	= 0;
		netdev->do_ioctl = cns3xxx_do_ioctl;
		netdev->change_mtu = cns3xxx_change_mtu;

		//netdev->get_stats = cns3xxx_get_stats;
		netdev->watchdog_timeo = 5 * HZ; // ref e1000_main.c
		netdev->tx_timeout = cns3xxx_timeout;
		netdev->set_mac_address	= cns3xxx_set_mac_addr;
#endif

#if defined(CNS3XXX_TX_HW_CHECKSUM)
		netdev->features |= NETIF_F_IP_CSUM;
	#ifndef CNS3XXX_SG_DISABLE
		netdev->features |= NETIF_F_SG;
	#endif
#endif


#ifdef CONFIG_CNS3XXX_NAPI
		//printk("napi netdev: %s\n", netdev->name);
		//printk("netdev addr: %p\n", netdev);
		//netif_napi_add(netdev, &priv->napi, cns3xxx_poll, CNS3XXX_NAPI_WEIGHT);
#endif

#if defined(CNS3XXX_VLAN_8021Q)
		// do not let 8021Q module insert vlan tag
		// can use the snippet code to get vlan tage
		// if (priv->vlgrp && vlan_tx_tag_present(skb)) 
		//   vlan_tag = cpu_to_be16(vlan_tx_tag_get(skb));
#ifdef CNS3XXX_8021Q_HW_TX
		// hardware support insert VLAN tag on TX path
		netdev->features |= NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX;
#else
		netdev->features |= NETIF_F_HW_VLAN_RX; // remove NETIF_F_HW_VLAN_TX flag that 8021Q module to insert vlan tag.
#endif

	        //netdev->vlan_rx_register = cns3xxx_vlan_rx_register;
		//netdev->vlan_rx_kill_vid = cns3xxx_vlan_rx_kill_vid;
#endif


		err = register_netdev(netdev);
		if (err) {
			printk("Register network dev :%s failed \n", netdev->name);
			goto err_register_netdev;
		}

		netif_carrier_off(netdev);
		netdev = 0;
	} // for (i=0 ; i < netdev_size ; ++i) 

	return 0;


err_register_netdev:
	free_netdev(netdev);

err_alloc_etherdev:
	return err;
}

#if 0
int cns3xxx_gsw_config_mac_port0(void)
{
        INIT_PORT0_PHY
	INIT_PORT0_MAC
        PORT0_LINK_DOWN
        return 0;
}

int cns3xxx_gsw_config_mac_port1(void)
{
        INIT_PORT1_PHY
	INIT_PORT1_MAC
        PORT1_LINK_DOWN
        return 0;
}

int cns3xxx_gsw_config_mac_port2(void)
{
        INIT_PORT2_PHY
	INIT_PORT2_MAC
        PORT2_LINK_DOWN
        return 0;
}
#endif

static int cns3xxx_pse_regs_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int num = 0;
	int i=0;

	for (i=0x00 ; i <= 0x84 ; i+=4)
		num += sprintf(page + num, "offset %#x: %#x\n", i, SWITCH_REG_VALUE(i) );
	for (i=0xd0 ; i <= 0xf8 ; i+=4)
		num += sprintf(page + num, "offset %#x: %#x\n", i, SWITCH_REG_VALUE(i) );
	for (i=0x100 ; i <= 0x164 ; i+=4)
		num += sprintf(page + num, "offset %#x: %#x\n", i, SWITCH_REG_VALUE(i) );
	num += sprintf(page + num, "IOCDA_REG:  %#x\n", IOCDA_REG);

	return num;
}

#if 0
static int cns3xxx_pse_rx_desc_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
#ifdef DEBUG_RX_PROC
	int num = 0;
//if (show_rx_proc)
{ // for debug
	int i=0, j=0;
	RXBuffer *rx_buffer = 0;

	if (g_ring_info.rx_ring) {
		for (i=0 ; i < g_ring_info.num_rx_queues ; ++i) {
			rx_buffer = get_rx_ring_head(g_ring_info.rx_ring+i);

			if (rx_buffer==0)
				break;

#if 0
			num += sprintf(page + num, "rx ring ## %d\n", i);
			num += sprintf(page + num, "get_rx_ring_size(rx_ring): %d\n", get_rx_ring_size(g_ring_info.rx_ring+i));
			num += sprintf(page + num, "rx cur index: %d\n", get_rx_cur_index(g_ring_info.rx_ring+i));
			num += sprintf(page + num, "rx hw index: %d\n", get_rx_hw_index_by_reg(i) );
#endif
			// dump rx descriptor
			for (j=0 ; j < get_rx_ring_size(g_ring_info.rx_ring+i); ++j) {
				RXDesc *rx=rx_buffer->rx_desc;

				num += sprintf(page + num, "#%d: sdp: %x ## sdl %d ## cown: %d ## eor: %d ## fsd: %d ## lsd: %d ## hr: %d ## sp: %d \n", j, rx->sdp, rx->sdl, rx->cown, rx->eor, rx->fsd, rx->lsd, rx->hr, rx->sp);
				++rx_buffer;
			}
		}


	}
}
	return num;
#endif
}

static int cns3xxx_pse_tx_desc_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
#ifdef DEBUG_TX_PROC
	int num = 0;
//if (show_tx_proc)
{ // for debug
	int i=0, j=0;
	TXBuffer *buffer = 0;

	//num += sprintf(page + num, "dscp: %#x\n", dscp);
	//num += sprintf(page + num, "TS_DESC_BASE_ADDR0_REG: %#x\n", TS_DESC_BASE_ADDR0_REG);
	//num += sprintf(page + num, "TS_DESC_PTR0_REG: %#x\n", TS_DESC_PTR0_REG);
	if (g_ring_info.tx_ring) {
		for (i=0 ; i < g_ring_info.num_tx_queues ; ++i) {
			buffer = get_tx_ring_head(g_ring_info.tx_ring+i);

			if (buffer==0)
				break;

			//num += sprintf(page + num, "tx ring ## %d\n", i);
			//num += sprintf(page + num, "get_tx_ring_size(tx_ring): %d\n", get_tx_ring_size(g_ring_info.tx_ring+i));
			//num += sprintf(page + num, "tx cur index: %d\n", get_tx_cur_index(g_ring_info.tx_ring+i));
			//num += sprintf(page + num, "cns3xxx_get_tx_hw_index 0 : %d\n", cns3xxx_get_tx_hw_index(0));
			//num += sprintf(page + num, "rx hw index: %d\n", get_rx_hw_index_by_reg(i) );
			// dump tx descriptor
			for (j=0 ; j < get_tx_ring_size(g_ring_info.tx_ring+i); ++j) {
				TXDesc *tx=buffer->tx_desc;

				num += sprintf(page + num, "#%d: sdp: %x ## sdl %d ## cown: %d ## eor: %d ## fsd: %d ## lsd: %d ## fr: %d ## fp: %d ## ico: %d ## tco: %d ## uco: %d ## pmap: %x\n", j, tx->sdp, tx->sdl, tx->cown, tx->eor, tx->fsd, tx->lsd, tx->fr, tx->fp, tx->ico, tx->tco, tx->uco, tx->pmap);
				++buffer;
			}
		}


	}
}
	return num;
#endif
}
#endif

static int cns3xxx_pse_counter_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int num = 0;
#ifdef CNS3XXX_MAC0_MIB_COUNTER
	num += sprintf(page + num, "C_RXOKPKT_MAC0_REG: %d\n", C_RXOKPKT_MAC0_REG);
	num += sprintf(page + num, "C_RXOKBYTE_MAC0_REG  : %d\n", C_RXOKBYTE_MAC0_REG  );
	num += sprintf(page + num, "C_RXRUNT_MAC0_REG  : %d\n", C_RXRUNT_MAC0_REG  );
	num += sprintf(page + num, "C_RXLONG_MAC0_REG  : %d\n", C_RXLONG_MAC0_REG  );
	num += sprintf(page + num, "C_RXDROP_MAC0_REG  : %d\n", C_RXDROP_MAC0_REG  );
	num += sprintf(page + num, "C_RXCRC_MAC0_REG  : %d\n", C_RXCRC_MAC0_REG  );
	num += sprintf(page + num, "C_RXARLDROP_MAC0_REG  : %d\n", C_RXARLDROP_MAC0_REG  );
	num += sprintf(page + num, "C_VIDROP_MAC0_REG  : %d\n", C_VIDROP_MAC0_REG  );
	num += sprintf(page + num, "C_VEDROP_MAC0_REG  : %d\n", C_VEDROP_MAC0_REG  );
	num += sprintf(page + num, "C_RXRL_MAC0_REG  : %d\n", C_RXRL_MAC0_REG  );
	num += sprintf(page + num, "C_RXPAUSE_MAC0_REG  : %d\n", C_RXPAUSE_MAC0_REG  );

	num += sprintf(page + num, "C_TXOKPKT_MAC0_REG  : %d\n", C_TXOKPKT_MAC0_REG  );
	num += sprintf(page + num, "C_TXOKBYTE_MAC0_REG  : %d\n", C_TXOKBYTE_MAC0_REG  );
	num += sprintf(page + num, "C_TXPAUSECOL_MAC0_REG  : %d\n", C_TXPAUSECOL_MAC0_REG  );
#endif

#ifdef CNS3XXX_MAC1_MIB_COUNTER
	num += sprintf(page + num, "C_RXOKPKT_MAC1_REG  : %d\n", C_RXOKPKT_MAC1_REG  );
	num += sprintf(page + num, "C_RXOKBYTE_MAC1_REG  : %d\n", C_RXOKBYTE_MAC1_REG  );
	num += sprintf(page + num, "C_RXRUNT_MAC1_REG  : %d\n", C_RXRUNT_MAC1_REG  );
	num += sprintf(page + num, "C_RXLONG_MAC1_REG  : %d\n", C_RXLONG_MAC1_REG  );
	num += sprintf(page + num, "C_RXDROP_MAC1_REG  : %d\n", C_RXDROP_MAC1_REG  );
	num += sprintf(page + num, "C_RXCRC_MAC1_REG  : %d\n", C_RXCRC_MAC1_REG  );
	num += sprintf(page + num, "C_RXARLDROP_MAC1_REG  : %d\n", C_RXARLDROP_MAC1_REG  );
	num += sprintf(page + num, "C_VIDROP_MAC1_REG  : %d\n", C_VIDROP_MAC1_REG  );
	num += sprintf(page + num, "C_VEDROP_MAC1_REG  : %d\n", C_VEDROP_MAC1_REG  );
	num += sprintf(page + num, "C_RXRL_MAC1_REG  : %d\n", C_RXRL_MAC1_REG  );
	num += sprintf(page + num, "C_RXPAUSE_MAC1_REG  : %d\n", C_RXPAUSE_MAC1_REG  );

	num += sprintf(page + num, "C_TXOKPKT_MAC1_REG  : %d\n", C_TXOKPKT_MAC1_REG  );
	num += sprintf(page + num, "C_TXOKBYTE_MAC1_REG  : %d\n", C_TXOKBYTE_MAC1_REG  );
	num += sprintf(page + num, "C_TXPAUSECOL_MAC1_REG  : %d\n", C_TXPAUSECOL_MAC1_REG  );
#endif

#ifdef CNS3XXX_CPU_MIB_COUNTER
	num += sprintf(page + num, "C_TSOKPKT_CPU_REG  : %d\n", C_TSOKPKT_CPU_REG  );
	num += sprintf(page + num, "C_TSOKBYTE_CPU_REG  : %d\n", C_TSOKBYTE_CPU_REG  );
	num += sprintf(page + num, "C_TSRUNT_CPU_REG  : %d\n", C_TSRUNT_CPU_REG  );
	num += sprintf(page + num, "C_TSLONG_CPU_REG  : %d\n", C_TSLONG_CPU_REG  );
	num += sprintf(page + num, "C_TSNODSTDROP_CPU_REG  : %d\n", C_TSNODSTDROP_CPU_REG  );
	num += sprintf(page + num, "C_TSARLDROP_CPU_REG  : %d\n", C_TSARLDROP_CPU_REG  );
	num += sprintf(page + num, "C_TSVIDROP_CPU_REG  : %d\n", C_TSVIDROP_CPU_REG  );
	num += sprintf(page + num, "C_TSVEDROP_CPU_REG  : %d\n", C_TSVEDROP_CPU_REG  );
	num += sprintf(page + num, "C_TSRL_CPU_REG  : %d\n", C_TSRL_CPU_REG  );

	num += sprintf(page + num, "C_FSOKPKT_CPU_REG  : %d\n", C_FSOKPKT_CPU_REG  );
	num += sprintf(page + num, "C_FSOKBYTE_CPU_REG  : %d\n", C_FSOKBYTE_CPU_REG  );
#endif

#ifdef CNS3XXX_MAC2_MIB_COUNTER
	num += sprintf(page + num, "C_RXOKPKT_MAC2_REG  : %d\n", C_RXOKPKT_MAC2_REG  );
	num += sprintf(page + num, "C_RXOKBYTE_MAC2_REG  : %d\n", C_RXOKBYTE_MAC2_REG  );
	num += sprintf(page + num, "C_RXRUNT_MAC2_REG  : %d\n", C_RXRUNT_MAC2_REG  );
	num += sprintf(page + num, "C_RXLONG_MAC2_REG  : %d\n", C_RXLONG_MAC2_REG  );
	num += sprintf(page + num, "C_RXDROP_MAC2_REG  : %d\n", C_RXDROP_MAC2_REG  );
	num += sprintf(page + num, "C_RXCRC_MAC2_REG  : %d\n", C_RXCRC_MAC2_REG  );
	num += sprintf(page + num, "C_RXARLDROP_MAC2_REG  : %d\n", C_RXARLDROP_MAC2_REG  );
	num += sprintf(page + num, "C_VIDROP_MAC2_REG  : %d\n", C_VIDROP_MAC2_REG  );
	num += sprintf(page + num, "C_VEDROP_MAC2_REG  : %d\n", C_VEDROP_MAC2_REG  );
	num += sprintf(page + num, "C_RXRL_MAC2_REG  : %d\n", C_RXRL_MAC2_REG  );
	num += sprintf(page + num, "C_RXPAUSE_MAC2_REG  : %d\n", C_RXPAUSE_MAC2_REG  );

	num += sprintf(page + num, "C_TXOKPKT_MAC2_REG  : %d\n", C_TXOKPKT_MAC2_REG  );
	num += sprintf(page + num, "C_TXOKBYTE_MAC2_REG  : %d\n", C_TXOKBYTE_MAC2_REG  );
	num += sprintf(page + num, "C_TXPAUSECOL_MAC2_REG  : %d\n", C_TXPAUSECOL_MAC2_REG  );
#endif
	return num;
}

static int cns3xxx_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int num = 0;
	//int i=0;

#if defined(CNS3XXX_VLAN_8021Q)
	num += sprintf(page + num, "CNS3XXX_VLAN_8021Q Enable\n");
#else
	num += sprintf(page + num, "CNS3XXX_VLAN_8021Q Disable\n");
#endif

#ifdef CONFIG_CNS3XXX_NAPI
	num += sprintf(page + num, "NAPI Enable\n");
#else
	num += sprintf(page + num, "NAPI Disable\n");
#endif
	num += sprintf(page + num, "PHY_CTRL_REG: %x \n", PHY_CTRL_REG);
	num += sprintf(page + num, "PHY_AUTO_ADDR_REG: %x \n", PHY_AUTO_ADDR_REG);
	num += sprintf(page + num, "MAC_GLOB_CFG_REG: %x \n", MAC_GLOB_CFG_REG);
	num += sprintf(page + num, "MAC0_CFG_REG: %x\n", MAC0_CFG_REG);
	num += sprintf(page + num, "MAC1_CFG_REG: %x\n", MAC1_CFG_REG);
	num += sprintf(page + num, "MAC2_CFG_REG: %x\n", MAC2_CFG_REG);
	num += sprintf(page + num, "CPU_CFG_REG: %x\n", CPU_CFG_REG);

	num += sprintf(page + num, "FS_DMA_CTRL0_REG : %#x\n", FS_DMA_CTRL0_REG); 
	num += sprintf(page + num, "FS_DESC_PTR0_REG : %#x\n", FS_DESC_PTR0_REG); 
	num += sprintf(page + num, "FS_DESC_BASE_ADDR0_REG : %#x\n", FS_DESC_BASE_ADDR0_REG); 
	num += sprintf(page + num, "TS_DMA_CTRL0_REG : %#x\n", TS_DMA_CTRL0_REG); 
	num += sprintf(page + num, "TS_DESC_PTR0_REG : %#x\n", TS_DESC_PTR0_REG); 
	num += sprintf(page + num, "TS_DESC_BASE_ADDR0_REG : %#x\n", TS_DESC_BASE_ADDR0_REG); 

	//num += sprintf(page + num, "sizeof(RXDesc) : %d\n", sizeof(RXDesc) );
	num += sprintf(page + num, "DELAY_INTR_CFG_REG: %x \n", DELAY_INTR_CFG_REG);
	num += sprintf(page + num, "VLAN_CFG: %x \n", VLAN_CFG);

#ifdef CNS3XXX_PVID_PROC
	num += sprintf(page + num, "MAC1_MAC0_PVID_REG: %x \n", MAC1_MAC0_PVID_REG);
	num += sprintf(page + num, "MAC2_CPU_PVID_REG: %x \n", MAC2_CPU_PVID_REG);
#endif

	num += sprintf(page + num, "FC_GLOB_THRS_REG: %x \n", FC_GLOB_THRS_REG);
	num += sprintf(page + num, "MC_GLOB_THRS_REG: %x \n", MC_GLOB_THRS_REG);

#ifdef CNS3XXX_DOUBLE_RX_RING
	num += sprintf(page + num, "FS_DMA_CTRL1_REG : %#x\n", FS_DMA_CTRL1_REG); 
	num += sprintf(page + num, "FS_DESC_PTR1_REG : %#x\n", FS_DESC_PTR1_REG); 
	num += sprintf(page + num, "FS_DESC_BASE_ADDR1_REG : %#x\n", FS_DESC_BASE_ADDR1_REG); 
#endif

#ifdef CNS3XXX_DOUBLE_TX_RING
	num += sprintf(page + num, "TS_DMA_CTRL1_REG : %#x\n", TS_DMA_CTRL1_REG); 
	num += sprintf(page + num, "TS_DESC_PTR1_REG : %#x\n", TS_DESC_PTR1_REG); 
	num += sprintf(page + num, "TS_DESC_BASE_ADDR1_REG : %#x\n", TS_DESC_BASE_ADDR1_REG); 
#endif

	num += sprintf(page + num, "SRAM_TEST_REG : %#x\n", SRAM_TEST_REG); 
	num += sprintf(page + num, "DMA_RING_CTRL_REG: %x \n", DMA_RING_CTRL_REG);
	num += sprintf(page + num, "TC_CTRL_REG : %#x\n", TC_CTRL_REG); 
	num += sprintf(page + num, "RATE_CTRL_REG : %#x\n", RATE_CTRL_REG); 
	num += sprintf(page + num, "MAC0_PRI_CTRL_REG : %#x\n", MAC0_PRI_CTRL_REG); 
	num += sprintf(page + num, "MAC1_PRI_CTRL_REG : %#x\n", MAC1_PRI_CTRL_REG); 
	num += sprintf(page + num, "MAC2_PRI_CTRL_REG : %#x\n", MAC2_PRI_CTRL_REG); 
	num += sprintf(page + num, "CPU_PRI_CTRL_REG : %#x\n", CPU_PRI_CTRL_REG); 
	num += sprintf(page + num, "HNAT_PRI_CTRL_REG : %#x\n", HNAT_PRI_CTRL_REG); 
	num += sprintf(page + num, "DMA_AUTO_POLL_CFG_REG : %#x\n", DMA_AUTO_POLL_CFG_REG); 

#ifdef CNS3XXX_SARL_PROC
	num += sprintf(page + num, "SARL_CTRL_REG : %#x\n", SARL_CTRL_REG); 
	num += sprintf(page + num, "SARL_OQ_GTH_REG : %#x\n", SARL_OQ_GTH_REG); 
	num += sprintf(page + num, "SARL_OQ_YTH_REG : %#x\n", SARL_OQ_YTH_REG); 
	num += sprintf(page + num, "SARL_OQ_RTH_REG : %#x\n", SARL_OQ_RTH_REG); 
#endif

	num += sprintf(page + num, "g_ring_info.num_rx_queues: %d\n", g_ring_info.num_rx_queues);
	num += sprintf(page + num, "g_ring_info.num_tx_queues: %d\n", g_ring_info.num_tx_queues);
	num += sprintf(page + num, "g_ring_info.rx_ring: %p\n", g_ring_info.rx_ring);
	num += sprintf(page + num, "g_ring_info.tx_ring: %p\n", g_ring_info.tx_ring);

	num += sprintf(page + num, "ring_index: %d\n", ring_index);


//#ifdef CONFIG_CNS3XXX_PORT_BASE
	if(is_config_cns3xxx_port_base()) {
		num += sprintf(page + num, "CONFIG_PORT_BASE\n");
	}
//#endif

//#ifdef  CONFIG_CNS3XXX_VLAN_BASE
	if(is_config_cns3xxx_vlan_base()) {
		num += sprintf(page + num, "CONFIG_VLAN_BASE\n");
	}
//#endif

//#ifdef CONFIG_HAVE_VLAN_TAG
	if(is_config_have_vlan_tag()) {
		num += sprintf(page + num, "CONFIG_HAVE_VLAN_TAG\n");
	}
//#endif

#ifdef DEBUG_PRIO_IPDSCR
{
	u32 addr=0;

	for (addr=0x50 ; addr <= 0x6c; addr+=4)
		num += sprintf(page + num, "addr %#x: %#x\n", addr, SWITCH_REG_VALUE(addr));
}
#endif

#ifdef CNS3XXX_TX_HW_CHECKSUM
	num += sprintf(page + num, "TX HW CHECKSUM Enable\n");
#else
	num += sprintf(page + num, "TX HW CHECKSUM Disable\n");
#endif

#ifdef CNS3XXX_RX_HW_CHECKSUM
	num += sprintf(page + num, "RX HW CHECKSUM Enable\n");
#else
	num += sprintf(page + num, "RX HW CHECKSUM Disable\n");
#endif
	num += sprintf(page + num, "MSG_LEVEL: %#x\n", MSG_LEVEL);
	num += sprintf(page + num, "C_TXOKPKT_MAC0_EXT_REG: %x\n", C_TXOKPKT_MAC0_EXT_REG);
	num += sprintf(page + num, "C_TXOKBYTE_MAC0_EXT_REG: %x\n", C_TXOKBYTE_MAC0_EXT_REG);





#ifdef CONFIG_SWITCH_BIG_ENDIAN
	num += sprintf(page + num, "switch big endian\n");
#else
	num += sprintf(page + num, "switch little endian\n");
#endif

	num += sprintf(page + num, "MAC_GLOB_CFG_EXT_REG: %x\n", MAC_GLOB_CFG_EXT_REG);
	num += sprintf(page + num, "FC_PORT_THRS_REG: %x\n", FC_PORT_THRS_REG);

#if 0
	num += sprintf(page + num, "GPIOB_PIN_EN_REG: %x\n", GPIOB_PIN_EN_REG);
	num += sprintf(page + num, "PLL_HM_PD_CTRL_REG: %x\n", PLL_HM_PD_CTRL_REG);
	num += sprintf(page + num, "CLK_GATE_REG: %x\n", CLK_GATE_REG);
#endif
	num += sprintf(page + num, "SLK_SKEW_CTRL_REG: %x\n", SLK_SKEW_CTRL_REG);
	num += sprintf(page + num, "IOCDA_REG: %x\n", IOCDA_REG);

#if 0	
	{
	u16 phy_data=0;
	u16 phy_addr=0;

	phy_addr=2;
        cns3xxx_read_phy(phy_addr, 0 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 0 : %x\n", phy_addr, phy_data);
        cns3xxx_read_phy(phy_addr, 1 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 1 : %x\n", phy_addr, phy_data);

        cns3xxx_read_phy(phy_addr, 4, &phy_data);
	num += sprintf(page + num, "phy %d ## register 4 : %x\n", phy_addr, phy_data);
        cns3xxx_read_phy(phy_addr, 5 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 5 : %x\n", phy_addr, phy_data);

        cns3xxx_read_phy(phy_addr, 9 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 9 : %x\n", phy_addr, phy_data);

        cns3xxx_read_phy(phy_addr, 10 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 10 : %x\n", phy_addr, phy_data);

        cns3xxx_read_phy(phy_addr, 15 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 15 : %x\n", phy_addr, phy_data);

        cns3xxx_read_phy(phy_addr, 17 , &phy_data);
	num += sprintf(page + num, "phy %d ## register 17 : %x\n", phy_addr, phy_data);
	}
#endif	
	
	if(jumbo_frame) {
		num += sprintf(page + num, "CNS3XXX JUMBO FRAME Support\n");
	} else {
		num += sprintf(page + num, "CNS3XXX JUMBO FRAME Not Support\n");
	}
	return num;
}

int cns3xxx_write_proc(struct file *file, const char *buffer, unsigned long count, void *data)
{
	if (count) {
		if (buffer[0] == 'r' && buffer[1] == 'g') {
			u8 rgmii = simple_strtol(buffer+3, 0, 0);

		        // enable GMII, MII, reverse MII
		        MAC2_CFG_REG &= (~(1 << 15));

			if (rgmii==1)
		        	MAC2_CFG_REG |= (1 << 15);
		}

		if (buffer[0] == 'r' && buffer[1] == 'x') {
			show_rx_proc = simple_strtol(buffer+3, 0, 0);
			printk("show_rx_proc: %d\n", show_rx_proc);
			return count;
		}
		if (buffer[0] == 't' && buffer[1] == 'x') {
			show_tx_proc = simple_strtol(buffer+3, 0, 0);
			printk("show_tx_proc: %d\n", show_tx_proc);
			return count;
		}
#ifdef CNS3XXX_CONFIG_CHANGE_TX_RING
		if (buffer[0] == 't') {
			ring_index = buffer[2] - '0';
			printk("ring_index: %d\n", ring_index);
		}
#endif

		if (buffer[0] == 'm') {
			MSG_LEVEL = simple_strtol(buffer+2, 0, 0);
			//dscp = simple_strtol("0x4", 0, 0);
			printk("MSG_LEVEL: %#x\n", MSG_LEVEL);
			//printk("buffer+2: %s\n", buffer+2);
		}
#ifdef CONFIG_FAST_BRIDGE
		if (buffer[0] == 'f') {
			fast_bridge_en = simple_strtol(buffer+2, 0, 0);
			printk("fast_bridge_en: %d\n", fast_bridge_en);
		}
#endif
		if (buffer[0] == 'u') {
			u8 e=0;
			e = simple_strtol(buffer+2, 0, 0);
			MAC_GLOB_CFG_REG &= (~(1 << 25));
			if (e == 1)
				MAC_GLOB_CFG_REG |= (1 << 25);
			
		}
		if (buffer[0] == 'n') {
			cns3xxx_nic_mode(0);

			// NIC mode on
			if (buffer[2] == '1') 
				cns3xxx_nic_mode(1);
			
		}
		if (buffer[0] == 'i') {
			u32 intr_mask=0;

			intr_mask = simple_strtol(buffer+2, 0, 0);
			INTR_MASK_REG = intr_mask;
		}
		if (buffer[0] == 'p') {
			CPU_CFG_REG &= (~(1 << 30));

			// 4N mode
			if (buffer[2] == '1') 
				CPU_CFG_REG |= (1 << 30);
		}

		if (buffer[0] == 'x') { 
			if (buffer[2] == '1') 
				enable_tx_dma(0, 1);
			if (buffer[2] == '0') 
				enable_tx_dma(0, 0);
		}
		if (buffer[0] == 'g') { 
			MAC2_CFG_REG &= (~(1<<16));
			if (buffer[2] == '1') 
				MAC2_CFG_REG |= (1<<16);
		}


	}


	return count;
}

static void *dumpTXstart(struct seq_file *m, loff_t *pos);
static void *dumpTXnext(struct seq_file *m, void *v, loff_t *pos);
static void dumpTXstop(struct seq_file *m, void *v);
static int dumpTXshow(struct seq_file *m, void *v);

static void *dumpTXstart(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *dumpTXnext(struct seq_file *m, void *v, loff_t *pos)
{
        ++*pos;
        return NULL;
}

static void dumpTXstop(struct seq_file *m, void *v)
{
}

static int dumpTXshow(struct seq_file *m, void *v)
{
        int i, j;
        TXBuffer *tx_buffer;
        TXDesc *tx_desc;

	seq_printf(m, "g_ring_info.num_tx_queues: %d\n", g_ring_info.num_tx_queues);
	if (g_ring_info.tx_ring) {
		for (i=0 ; i < g_ring_info.num_tx_queues ; ++i) {
			for (j=0 ; j < get_tx_ring_size(g_ring_info.tx_ring+i); ++j) {
				seq_printf(m, "g_ring_info.tx_ring+i->num_free_desc: %d\n", (g_ring_info.tx_ring+i)->num_free_desc);
				tx_buffer = get_tx_ring_head(g_ring_info.tx_ring+i);
				tx_buffer += j;
                		tx_desc = tx_buffer->tx_desc;
				seq_printf(m,"idx: %d ", j);
				if (tx_desc) {
					seq_printf(m,"tx_desc: %p ", tx_desc);
					seq_printf(m,"sdp: %x ", tx_desc->sdp);
#if 1
					seq_printf(m,"sdl: %d ", tx_desc->sdl);
					seq_printf(m,"tco: %d ", tx_desc->tco);
					seq_printf(m,"uco: %d ", tx_desc->uco);
					seq_printf(m,"ico: %d ", tx_desc->ico);
					seq_printf(m,"pri: %d ", tx_desc->pri);
					seq_printf(m,"fp: %d ", tx_desc->fp);
					seq_printf(m,"fr: %d ", tx_desc->fr);
					seq_printf(m,"intr: %d ", tx_desc->interrupt);
					seq_printf(m,"lsd: %d ", tx_desc->lsd);
					seq_printf(m,"fsd: %d ", tx_desc->fsd);
					seq_printf(m,"eor: %d ", tx_desc->eor);
					seq_printf(m,"cown: %d\n", tx_desc->cown);

					seq_printf(m,"ctv: %d ", tx_desc->ctv);
					seq_printf(m,"stv: %d ", tx_desc->stv);
					seq_printf(m,"sid: %d ", tx_desc->sid);
					seq_printf(m,"inss: %d ", tx_desc->inss);
					seq_printf(m,"dels: %d ", tx_desc->dels);
					seq_printf(m,"pmap: %d ", tx_desc->pmap);
					seq_printf(m,"mark: %d ", tx_desc->mark);
					seq_printf(m,"ewan: %d ", tx_desc->ewan);
					seq_printf(m,"fewan: %d\n", tx_desc->fewan);
					
					seq_printf(m,"c_vid: %d ", tx_desc->c_vid);
					seq_printf(m,"c_cfs: %d ", tx_desc->c_cfs);
					seq_printf(m,"c_pri: %d ", tx_desc->c_pri);
					seq_printf(m,"s_vid: %d ", tx_desc->s_vid);
					seq_printf(m,"s_dei: %d ", tx_desc->s_dei);
					seq_printf(m,"s_pri: %d\n", tx_desc->s_pri);
					#endif
				}
			} // for (j=0 ; j < get_tx_ring_size(g_ring_info.tx_ring+i); ++j) 
		} // for (i=0 ; i < g_ring_info.num_tx_queues ; ++i) 
	} // if (g_ring_info.tx_ring) 
	
	return 0;
}

const struct seq_operations dumpTX_ops = {
        .start  = dumpTXstart,
        .next = dumpTXnext,
        .stop = dumpTXstop,
        .show = dumpTXshow
};

static int dumpTX_ops_open(struct inode *inode, struct file *file)
{
        return seq_open(file, &dumpTX_ops);
}


static const struct file_operations dumpTX_fs_ops= { 
        .open           = dumpTX_ops_open,
        .read           = seq_read,
        .llseek         = seq_lseek,
        .release        = seq_release,
};




static void *dumpRXstart(struct seq_file *m, loff_t *pos);
static void *dumpRXnext(struct seq_file *m, void *v, loff_t *pos);
static void dumpRXstop(struct seq_file *m, void *v);
static int dumpRXshow(struct seq_file *m, void *v);

const struct seq_operations dumpRX_ops = {
        .start  = dumpRXstart,
        .next = dumpRXnext,
        .stop = dumpRXstop,
        .show = dumpRXshow
};

static int dumpRX_ops_open(struct inode *inode, struct file *file)
{
        return seq_open(file, &dumpRX_ops);
}

static const struct file_operations dumpRX_fs_ops= {
        .open           = dumpRX_ops_open,
        .read           = seq_read,
        .llseek         = seq_lseek,
        .release        = seq_release,
};


static void *dumpRXstart(struct seq_file *m, loff_t *pos)
{
        return *pos < 1 ? (void *)1 : NULL;
}

static void *dumpRXnext(struct seq_file *m, void *v, loff_t *pos)
{
        ++*pos;
        return NULL;
}

static void dumpRXstop(struct seq_file *m, void *v)
{
}

static int dumpRXshow(struct seq_file *m, void *v)
{
	int i, j;
	RXBuffer *rx_buffer;
	RXDesc *rx_desc;

	seq_printf(m, "g_ring_info.num_rx_queues: %d\n", g_ring_info.num_rx_queues);
	if (g_ring_info.rx_ring) {
		for (i=0 ; i < g_ring_info.num_rx_queues ; ++i) {
			for (j=0 ; j < get_rx_ring_size(g_ring_info.rx_ring+i); ++j) {
				rx_buffer = get_rx_ring_head(g_ring_info.rx_ring+i);
				rx_buffer += j;
                		rx_desc = rx_buffer->rx_desc;
				seq_printf(m,"idx: %d ", j);
				if (rx_desc) 
{

                seq_printf(m,"rx_desc: %p ", rx_desc);
                seq_printf(m,"sdp: %x ", rx_desc->sdp);
                seq_printf(m,"sdl: %d ", rx_desc->sdl);
                seq_printf(m,"l4f: %d ", rx_desc->l4f);
                seq_printf(m,"ipf: %d ", rx_desc->ipf);
                seq_printf(m,"prot: %d ", rx_desc->prot);
                seq_printf(m,"hr: %d ", rx_desc->hr);
                seq_printf(m,"lsd: %d ", rx_desc->lsd);
                seq_printf(m,"fsd: %d ", rx_desc->fsd);
                seq_printf(m,"eor: %d ", rx_desc->eor);
                seq_printf(m,"cown: %d\n", rx_desc->cown);

                seq_printf(m,"ctv: %d ", rx_desc->ctv);
                seq_printf(m,"stv: %d ", rx_desc->stv);
                seq_printf(m,"unv: %d ", rx_desc->unv);
                seq_printf(m,"iwan: %d ", rx_desc->iwan);
                seq_printf(m,"exdv: %d ", rx_desc->exdv);
                seq_printf(m,"sp: %d ", rx_desc->sp);
                seq_printf(m,"crc_err: %d ", rx_desc->crc_err);
                seq_printf(m,"un_eth: %d ", rx_desc->un_eth);
                seq_printf(m,"tc: %d ", rx_desc->tc);
                seq_printf(m,"ip_offset: %d\n", rx_desc->ip_offset);

                seq_printf(m,"c_vid: %d ", rx_desc->c_vid);
                seq_printf(m,"c_cfs: %d ", rx_desc->c_cfs);
                seq_printf(m,"c_pri: %d ", rx_desc->c_pri);
                seq_printf(m,"s_vid: %d ", rx_desc->s_vid);
                seq_printf(m,"s_dei: %d ", rx_desc->s_dei);
                seq_printf(m,"s_pri: %d\n", rx_desc->s_pri);

				}
			} // for (j=0 ; j < get_rx_ring_size(g_ring_info.rx_ring+i); ++j) 
		} // for (i=0 ; i < g_ring_info.num_rx_queues ; ++i) 
	} // if (g_ring_info.rx_ring) 
	
	return 0;
}

static int cns3xxx_proc_remove(void)
{

   
        remove_proc_entry("pse", cns3xxx_proc_dir);
        remove_proc_entry("pse_regs", cns3xxx_proc_dir);
        remove_proc_entry("pse_counter", cns3xxx_proc_dir);
        remove_proc_entry("pse_tx_desc", cns3xxx_proc_dir);
        remove_proc_entry("pse_rx_desc", cns3xxx_proc_dir);
        
    		return 0;
}

static int __init cns3xxx_proc_init(void)
{
	struct proc_dir_entry *switch_proc_entry=0;
		
        switch_proc_entry = create_proc_entry("pse", S_IFREG | S_IRUGO, cns3xxx_proc_dir);
        if (switch_proc_entry) {
                switch_proc_entry->read_proc = cns3xxx_read_proc;
                switch_proc_entry->write_proc = cns3xxx_write_proc;
        }

        switch_proc_entry = create_proc_entry("pse_counter", S_IFREG | S_IRUGO, cns3xxx_proc_dir);
        if (switch_proc_entry) {
                switch_proc_entry->read_proc = cns3xxx_pse_counter_read_proc;
        }

#if 0
        switch_proc_entry = create_proc_entry("pse_rx_desc", S_IFREG | S_IRUGO, cns3xxx_proc_dir);
        if (switch_proc_entry) {
                switch_proc_entry->read_proc = cns3xxx_pse_rx_desc_read_proc;
        }
        switch_proc_entry = create_proc_entry("pse_tx_desc", S_IFREG | S_IRUGO, cns3xxx_proc_dir);
        if (switch_proc_entry) {
                switch_proc_entry->read_proc = cns3xxx_pse_tx_desc_read_proc;
        }
#endif
        switch_proc_entry = create_proc_entry("pse_regs", S_IFREG | S_IRUGO, cns3xxx_proc_dir);
        if (switch_proc_entry) {
                switch_proc_entry->read_proc = cns3xxx_pse_regs_read_proc;
        }

#if 0
	proc_create("cns3xxx/cnsxtxdescs", 0, NULL, &dumpTX_fs_ops);
	proc_create("cns3xxx/cnsxrxdescs", 0, NULL, &dumpRX_fs_ops);
#endif
	proc_create("cns3xxx/pse_tx_desc", 0, NULL, &dumpTX_fs_ops);
	proc_create("cns3xxx/pse_rx_desc", 0, NULL, &dumpRX_fs_ops);

        return 1;
}

static int cns3xxx_notify_reboot(struct notifier_block *nb, unsigned long event, void *ptr)
{
	// stop the DMA
	enable_rx_dma(0, 0);
	enable_tx_dma(0, 0);
	enable_rx_dma(1, 0);
	enable_tx_dma(1, 0);

	// disable Port 0
	enable_port(0, 0); 
	enable_port(1, 0); 
	enable_port(2, 0); 
	enable_port(3, 0); 

	// disable phy auto-poll
	PHY_AUTO_ADDR_REG &= ~((1<<5) | (1<<13) | (1<<21));
	//printk("disable phy auto-poll\r\n");
	// wait state machine idle
	mdelay(1000);
	//printk("wait 1 sec end\r\n");
	
	return NOTIFY_DONE;
}

#ifdef CONFIG_CNS3XXX_NAPI
static struct net_device *init_napi_dev(struct net_device *ndev, const RingInfo *ring_info)
{
	CNS3XXXPrivate *priv;

	ndev = alloc_etherdev(sizeof(CNS3XXXPrivate));
	if (!ndev) {
		printk("Cannot allocate NAPI virtual device \n");
		BUG();
	}
	priv = netdev_priv(ndev);
	memset(priv, 0, sizeof(CNS3XXXPrivate));

	//priv = netdev_priv(napi_dev);
	priv->num_rx_queues = ring_info->num_rx_queues;
	priv->num_tx_queues = ring_info->num_tx_queues;
	priv->rx_ring = ring_info->rx_ring;
	priv->tx_ring = ring_info->tx_ring;
	//priv->is_qf=0; // because of memset, so need not the line

	//printk("ndev netdev: %s\n", ndev->name);
	//printk("ndev addr: %p\n", ndev);
	netif_napi_add(ndev, &priv->napi , cns3xxx_poll, CNS3XXX_NAPI_WEIGHT);
        dev_hold(ndev);
	set_bit(__LINK_STATE_START, &ndev->state);

	return ndev;
}
#endif


void cns3xxx_config_intr(void)
{
	void set_interrupt_type(u32 id, u32 type);
	void get_interrupt_type(u32 id, u32 *type);

	void set_interrupt_pri(u32 id, u32 pri);
	void get_interrupt_pri(u32 id, u32 *pri);

	u32 v=0xffffffff;

	get_interrupt_type(FSRC_RING0_INTERRUPT_ID, &v);
#if 1
	set_interrupt_type(FSRC_RING0_INTERRUPT_ID, RISING_EDGE);
	get_interrupt_type(FSRC_RING0_INTERRUPT_ID, &v);

	get_interrupt_type(FSRC_RING1_INTERRUPT_ID, &v);
	set_interrupt_type(FSRC_RING1_INTERRUPT_ID, RISING_EDGE);
	get_interrupt_type(FSRC_RING1_INTERRUPT_ID, &v);

	get_interrupt_type(FSQF_RING0_INTERRUPT_ID, &v);
	set_interrupt_type(FSQF_RING0_INTERRUPT_ID, RISING_EDGE);
	get_interrupt_type(FSQF_RING0_INTERRUPT_ID, &v);

	get_interrupt_type(FSQF_RING1_INTERRUPT_ID, &v);
	set_interrupt_type(FSQF_RING1_INTERRUPT_ID, RISING_EDGE);
	get_interrupt_type(FSQF_RING1_INTERRUPT_ID, &v);

	#ifdef CNS3XXX_USE_MASK
		get_interrupt_pri(FSRC_RING0_INTERRUPT_ID, &v);
		set_interrupt_pri(FSRC_RING0_INTERRUPT_ID, 0xc);
		get_interrupt_pri(FSRC_RING0_INTERRUPT_ID, &v);

		get_interrupt_pri(FSRC_RING1_INTERRUPT_ID, &v);
		set_interrupt_pri(FSRC_RING1_INTERRUPT_ID, 0xc);
		get_interrupt_pri(FSRC_RING1_INTERRUPT_ID, &v);

		get_interrupt_pri(FSQF_RING1_INTERRUPT_ID, &v);
		set_interrupt_pri(FSQF_RING1_INTERRUPT_ID, 0xc);
		get_interrupt_pri(FSQF_RING1_INTERRUPT_ID, &v);

	#ifndef CONFIG_CNS3XXX_NAPI
		set_interrupt_pri(FSQF_RING0_INTERRUPT_ID, 0xc);
	#endif
	

	#endif // CNS3XXX_USE_MASK
#endif
}

#define PSE_DEFAULT_FREE_PAGE_COUNT	0x2F6
static inline void empty_rx(void)
{
	/* FIXME: 
	 * this function recevie all packets (in PSE memory) and drop them.
	 * If SRAM_TEST_REG[9:0] is 0x2F6, we think that PSE is idle
	 */
    struct net_device *netdev = PORT0_NETDEV;
    int work_done;
#ifdef DEBUG
	int cnt = 0;
#endif
	
    CNS3XXXPrivate *priv = netdev_priv(netdev);
	while ((SRAM_TEST_REG &0x3FF) < PSE_DEFAULT_FREE_PAGE_COUNT) {
		work_done = 0;
#ifdef CONFIG_CNS3XXX_NAPI
	    cns3xxx_receive_packet(priv, 0, &work_done, 16);
#else
	    cns3xxx_receive_packet(priv, 0);
#endif
	/*XXX: for debug */
	#if 0 
		cnt++;
		if (cnt>100) {
			printk("%s: count %d, SRAM_TEST_REG: 0x%.8x\n", __FUNCTION__, cnt, SRAM_TEST_REG);
			printk("MAC0_CFG_REG 0x%.8x MAC1_CFG_REG 0x%.8x MAC2_CFG_REG 0x%.8x\n", MAC0_CFG_REG, MAC1_CFG_REG, MAC2_CFG_REG);
			break;
		}
	#endif
	}
}

static int is_tx_empty(TXRing *tx_ring)
{

	u32 cur_index, ring_size = get_tx_ring_size(tx_ring);
	TXDesc *tx_desc;

	for (cur_index=0; cur_index<ring_size; cur_index++) {
		tx_desc = (get_tx_buffer_by_index(tx_ring, cur_index))->tx_desc;

		if (0 == tx_desc->cown) {
			return 0;
		}
	}
	return 1;	
}

static inline int pse_suspend(void)
{
    struct net_device *netdev = PORT0_NETDEV;
    CNS3XXXPrivate *priv = netdev_priv(netdev);

#ifdef CONFIG_CPU_FREQ
	atomic_set(&cpufreq_pse_flag, 1);
#endif
	/* 1. suspend tx dma and disable TX DMA */
	while(!is_tx_empty(priv->tx_ring));
	while (TS_DMA_STA_REG); /* wait for TX DMA complete */
	DMA_AUTO_POLL_CFG_REG |= (0x1<<4);
	enable_tx_dma(0, 0);
	enable_tx_dma(1, 0);
	/* 2. disable mac port */
	enable_port(0, 0);
	enable_port(1, 0);
	enable_port(2, 0);
	enable_port(3, 0);
 	/* 3. empty PSE memory */
	empty_rx();
 	/* 4. suspend rx dma and disable RX DMA */
	while (FS_DMA_STA_REG); /* wait for RX DMA complete */
	DMA_AUTO_POLL_CFG_REG |= 0x1;
	enable_rx_dma(0, 0);
	enable_rx_dma(1, 0);
	INTR_MASK_REG=0xffffffff;

    return NOTIFY_DONE;
}

static inline int pse_resume(void)
{
	/*  2. resume tx dma */
	while (TS_DMA_STA_REG);
	DMA_AUTO_POLL_CFG_REG &= ~(0x1<<4);
	enable_tx_dma(0, 1);
	enable_tx_dma(1, 1);
	/*  3. resume rx dma */
	while (FS_DMA_STA_REG);
	DMA_AUTO_POLL_CFG_REG &= ~ 0x1;
	enable_rx_dma(0, 1);
	enable_rx_dma(1, 1);
	/*  1. enable mac port */
	enable_port(0, 1);
	enable_port(1, 1);
	enable_port(2, 1);
	enable_port(3, 1);
	INTR_MASK_REG=0x0;
#ifdef CONFIG_CPU_FREQ
	atomic_set(&cpufreq_pse_flag, 0);
#endif
    return NOTIFY_DONE;
}

#ifdef CONFIG_CNS3XXX_PSE_WOL
static int pse_wol_suspend(void)
{
	INTR_STAT_REG = INTR_STAT_REG; /* clear interrupt status */
	INTR_MASK_REG = 0xE3FFFFFF; /* Only enable pause frame drop interrupt */
	enable_port(3, 0); /* disable CPU port */
    return NOTIFY_DONE;
}

static int pse_wol_resume(void)
{
	INTR_STAT_REG = INTR_STAT_REG; /* clear interrupt status*/
	INTR_MASK_REG = 0x0; /* */
	enable_port(3, 1); /* enable CPU port */
	
    return NOTIFY_DONE;
}
#endif

#ifdef CONFIG_CPU_FREQ
static int pse_cpufreq_notifier(struct notifier_block *nb, unsigned long phase, void *p)
{
	if (unlikely(!rc_setup_rx_tx)) {
		return NOTIFY_DONE;
	}

	switch (phase) {
	case CPUFREQ_POSTCHANGE:
		pse_resume();
		break;
	case CPUFREQ_PRECHANGE:
		pse_suspend();
		break;
	default:
		break; 
	}
	return NOTIFY_DONE;
}

static struct notifier_block pse_cpufreq_notifier_block = {
	.notifier_call = pse_cpufreq_notifier
};
#endif /* #ifdef CONFIG_CPU_FREQ */

#ifdef CONFIG_SUSPEND
/*
 * This function is called for suspend and resume.
 */
static int pse_power_event(struct notifier_block *this, unsigned long event,
                 void *ptr)
{
	if (unlikely(!rc_setup_rx_tx)) {
		return NOTIFY_DONE;
	}

    switch (event) {
    case PM_POST_SUSPEND:
#ifdef CONFIG_CNS3XXX_PSE_WOL
		return pse_wol_resume();
#endif
    case PM_POST_HIBERNATION:
        return pse_resume();
    case PM_SUSPEND_PREPARE:
#ifdef CONFIG_CNS3XXX_PSE_WOL
		return pse_wol_suspend();
#endif
    case PM_HIBERNATION_PREPARE:
        return pse_suspend();
    default:
        return NOTIFY_DONE;
    }
}

static struct notifier_block pse_power_notifier = {
    .notifier_call = pse_power_event,
};
#endif

static int __init cns3xxx_init_module(void)
{
#if 1
	// when tx_ring/rx_ring alloc memory, 
	// don't free them until cns3xxx_exit_module
	
	RingInfo ring_info; 
	int i=0;
	//spin_lock_init(&star_gsw_send_lock);


#ifdef CNS3XXX_DOUBLE_RX_RING
        ring_info.num_rx_queues = 2;
#else
        ring_info.num_rx_queues = 1;
#endif

#ifdef CNS3XXX_DOUBLE_TX_RING
        ring_info.num_tx_queues = 2;
#else
        ring_info.num_tx_queues = 1;
#endif
	ring_info.rx_ring = kcalloc(ring_info.num_rx_queues, sizeof(RXRing), GFP_KERNEL);
	if (!ring_info.rx_ring)
		return -ENOMEM;
	
	//printk("ring_info.rx_ring: %p\n", ring_info.rx_ring);
	for (i=0 ; i < ring_info.num_rx_queues ; ++i) {
		memset(ring_info.rx_ring + i, 0, sizeof(RXRing));
	}


	ring_info.tx_ring = kcalloc(ring_info.num_tx_queues, sizeof(TXRing), GFP_KERNEL);


	if (!ring_info.tx_ring)
		return -ENOMEM;
	//printk("ring_info.tx_ring: %p\n", ring_info.tx_ring);

	for (i=0 ; i < ring_info.num_tx_queues ; ++i) {
		memset(ring_info.tx_ring + i, 0, sizeof(TXRing));
	}


	g_ring_info = ring_info;

//#else

cns3xxx_gsw_up_init();

{
  u32 reg_config = 0;

  reg_config = PHY_AUTO_ADDR_REG;
  reg_config &= ~(3 << 30);
	if(jumbo_frame) {
	  reg_config |= (3 << 30); // maximum frame length: 9600 bytes
	} else {
  	reg_config |= (2 << 30); // maximum frame length: 1536 bytes
	}

  PHY_AUTO_ADDR_REG = reg_config;
  
	if(jumbo_frame) {
		MAX_PACKET_LEN = 9600;
		printk("jumbo_frame on\n");
		printk("MAX_PACKET_LEN:%d\n", MAX_PACKET_LEN);
	}
	else {
		MAX_PACKET_LEN = 1536;
		printk("jumbo_frame off\n");
		printk("MAX_PACKET_LEN:%d\n", MAX_PACKET_LEN);
	}
}

	cns3xxx_probe(ring_info);
	cns3xxx_config_intr();

#ifdef CNS3XXX_VLAN_8021Q
//#ifdef CNS3XXX_NIC_MODE_8021Q
	if(is_cns3xxx_nic_mode_8021q()) {
		cns3xxx_nic_mode(1);
	}
//#endif
#endif
	spin_lock_init(&tx_lock);
	spin_lock_init(&rx_lock);

#ifdef CONFIG_CNS3XXX_NAPI
	napi_dev = init_napi_dev(napi_dev, &ring_info);
	#ifdef CNS3XXX_DOUBLE_RX_RING
	r1_napi_dev = init_napi_dev(r1_napi_dev, &ring_info);
	#endif
#endif

	cns3xxx_proc_init();
	register_reboot_notifier(&cns3xxx_notifier_reboot);
	clear_fs_dma_state(0);

	if (ring_info.num_rx_queues == 2) {
		// enable RX dobule ring
		DMA_RING_CTRL_REG |= 1;
	}

	if (ring_info.num_tx_queues == 2 ) {
		// enable TX dobule ring
		DMA_RING_CTRL_REG |= (1 << 16);
	}
#ifdef CONFIG_SUSPEND
    register_pm_notifier(&pse_power_notifier);
#endif

#ifdef CONFIG_CPU_FREQ
	atomic_set(&cpufreq_pse_flag, 0);
	if (cpufreq_register_notifier(&pse_cpufreq_notifier_block, 
						CPUFREQ_TRANSITION_NOTIFIER)) {
		printk("%s: Failed to setup cpufreq notifier\n", __FUNCTION__);
	}
#endif

#endif
	return 0;
}

static void __exit cns3xxx_exit_module(void)
{
#if 1
	int i=0;

	//printk("g_ring_info.rx_ring: %p\n", g_ring_info.rx_ring);
	//printk("g_ring_info.tx_ring: %p\n", g_ring_info.tx_ring);
	kfree(g_ring_info.rx_ring);
	kfree(g_ring_info.tx_ring);

	cns3xxx_proc_remove();


	for (i=0 ; i < NETDEV_SIZE ; ++i) {
		CNS3XXXPrivate *priv = 0;

		if (net_dev_array[i]){
			priv = netdev_priv(net_dev_array[i]);

#if 0
			if (priv->tx_ring!=0){
				kfree(priv->tx_ring);
				priv->tx_ring = 0;
			}

			if (priv->rx_ring!=0) {
				kfree(priv->rx_ring);
				priv->rx_ring = 0;
			}
#endif

			unregister_netdev(net_dev_array[i]);
			free_netdev(net_dev_array[i]);
		}


#if 0
		sprintf(netdev_name, "eth%d", i);
		//printk("net_dev_array[0]: %x\n", net_dev_array[0]);
		netdev=__dev_get_by_name(&init_net, netdev_name);
		// if no unregister_netdev and free_netdev,
		// after remove module, ifconfig will hang.
		if (netdev) {
			unregister_netdev(netdev);
			free_netdev(netdev);
		}
#endif
	}
#else
#endif

#ifdef CONFIG_CNS3XXX_NAPI
	free_netdev(napi_dev);
	#ifdef CNS3XXX_DOUBLE_RX_RING
	free_netdev(r1_napi_dev);
	#endif
#endif

#ifdef CONFIG_CPU_FREQ
	cpufreq_unregister_notifier(&pse_cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
#endif

	unregister_reboot_notifier(&cns3xxx_notifier_reboot);
	printk("remove cns3xxx pse module\n");

#if 0
	// disable phy auto-poll
	PHY_AUTO_ADDR_REG &= ~((1<<5) | (1<<13) | (1<<21));
	mdelay(1000); // wait state machine idle
#endif
}


// this snippet code ref 8139cp.c
#if defined(CNS3XXX_VLAN_8021Q)
void cns3xxx_vlan_rx_register(struct net_device *dev, struct vlan_group *grp)
{
        CNS3XXXPrivate *priv = netdev_priv(dev);
        unsigned long flags;

        spin_lock_irqsave(&priv->lock, flags);
        printk("gsw_vlan_rx_register\n");
        priv->vlgrp = grp;
        spin_unlock_irqrestore(&priv->lock, flags);
}

void cns3xxx_vlan_rx_kill_vid(struct net_device *dev, unsigned short vid)
{
        CNS3XXXPrivate *priv = netdev_priv(dev);
        unsigned long flags;

        spin_lock_irqsave(&priv->lock, flags);
        printk("gsw_vlan_rx_kill_vid\n");
        // reference: linux-2.6.24-current/drivers/netvia-velocity.c
        vlan_group_set_device(priv->vlgrp, vid, NULL);
                //priv->vlgrp->vlan_devices[vid] = NULL;
        spin_unlock_irqrestore(&priv->lock, flags);
}

#endif
u32 cns3xxx_main;
EXPORT_SYMBOL(cns3xxx_main);

MODULE_AUTHOR("Cavium Networks, <tech@XXXX.com>");
MODULE_DESCRIPTION("CNS3XXX Switch Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(cns3xxx_init_module);
module_exit(cns3xxx_exit_module);

