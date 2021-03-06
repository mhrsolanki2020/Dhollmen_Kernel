/******************************************************************************
 * Copyright(c) 2008 - 2010 Realtek Corporation. All rights reserved.
 * Linux device driver for RTL8192E
 *
 * Based on the r8180 driver, which is:
 * Copyright 2004-2005 Andrea Merello <andreamrl@tiscali.it>, et al.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 * Jerry chuang <wlanfae@realtek.com>
 */

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "r8192E_hw.h"
#include "r8192E.h"
#include "r8190_rtl8256.h" /* RTL8225 Radio frontend */
#include "r8180_93cx6.h"   /* Card EEPROM */
#include "r8192E_wx.h"
#include "r819xE_phy.h" //added by WB 4.30.2008
#include "r819xE_phyreg.h"
#include "r819xE_cmdpkt.h"
#include "r8192E_dm.h"

#ifdef CONFIG_PM
#include "r8192_pm.h"
#endif

#ifdef ENABLE_DOT11D
#include "ieee80211/dot11d.h"
#endif

//set here to open your trace code. //WB
u32 rt_global_debug_component = COMP_ERR ; //always open err flags on

static DEFINE_PCI_DEVICE_TABLE(rtl8192_pci_id_tbl) = {
	/* Realtek */
	{ PCI_DEVICE(0x10ec, 0x8192) },

	/* Corega */
	{ PCI_DEVICE(0x07aa, 0x0044) },
	{ PCI_DEVICE(0x07aa, 0x0047) },
	{}
};

static char ifname[IFNAMSIZ] = "wlan%d";
static int hwwep = 1; //default use hw. set 0 to use software security
static int channels = 0x3fff;

MODULE_LICENSE("GPL");
MODULE_VERSION("V 1.1");
MODULE_DEVICE_TABLE(pci, rtl8192_pci_id_tbl);
//MODULE_AUTHOR("Andrea Merello <andreamrl@tiscali.it>");
MODULE_DESCRIPTION("Linux driver for Realtek RTL819x WiFi cards");

module_param_string(ifname, ifname, sizeof(ifname), S_IRUGO|S_IWUSR);
module_param(hwwep,int, S_IRUGO|S_IWUSR);
module_param(channels,int, S_IRUGO|S_IWUSR);

MODULE_PARM_DESC(ifname," Net interface name, wlan%d=default");
MODULE_PARM_DESC(hwwep," Try to use hardware WEP support. Still broken and not available on all cards");
MODULE_PARM_DESC(channels," Channel bitmask for specific locales. NYI");

static int __devinit rtl8192_pci_probe(struct pci_dev *pdev,
			 const struct pci_device_id *id);
static void __devexit rtl8192_pci_disconnect(struct pci_dev *pdev);

static struct pci_driver rtl8192_pci_driver = {
	.name		= RTL819xE_MODULE_NAME,	          /* Driver name   */
	.id_table	= rtl8192_pci_id_tbl,	          /* PCI_ID table  */
	.probe		= rtl8192_pci_probe,	          /* probe fn      */
	.remove		= __devexit_p(rtl8192_pci_disconnect),	  /* remove fn     */
#ifdef CONFIG_PM
	.suspend	= rtl8192E_suspend,	          /* PM suspend fn */
	.resume		= rtl8192E_resume,                 /* PM resume fn  */
#else
	.suspend	= NULL,			          /* PM suspend fn */
	.resume		= NULL,			          /* PM resume fn  */
#endif
};

static void rtl8192_start_beacon(struct ieee80211_device *ieee80211);
static void rtl8192_stop_beacon(struct ieee80211_device *ieee80211);
static void rtl819x_watchdog_wqcallback(struct work_struct *work);
static void rtl8192_irq_rx_tasklet(unsigned long arg);
static void rtl8192_irq_tx_tasklet(unsigned long arg);
static void rtl8192_prepare_beacon(unsigned long arg);
static irqreturn_t rtl8192_interrupt(int irq, void *param);
static void rtl819xE_tx_cmd(struct r8192_priv *priv, struct sk_buff *skb);
static void rtl8192_update_ratr_table(struct r8192_priv *priv);
static void rtl8192_restart(struct work_struct *work);
static void watch_dog_timer_callback(unsigned long data);
static int _rtl8192_up(struct r8192_priv *priv);
static void rtl8192_cancel_deferred_work(struct r8192_priv* priv);
static short rtl8192_tx(struct r8192_priv *priv, struct sk_buff* skb);

#ifdef ENABLE_DOT11D

typedef struct _CHANNEL_LIST
{
	u8	Channel[32];
	u8	Len;
}CHANNEL_LIST, *PCHANNEL_LIST;

static const CHANNEL_LIST ChannelPlan[] = {
	{{1,2,3,4,5,6,7,8,9,10,11,36,40,44,48,52,56,60,64,149,153,157,161,165},24},  		//FCC
	{{1,2,3,4,5,6,7,8,9,10,11},11},                    				//IC
	{{1,2,3,4,5,6,7,8,9,10,11,12,13,36,40,44,48,52,56,60,64},21},  	//ETSI
	{{1,2,3,4,5,6,7,8,9,10,11,12,13},13},    //Spain. Change to ETSI.
	{{1,2,3,4,5,6,7,8,9,10,11,12,13},13},  	//France. Change to ETSI.
	{{1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64},22},	//MKK					//MKK
	{{1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64},22},//MKK1
	{{1,2,3,4,5,6,7,8,9,10,11,12,13},13},	//Israel.
	{{1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64},22},			// For 11a , TELEC
	{{1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64}, 22},    //MIC
	{{1,2,3,4,5,6,7,8,9,10,11,12,13,14},14}					//For Global Domain. 1-11:active scan, 12-14 passive scan. //+YJ, 080626
};

static void rtl819x_set_channel_map(u8 channel_plan, struct r8192_priv* priv)
{
	int i, max_chan=-1, min_chan=-1;
	struct ieee80211_device* ieee = priv->ieee80211;
	switch (channel_plan)
	{
		case COUNTRY_CODE_FCC:
		case COUNTRY_CODE_IC:
		case COUNTRY_CODE_ETSI:
		case COUNTRY_CODE_SPAIN:
		case COUNTRY_CODE_FRANCE:
		case COUNTRY_CODE_MKK:
		case COUNTRY_CODE_MKK1:
		case COUNTRY_CODE_ISRAEL:
		case COUNTRY_CODE_TELEC:
		case COUNTRY_CODE_MIC:
		{
			Dot11d_Init(ieee);
			ieee->bGlobalDomain = false;
                        //acturally 8225 & 8256 rf chip only support B,G,24N mode
			min_chan = 1;
			max_chan = 14;

			if (ChannelPlan[channel_plan].Len != 0){
				// Clear old channel map
				memset(GET_DOT11D_INFO(ieee)->channel_map, 0, sizeof(GET_DOT11D_INFO(ieee)->channel_map));
				// Set new channel map
				for (i=0;i<ChannelPlan[channel_plan].Len;i++)
				{
	                                if (ChannelPlan[channel_plan].Channel[i] < min_chan || ChannelPlan[channel_plan].Channel[i] > max_chan)
					    break;
					GET_DOT11D_INFO(ieee)->channel_map[ChannelPlan[channel_plan].Channel[i]] = 1;
				}
			}
			break;
		}
		case COUNTRY_CODE_GLOBAL_DOMAIN:
		{
			GET_DOT11D_INFO(ieee)->bEnabled = 0; //this flag enabled to follow 11d country IE setting, otherwise, it shall follow global domain setting
			Dot11d_Reset(ieee);
			ieee->bGlobalDomain = true;
			break;
		}
		default:
			break;
	}
}
#endif

static inline bool rx_hal_is_cck_rate(prx_fwinfo_819x_pci pdrvinfo)
{
	return (pdrvinfo->RxRate == DESC90_RATE1M ||
		pdrvinfo->RxRate == DESC90_RATE2M ||
		pdrvinfo->RxRate == DESC90_RATE5_5M ||
		pdrvinfo->RxRate == DESC90_RATE11M) &&
		!pdrvinfo->RxHT;
}

void CamResetAllEntry(struct r8192_priv* priv)
{
	write_nic_dword(priv, RWCAM, BIT31|BIT30);
}

void write_cam(struct r8192_priv *priv, u8 addr, u32 data)
{
        write_nic_dword(priv, WCAMI, data);
        write_nic_dword(priv, RWCAM, BIT31|BIT16|(addr&0xff) );
}

u32 read_cam(struct r8192_priv *priv, u8 addr)
{
        write_nic_dword(priv, RWCAM, 0x80000000|(addr&0xff) );
        return read_nic_dword(priv, 0xa8);
}

u8 read_nic_byte(struct r8192_priv *priv, int x)
{
        return 0xff & readb(priv->mem_start + x);
}

u32 read_nic_dword(struct r8192_priv *priv, int x)
{
        return readl(priv->mem_start + x);
}

u16 read_nic_word(struct r8192_priv *priv, int x)
{
        return readw(priv->mem_start + x);
}

void write_nic_byte(struct r8192_priv *priv, int x,u8 y)
{
        writeb(y, priv->mem_start + x);
	udelay(20);
}

void write_nic_dword(struct r8192_priv *priv, int x,u32 y)
{
        writel(y, priv->mem_start + x);
	udelay(20);
}

void write_nic_word(struct r8192_priv *priv, int x,u16 y)
{
        writew(y, priv->mem_start + x);
	udelay(20);
}

u8 rtl8192e_ap_sec_type(struct ieee80211_device *ieee)
{
	static const u8 ccmp_ie[4] = {0x00,0x50,0xf2,0x04};
	static const u8 ccmp_rsn_ie[4] = {0x00, 0x0f, 0xac, 0x04};
	int wpa_ie_len= ieee->wpa_ie_len;
	struct ieee80211_crypt_data* crypt;
	int encrypt;

	crypt = ieee->crypt[ieee->tx_keyidx];

	encrypt = (ieee->current_network.capability & WLAN_CAPABILITY_PRIVACY) ||
		  (ieee->host_encrypt && crypt && crypt->ops &&
		   (0 == strcmp(crypt->ops->name,"WEP")));

	/* simply judge  */
	if(encrypt && (wpa_ie_len == 0)) {
		// wep encryption, no N mode setting */
		return SEC_ALG_WEP;
	} else if((wpa_ie_len != 0)) {
		// parse pairwise key type */
		if (((ieee->wpa_ie[0] == 0xdd) && (!memcmp(&(ieee->wpa_ie[14]),ccmp_ie,4))) ||
				((ieee->wpa_ie[0] == 0x30) && (!memcmp(&ieee->wpa_ie[10],ccmp_rsn_ie, 4))))
			return SEC_ALG_CCMP;
		else
			return SEC_ALG_TKIP;
	} else {
		return SEC_ALG_NONE;
	}
}

void rtl8192e_SetHwReg(struct ieee80211_device *ieee80211, u8 variable, u8 *val)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);

	switch(variable)
	{

		case HW_VAR_BSSID:
			write_nic_dword(priv, BSSIDR, ((u32*)(val))[0]);
			write_nic_word(priv, BSSIDR+2, ((u16*)(val+2))[0]);
		break;

		case HW_VAR_MEDIA_STATUS:
		{
			RT_OP_MODE	OpMode = *((RT_OP_MODE *)(val));
			u8		btMsr = read_nic_byte(priv, MSR);

			btMsr &= 0xfc;

			switch(OpMode)
			{
			case RT_OP_MODE_INFRASTRUCTURE:
				btMsr |= MSR_INFRA;
				break;

			case RT_OP_MODE_IBSS:
				btMsr |= MSR_ADHOC;
				break;

			case RT_OP_MODE_AP:
				btMsr |= MSR_AP;
				break;

			default:
				btMsr |= MSR_NOLINK;
				break;
			}

			write_nic_byte(priv, MSR, btMsr);
		}
		break;

		case HW_VAR_CHECK_BSSID:
		{
			u32	RegRCR, Type;

			Type = ((u8*)(val))[0];
			RegRCR = read_nic_dword(priv, RCR);
			priv->ReceiveConfig = RegRCR;

			if (Type == true)
				RegRCR |= (RCR_CBSSID);
			else if (Type == false)
				RegRCR &= (~RCR_CBSSID);

			write_nic_dword(priv, RCR,RegRCR);
			priv->ReceiveConfig = RegRCR;

		}
		break;

		case HW_VAR_SLOT_TIME:
		{
			priv->slot_time = val[0];
			write_nic_byte(priv, SLOT_TIME, val[0]);

		}
		break;

		case HW_VAR_ACK_PREAMBLE:
		{
			u32 regTmp = 0;
			priv->short_preamble = (bool)(*(u8*)val );
			regTmp = priv->basic_rate;
			if (priv->short_preamble)
				regTmp |= BRSR_AckShortPmb;
			write_nic_dword(priv, RRSR, regTmp);
		}
		break;

		case HW_VAR_CPU_RST:
			write_nic_dword(priv, CPU_GEN, ((u32*)(val))[0]);
		break;

		default:
		break;
	}

}

static struct proc_dir_entry *rtl8192_proc = NULL;

static int proc_get_stats_ap(char *page, char **start,
			  off_t offset, int count,
			  int *eof, void *data)
{
	struct r8192_priv *priv = data;
	struct ieee80211_device *ieee = priv->ieee80211;
	struct ieee80211_network *target;
	int len = 0;

        list_for_each_entry(target, &ieee->network_list, list) {

		len += snprintf(page + len, count - len,
                "%s ", target->ssid);

		if(target->wpa_ie_len>0 || target->rsn_ie_len>0){
	                len += snprintf(page + len, count - len,
        	        "WPA\n");
		}
		else{
                        len += snprintf(page + len, count - len,
                        "non_WPA\n");
                }

        }

	*eof = 1;
	return len;
}

static int proc_get_registers(char *page, char **start,
			  off_t offset, int count,
			  int *eof, void *data)
{
	struct r8192_priv *priv = data;
	int len = 0;
	int i,n;
	int max=0xff;

	/* This dump the current register page */
	len += snprintf(page + len, count - len,
                        "\n####################page 0##################\n ");

	for(n=0;n<=max;)
	{
		len += snprintf(page + len, count - len,
			"\nD:  %2x > ",n);

		for(i=0;i<16 && n<=max;i++,n++)
		len += snprintf(page + len, count - len,
			"%2x ",read_nic_byte(priv,n));
	}
	len += snprintf(page + len, count - len,"\n");
	len += snprintf(page + len, count - len,
                        "\n####################page 1##################\n ");
        for(n=0;n<=max;)
        {
                len += snprintf(page + len, count - len,
                        "\nD:  %2x > ",n);

                for(i=0;i<16 && n<=max;i++,n++)
                len += snprintf(page + len, count - len,
                        "%2x ",read_nic_byte(priv,0x100|n));
        }

	len += snprintf(page + len, count - len,
                        "\n####################page 3##################\n ");
        for(n=0;n<=max;)
        {
                len += snprintf(page + len, count - len,
                        "\nD:  %2x > ",n);

                for(i=0;i<16 && n<=max;i++,n++)
                len += snprintf(page + len, count - len,
                        "%2x ",read_nic_byte(priv,0x300|n));
        }

	*eof = 1;
	return len;

}

static int proc_get_stats_tx(char *page, char **start,
			  off_t offset, int count,
			  int *eof, void *data)
{
	struct r8192_priv *priv = data;

	int len = 0;

	len += snprintf(page + len, count - len,
		"TX VI priority ok int: %lu\n"
		"TX VO priority ok int: %lu\n"
		"TX BE priority ok int: %lu\n"
		"TX BK priority ok int: %lu\n"
		"TX MANAGE priority ok int: %lu\n"
		"TX BEACON priority ok int: %lu\n"
		"TX BEACON priority error int: %lu\n"
		"TX CMDPKT priority ok int: %lu\n"
		"TX queue stopped?: %d\n"
		"TX fifo overflow: %lu\n"
		"TX total data packets %lu\n"
		"TX total data bytes :%lu\n",
		priv->stats.txviokint,
		priv->stats.txvookint,
		priv->stats.txbeokint,
		priv->stats.txbkokint,
		priv->stats.txmanageokint,
		priv->stats.txbeaconokint,
		priv->stats.txbeaconerr,
		priv->stats.txcmdpktokint,
		netif_queue_stopped(priv->ieee80211->dev),
		priv->stats.txoverflow,
		priv->ieee80211->stats.tx_packets,
		priv->ieee80211->stats.tx_bytes);

	*eof = 1;
	return len;
}

static int proc_get_stats_rx(char *page, char **start,
			  off_t offset, int count,
			  int *eof, void *data)
{
	struct r8192_priv *priv = data;
	int len = 0;

	len += snprintf(page + len, count - len,
		"RX packets: %lu\n"
		"RX desc err: %lu\n"
		"RX rx overflow error: %lu\n",
		priv->stats.rxint,
		priv->stats.rxrdu,
		priv->stats.rxoverflow);

	*eof = 1;
	return len;
}

static void rtl8192_proc_module_init(void)
{
	RT_TRACE(COMP_INIT, "Initializing proc filesystem\n");
	rtl8192_proc=create_proc_entry(RTL819xE_MODULE_NAME, S_IFDIR, init_net.proc_net);
}

static void rtl8192_proc_module_remove(void)
{
	remove_proc_entry(RTL819xE_MODULE_NAME, init_net.proc_net);
}

static void rtl8192_proc_remove_one(struct r8192_priv *priv)
{
	struct net_device *dev = priv->ieee80211->dev;

	printk("dev name=======> %s\n",dev->name);

	if (priv->dir_dev) {
		remove_proc_entry("stats-tx", priv->dir_dev);
		remove_proc_entry("stats-rx", priv->dir_dev);
		remove_proc_entry("stats-ap", priv->dir_dev);
		remove_proc_entry("registers", priv->dir_dev);
		remove_proc_entry("wlan0", rtl8192_proc);
		priv->dir_dev = NULL;
	}
}

static void rtl8192_proc_init_one(struct r8192_priv *priv)
{
	struct net_device *dev = priv->ieee80211->dev;
	struct proc_dir_entry *e;

	priv->dir_dev = create_proc_entry(dev->name,
					  S_IFDIR | S_IRUGO | S_IXUGO,
					  rtl8192_proc);
	if (!priv->dir_dev) {
		RT_TRACE(COMP_ERR, "Unable to initialize /proc/net/rtl8192/%s\n",
		      dev->name);
		return;
	}
	e = create_proc_read_entry("stats-rx", S_IFREG | S_IRUGO,
				   priv->dir_dev, proc_get_stats_rx, priv);

	if (!e) {
		RT_TRACE(COMP_ERR,"Unable to initialize "
		      "/proc/net/rtl8192/%s/stats-rx\n",
		      dev->name);
	}

	e = create_proc_read_entry("stats-tx", S_IFREG | S_IRUGO,
				   priv->dir_dev, proc_get_stats_tx, priv);

	if (!e) {
		RT_TRACE(COMP_ERR, "Unable to initialize "
		      "/proc/net/rtl8192/%s/stats-tx\n",
		      dev->name);
	}

	e = create_proc_read_entry("stats-ap", S_IFREG | S_IRUGO,
				   priv->dir_dev, proc_get_stats_ap, priv);

	if (!e) {
		RT_TRACE(COMP_ERR, "Unable to initialize "
		      "/proc/net/rtl8192/%s/stats-ap\n",
		      dev->name);
	}

	e = create_proc_read_entry("registers", S_IFREG | S_IRUGO,
				   priv->dir_dev, proc_get_registers, priv);
	if (!e) {
		RT_TRACE(COMP_ERR, "Unable to initialize "
		      "/proc/net/rtl8192/%s/registers\n",
		      dev->name);
	}
}

static short check_nic_enough_desc(struct ieee80211_device *ieee, int prio)
{
    struct r8192_priv *priv = ieee80211_priv(ieee->dev);
    struct rtl8192_tx_ring *ring = &priv->tx_ring[prio];

    /* for now we reserve two free descriptor as a safety boundary
     * between the tail and the head
     */
    return (ring->entries - skb_queue_len(&ring->queue) >= 2);
}

static void tx_timeout(struct net_device *dev)
{
	struct r8192_priv *priv = ieee80211_priv(dev);

	schedule_work(&priv->reset_wq);
	printk("TXTIMEOUT");
}

static void rtl8192_irq_enable(struct r8192_priv *priv)
{
	u32 mask;

	mask = IMR_ROK | IMR_VODOK | IMR_VIDOK | IMR_BEDOK | IMR_BKDOK |
	       IMR_HCCADOK | IMR_MGNTDOK | IMR_COMDOK | IMR_HIGHDOK |
	       IMR_BDOK | IMR_RXCMDOK | IMR_TIMEOUT0 | IMR_RDU | IMR_RXFOVW |
	       IMR_TXFOVW | IMR_BcnInt | IMR_TBDOK | IMR_TBDER;

	write_nic_dword(priv, INTA_MASK, mask);
}

static void rtl8192_irq_disable(struct r8192_priv *priv)
{
	write_nic_dword(priv, INTA_MASK, 0);
	synchronize_irq(priv->irq);
}

static void rtl8192_update_msr(struct r8192_priv *priv)
{
	u8 msr;

	msr  = read_nic_byte(priv, MSR);
	msr &= ~ MSR_LINK_MASK;

	/* do not change in link_state != WLAN_LINK_ASSOCIATED.
	 * msr must be updated if the state is ASSOCIATING.
	 * this is intentional and make sense for ad-hoc and
	 * master (see the create BSS/IBSS func)
	 */
	if (priv->ieee80211->state == IEEE80211_LINKED){

		if (priv->ieee80211->iw_mode == IW_MODE_INFRA)
			msr |= (MSR_LINK_MANAGED<<MSR_LINK_SHIFT);
		else if (priv->ieee80211->iw_mode == IW_MODE_ADHOC)
			msr |= (MSR_LINK_ADHOC<<MSR_LINK_SHIFT);
		else if (priv->ieee80211->iw_mode == IW_MODE_MASTER)
			msr |= (MSR_LINK_MASTER<<MSR_LINK_SHIFT);

	}else
		msr |= (MSR_LINK_NONE<<MSR_LINK_SHIFT);

	write_nic_byte(priv, MSR, msr);
}

static void rtl8192_set_chan(struct ieee80211_device *ieee80211, short ch)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);

	priv->chan = ch;

	/* need to implement rf set channel here WB */

	if (priv->rf_set_chan)
		priv->rf_set_chan(ieee80211, priv->chan);
}

static void rtl8192_rx_enable(struct r8192_priv *priv)
{
	write_nic_dword(priv, RDQDA, priv->rx_ring_dma);
}

/* the TX_DESC_BASE setting is according to the following queue index
 *  BK_QUEUE       ===>                        0
 *  BE_QUEUE       ===>                        1
 *  VI_QUEUE       ===>                        2
 *  VO_QUEUE       ===>                        3
 *  HCCA_QUEUE     ===>                        4
 *  TXCMD_QUEUE    ===>                        5
 *  MGNT_QUEUE     ===>                        6
 *  HIGH_QUEUE     ===>                        7
 *  BEACON_QUEUE   ===>                        8
 *  */
static const u32 TX_DESC_BASE[] = {BKQDA, BEQDA, VIQDA, VOQDA, HCCAQDA, CQDA, MQDA, HQDA, BQDA};
static void rtl8192_tx_enable(struct r8192_priv *priv)
{
	u32 i;

	for (i = 0; i < MAX_TX_QUEUE_COUNT; i++)
		write_nic_dword(priv, TX_DESC_BASE[i], priv->tx_ring[i].dma);

	ieee80211_reset_queue(priv->ieee80211);
}

static void rtl8192_free_rx_ring(struct r8192_priv *priv)
{
	int i;

	for (i = 0; i < priv->rxringcount; i++) {
		struct sk_buff *skb = priv->rx_buf[i];
		if (!skb)
			continue;

		pci_unmap_single(priv->pdev,
				 *((dma_addr_t *)skb->cb),
				 priv->rxbuffersize, PCI_DMA_FROMDEVICE);
		kfree_skb(skb);
	}

	pci_free_consistent(priv->pdev, sizeof(*priv->rx_ring) * priv->rxringcount,
			    priv->rx_ring, priv->rx_ring_dma);
	priv->rx_ring = NULL;
}

static void rtl8192_free_tx_ring(struct r8192_priv *priv, unsigned int prio)
{
	struct rtl8192_tx_ring *ring = &priv->tx_ring[prio];

	while (skb_queue_len(&ring->queue)) {
		tx_desc_819x_pci *entry = &ring->desc[ring->idx];
		struct sk_buff *skb = __skb_dequeue(&ring->queue);

		pci_unmap_single(priv->pdev, le32_to_cpu(entry->TxBuffAddr),
				 skb->len, PCI_DMA_TODEVICE);
		kfree_skb(skb);
		ring->idx = (ring->idx + 1) % ring->entries;
	}

	pci_free_consistent(priv->pdev, sizeof(*ring->desc)*ring->entries,
			    ring->desc, ring->dma);
	ring->desc = NULL;
}

void PHY_SetRtl8192eRfOff(struct r8192_priv *priv)
{
	//disable RF-Chip A/B
	rtl8192_setBBreg(priv, rFPGA0_XA_RFInterfaceOE, BIT4, 0x0);
	//analog to digital off, for power save
	rtl8192_setBBreg(priv, rFPGA0_AnalogParameter4, 0x300, 0x0);
	//digital to analog off, for power save
	rtl8192_setBBreg(priv, rFPGA0_AnalogParameter1, 0x18, 0x0);
	//rx antenna off
	rtl8192_setBBreg(priv, rOFDM0_TRxPathEnable, 0xf, 0x0);
	//rx antenna off
	rtl8192_setBBreg(priv, rOFDM1_TRxPathEnable, 0xf, 0x0);
	//analog to digital part2 off, for power save
	rtl8192_setBBreg(priv, rFPGA0_AnalogParameter1, 0x60, 0x0);
	rtl8192_setBBreg(priv, rFPGA0_AnalogParameter1, 0x4, 0x0);
	// Analog parameter!!Change bias and Lbus control.
	write_nic_byte(priv, ANAPAR_FOR_8192PciE, 0x07);
}

static void rtl8192_halt_adapter(struct r8192_priv *priv, bool reset)
{
	int i;
	u8 OpMode;
	u32 ulRegRead;

	OpMode = RT_OP_MODE_NO_LINK;
	priv->ieee80211->SetHwRegHandler(priv->ieee80211, HW_VAR_MEDIA_STATUS, &OpMode);

	if (!priv->ieee80211->bSupportRemoteWakeUp) {
		/*
		 * disable tx/rx. In 8185 we write 0x10 (Reset bit),
		 * but here we make reference to WMAC and wirte 0x0
		 */
		write_nic_byte(priv, CMDR, 0);
	}

	mdelay(20);

	if (!reset) {
		mdelay(150);

		priv->bHwRfOffAction = 2;

		/*
		 * Call MgntActSet_RF_State instead to
		 * prevent RF config race condition.
		 */
		if (!priv->ieee80211->bSupportRemoteWakeUp) {
			PHY_SetRtl8192eRfOff(priv);
			ulRegRead = read_nic_dword(priv, CPU_GEN);
			ulRegRead |= CPU_GEN_SYSTEM_RESET;
			write_nic_dword(priv,CPU_GEN, ulRegRead);
		} else {
			/* for WOL */
			write_nic_dword(priv, WFCRC0, 0xffffffff);
			write_nic_dword(priv, WFCRC1, 0xffffffff);
			write_nic_dword(priv, WFCRC2, 0xffffffff);

			/* Write PMR register */
			write_nic_byte(priv, PMR, 0x5);
			/* Disable tx, enanble rx */
			write_nic_byte(priv, MacBlkCtrl, 0xa);
		}
	}

	for(i = 0; i < MAX_QUEUE_SIZE; i++) {
		skb_queue_purge(&priv->ieee80211->skb_waitQ [i]);
	}
	for(i = 0; i < MAX_QUEUE_SIZE; i++) {
		skb_queue_purge(&priv->ieee80211->skb_aggQ [i]);
	}

	skb_queue_purge(&priv->skb_queue);
}

static void rtl8192_data_hard_stop(struct ieee80211_device *ieee80211)
{
}

static void rtl8192_data_hard_resume(struct ieee80211_device *ieee80211)
{
}

/*
 * this function TX data frames when the ieee80211 stack requires this.
 * It checks also if we need to stop the ieee tx queue, eventually do it
 */
static void rtl8192_hard_data_xmit(struct sk_buff *skb,
				   struct ieee80211_device *ieee80211, int rate)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	int ret;
	cb_desc *tcb_desc = (cb_desc *)(skb->cb + MAX_DEV_ADDR_SIZE);
	u8 queue_index = tcb_desc->queue_index;

	/* shall not be referred by command packet */
	BUG_ON(queue_index == TXCMD_QUEUE);

	if (priv->bHwRadioOff || (!priv->up))
	{
		kfree_skb(skb);
		return;
	}

	skb_push(skb, priv->ieee80211->tx_headroom);
	ret = rtl8192_tx(priv, skb);
	if (ret != 0) {
		kfree_skb(skb);
	}

	if (queue_index != MGNT_QUEUE) {
		priv->ieee80211->stats.tx_bytes += (skb->len - priv->ieee80211->tx_headroom);
		priv->ieee80211->stats.tx_packets++;
	}
}

/*
 * This is a rough attempt to TX a frame
 * This is called by the ieee 80211 stack to TX management frames.
 * If the ring is full packet are dropped (for data frame the queue
 * is stopped before this can happen).
 */
static int rtl8192_hard_start_xmit(struct sk_buff *skb, struct ieee80211_device *ieee80211)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	int ret;
        cb_desc *tcb_desc = (cb_desc *)(skb->cb + MAX_DEV_ADDR_SIZE);
        u8 queue_index = tcb_desc->queue_index;

        if (queue_index != TXCMD_QUEUE) {
		if (priv->bHwRadioOff || (!priv->up))
		{
			kfree_skb(skb);
			return 0;
		}
        }

	if (queue_index == TXCMD_QUEUE) {
		rtl819xE_tx_cmd(priv, skb);
		ret = 0;
		return ret;
	} else {
		tcb_desc->RATRIndex = 7;
		tcb_desc->bTxDisableRateFallBack = 1;
		tcb_desc->bTxUseDriverAssingedRate = 1;
		tcb_desc->bTxEnableFwCalcDur = 1;
		skb_push(skb, ieee80211->tx_headroom);
		ret = rtl8192_tx(priv, skb);
		if (ret != 0) {
			kfree_skb(skb);
		}
	}

	return ret;
}

static void rtl8192_tx_isr(struct r8192_priv *priv, int prio)
{
	struct rtl8192_tx_ring *ring = &priv->tx_ring[prio];

	while (skb_queue_len(&ring->queue)) {
		tx_desc_819x_pci *entry = &ring->desc[ring->idx];
		struct sk_buff *skb;

		/*
		 * beacon packet will only use the first descriptor defaultly,
		 * and the OWN may not be cleared by the hardware
		 */
		if (prio != BEACON_QUEUE) {
			if (entry->OWN)
				return;
			ring->idx = (ring->idx + 1) % ring->entries;
		}

		skb = __skb_dequeue(&ring->queue);
		pci_unmap_single(priv->pdev, le32_to_cpu(entry->TxBuffAddr),
				 skb->len, PCI_DMA_TODEVICE);

		kfree_skb(skb);
	}

	if (prio != BEACON_QUEUE) {
		/* try to deal with the pending packets  */
		tasklet_schedule(&priv->irq_tx_tasklet);
	}
}

static void rtl8192_stop_beacon(struct ieee80211_device *ieee80211)
{
}

static void rtl8192_config_rate(struct r8192_priv *priv, u16* rate_config)
{
	 struct ieee80211_network *net;
	 u8 i=0, basic_rate = 0;
	 net = & priv->ieee80211->current_network;

	 for (i=0; i<net->rates_len; i++)
	 {
		 basic_rate = net->rates[i]&0x7f;
		 switch(basic_rate)
		 {
			 case MGN_1M:	*rate_config |= RRSR_1M;	break;
			 case MGN_2M:	*rate_config |= RRSR_2M;	break;
			 case MGN_5_5M:	*rate_config |= RRSR_5_5M;	break;
			 case MGN_11M:	*rate_config |= RRSR_11M;	break;
			 case MGN_6M:	*rate_config |= RRSR_6M;	break;
			 case MGN_9M:	*rate_config |= RRSR_9M;	break;
			 case MGN_12M:	*rate_config |= RRSR_12M;	break;
			 case MGN_18M:	*rate_config |= RRSR_18M;	break;
			 case MGN_24M:	*rate_config |= RRSR_24M;	break;
			 case MGN_36M:	*rate_config |= RRSR_36M;	break;
			 case MGN_48M:	*rate_config |= RRSR_48M;	break;
			 case MGN_54M:	*rate_config |= RRSR_54M;	break;
		 }
	 }
	 for (i=0; i<net->rates_ex_len; i++)
	 {
		 basic_rate = net->rates_ex[i]&0x7f;
		 switch(basic_rate)
		 {
			 case MGN_1M:	*rate_config |= RRSR_1M;	break;
			 case MGN_2M:	*rate_config |= RRSR_2M;	break;
			 case MGN_5_5M:	*rate_config |= RRSR_5_5M;	break;
			 case MGN_11M:	*rate_config |= RRSR_11M;	break;
			 case MGN_6M:	*rate_config |= RRSR_6M;	break;
			 case MGN_9M:	*rate_config |= RRSR_9M;	break;
			 case MGN_12M:	*rate_config |= RRSR_12M;	break;
			 case MGN_18M:	*rate_config |= RRSR_18M;	break;
			 case MGN_24M:	*rate_config |= RRSR_24M;	break;
			 case MGN_36M:	*rate_config |= RRSR_36M;	break;
			 case MGN_48M:	*rate_config |= RRSR_48M;	break;
			 case MGN_54M:	*rate_config |= RRSR_54M;	break;
		 }
	 }
}

#define SHORT_SLOT_TIME 9
#define NON_SHORT_SLOT_TIME 20

static void rtl8192_update_cap(struct r8192_priv *priv, u16 cap)
{
	u32 tmp = 0;
	struct ieee80211_network *net = &priv->ieee80211->current_network;

	priv->short_preamble = cap & WLAN_CAPABILITY_SHORT_PREAMBLE;
	tmp = priv->basic_rate;
	if (priv->short_preamble)
		tmp |= BRSR_AckShortPmb;
	write_nic_dword(priv, RRSR, tmp);

	if (net->mode & (IEEE_G|IEEE_N_24G))
	{
		u8 slot_time = 0;
		if ((cap & WLAN_CAPABILITY_SHORT_SLOT)&&(!priv->ieee80211->pHTInfo->bCurrentRT2RTLongSlotTime))
		{//short slot time
			slot_time = SHORT_SLOT_TIME;
		}
		else //long slot time
			slot_time = NON_SHORT_SLOT_TIME;
		priv->slot_time = slot_time;
		write_nic_byte(priv, SLOT_TIME, slot_time);
	}

}

static void rtl8192_net_update(struct r8192_priv *priv)
{
	struct ieee80211_network *net;
	u16 BcnTimeCfg = 0, BcnCW = 6, BcnIFS = 0xf;
	u16 rate_config = 0;
	net = &priv->ieee80211->current_network;

	/* update Basic rate: RR, BRSR */
	rtl8192_config_rate(priv, &rate_config);

	/*
	 * Select RRSR (in Legacy-OFDM and CCK)
	 * For 8190, we select only 24M, 12M, 6M, 11M, 5.5M,
	 * 2M, and 1M from the Basic rate.
	 * We do not use other rates.
	 */
	priv->basic_rate = rate_config &= 0x15f;

	/* BSSID */
	write_nic_dword(priv, BSSIDR, ((u32 *)net->bssid)[0]);
	write_nic_word(priv, BSSIDR+4, ((u16 *)net->bssid)[2]);

	if (priv->ieee80211->iw_mode == IW_MODE_ADHOC)
	{
		write_nic_word(priv, ATIMWND, 2);
		write_nic_word(priv, BCN_DMATIME, 256);
		write_nic_word(priv, BCN_INTERVAL, net->beacon_interval);
		/*
		 * BIT15 of BCN_DRV_EARLY_INT will indicate
		 * whether software beacon or hw beacon is applied.
		 */
		write_nic_word(priv, BCN_DRV_EARLY_INT, 10);
		write_nic_byte(priv, BCN_ERR_THRESH, 100);

		BcnTimeCfg |= (BcnCW<<BCN_TCFG_CW_SHIFT);
		/* TODO: BcnIFS may required to be changed on ASIC */
		BcnTimeCfg |= BcnIFS<<BCN_TCFG_IFS;
		write_nic_word(priv, BCN_TCFG, BcnTimeCfg);
	}
}

static void rtl819xE_tx_cmd(struct r8192_priv *priv, struct sk_buff *skb)
{
    struct rtl8192_tx_ring *ring;
    tx_desc_819x_pci *entry;
    unsigned int idx;
    dma_addr_t mapping;
    cb_desc *tcb_desc;
    unsigned long flags;

    ring = &priv->tx_ring[TXCMD_QUEUE];
    mapping = pci_map_single(priv->pdev, skb->data, skb->len, PCI_DMA_TODEVICE);

    spin_lock_irqsave(&priv->irq_th_lock,flags);
    idx = (ring->idx + skb_queue_len(&ring->queue)) % ring->entries;
    entry = &ring->desc[idx];

    tcb_desc = (cb_desc *)(skb->cb + MAX_DEV_ADDR_SIZE);
    memset(entry,0,12);
    entry->LINIP = tcb_desc->bLastIniPkt;
    entry->FirstSeg = 1;//first segment
    entry->LastSeg = 1; //last segment
    if(tcb_desc->bCmdOrInit == DESC_PACKET_TYPE_INIT) {
        entry->CmdInit = DESC_PACKET_TYPE_INIT;
    } else {
        entry->CmdInit = DESC_PACKET_TYPE_NORMAL;
        entry->Offset = sizeof(TX_FWINFO_8190PCI) + 8;
        entry->PktSize = (u16)(tcb_desc->pkt_size + entry->Offset);
        entry->QueueSelect = QSLT_CMD;
        entry->TxFWInfoSize = 0x08;
        entry->RATid = (u8)DESC_PACKET_TYPE_INIT;
    }
    entry->TxBufferSize = skb->len;
    entry->TxBuffAddr = cpu_to_le32(mapping);
    entry->OWN = 1;

    __skb_queue_tail(&ring->queue, skb);
    spin_unlock_irqrestore(&priv->irq_th_lock,flags);

    write_nic_byte(priv, TPPoll, TPPoll_CQ);

    return;
}

/*
 * Mapping Software/Hardware descriptor queue id to "Queue Select Field"
 * in TxFwInfo data structure
 */
static u8 MapHwQueueToFirmwareQueue(u8 QueueID)
{
	u8 QueueSelect = 0;

	switch (QueueID) {
	case BE_QUEUE:
		QueueSelect = QSLT_BE;
		break;

	case BK_QUEUE:
		QueueSelect = QSLT_BK;
		break;

	case VO_QUEUE:
		QueueSelect = QSLT_VO;
		break;

	case VI_QUEUE:
		QueueSelect = QSLT_VI;
		break;

	case MGNT_QUEUE:
		QueueSelect = QSLT_MGNT;
		break;

	case BEACON_QUEUE:
		QueueSelect = QSLT_BEACON;
		break;

	case TXCMD_QUEUE:
		QueueSelect = QSLT_CMD;
		break;

	case HIGH_QUEUE:
	default:
		RT_TRACE(COMP_ERR, "Impossible Queue Selection: %d\n", QueueID);
		break;
	}
	return QueueSelect;
}

static u8 MRateToHwRate8190Pci(u8 rate)
{
	u8  ret = DESC90_RATE1M;

	switch(rate) {
		case MGN_1M:	ret = DESC90_RATE1M;		break;
		case MGN_2M:	ret = DESC90_RATE2M;		break;
		case MGN_5_5M:	ret = DESC90_RATE5_5M;	break;
		case MGN_11M:	ret = DESC90_RATE11M;	break;
		case MGN_6M:	ret = DESC90_RATE6M;		break;
		case MGN_9M:	ret = DESC90_RATE9M;		break;
		case MGN_12M:	ret = DESC90_RATE12M;	break;
		case MGN_18M:	ret = DESC90_RATE18M;	break;
		case MGN_24M:	ret = DESC90_RATE24M;	break;
		case MGN_36M:	ret = DESC90_RATE36M;	break;
		case MGN_48M:	ret = DESC90_RATE48M;	break;
		case MGN_54M:	ret = DESC90_RATE54M;	break;

		// HT rate since here
		case MGN_MCS0:	ret = DESC90_RATEMCS0;	break;
		case MGN_MCS1:	ret = DESC90_RATEMCS1;	break;
		case MGN_MCS2:	ret = DESC90_RATEMCS2;	break;
		case MGN_MCS3:	ret = DESC90_RATEMCS3;	break;
		case MGN_MCS4:	ret = DESC90_RATEMCS4;	break;
		case MGN_MCS5:	ret = DESC90_RATEMCS5;	break;
		case MGN_MCS6:	ret = DESC90_RATEMCS6;	break;
		case MGN_MCS7:	ret = DESC90_RATEMCS7;	break;
		case MGN_MCS8:	ret = DESC90_RATEMCS8;	break;
		case MGN_MCS9:	ret = DESC90_RATEMCS9;	break;
		case MGN_MCS10:	ret = DESC90_RATEMCS10;	break;
		case MGN_MCS11:	ret = DESC90_RATEMCS11;	break;
		case MGN_MCS12:	ret = DESC90_RATEMCS12;	break;
		case MGN_MCS13:	ret = DESC90_RATEMCS13;	break;
		case MGN_MCS14:	ret = DESC90_RATEMCS14;	break;
		case MGN_MCS15:	ret = DESC90_RATEMCS15;	break;
		case (0x80|0x20): ret = DESC90_RATEMCS32; break;

		default:       break;
	}
	return ret;
}

static u8 QueryIsShort(u8 TxHT, u8 TxRate, cb_desc *tcb_desc)
{
	u8   tmp_Short;

	tmp_Short = (TxHT==1)?((tcb_desc->bUseShortGI)?1:0):((tcb_desc->bUseShortPreamble)?1:0);

	if(TxHT==1 && TxRate != DESC90_RATEMCS15)
		tmp_Short = 0;

	return tmp_Short;
}

/*
 * The tx procedure is just as following,
 * skb->cb will contain all the following information,
 * priority, morefrag, rate, &dev.
 */
static short rtl8192_tx(struct r8192_priv *priv, struct sk_buff* skb)
{
	struct rtl8192_tx_ring *ring;
	unsigned long flags;
	cb_desc *tcb_desc = (cb_desc *)(skb->cb + MAX_DEV_ADDR_SIZE);
	tx_desc_819x_pci *pdesc = NULL;
	TX_FWINFO_8190PCI *pTxFwInfo = NULL;
	dma_addr_t mapping;
	bool multi_addr = false, broad_addr = false, uni_addr = false;
	u8 *pda_addr = NULL;
	int idx;

	if (priv->bdisable_nic) {
		RT_TRACE(COMP_ERR, "Nic is disabled! Can't tx packet len=%d qidx=%d!!!\n",
			 skb->len, tcb_desc->queue_index);
		return skb->len;
	}

#ifdef ENABLE_LPS
	priv->ieee80211->bAwakePktSent = true;
#endif

	mapping = pci_map_single(priv->pdev, skb->data, skb->len, PCI_DMA_TODEVICE);

	/* collect the tx packets statitcs */
	pda_addr = ((u8 *)skb->data) + sizeof(TX_FWINFO_8190PCI);
	if (is_multicast_ether_addr(pda_addr))
		multi_addr = true;
	else if (is_broadcast_ether_addr(pda_addr))
		broad_addr = true;
	else
		uni_addr = true;

	if (uni_addr)
		priv->stats.txbytesunicast += (u8)(skb->len) - sizeof(TX_FWINFO_8190PCI);

	/* fill tx firmware */
	pTxFwInfo = (PTX_FWINFO_8190PCI)skb->data;
	memset(pTxFwInfo, 0, sizeof(TX_FWINFO_8190PCI));
	pTxFwInfo->TxHT = (tcb_desc->data_rate&0x80) ? 1 : 0;
	pTxFwInfo->TxRate = MRateToHwRate8190Pci((u8)tcb_desc->data_rate);
	pTxFwInfo->EnableCPUDur = tcb_desc->bTxEnableFwCalcDur;
	pTxFwInfo->Short = QueryIsShort(pTxFwInfo->TxHT, pTxFwInfo->TxRate, tcb_desc);

	/* Aggregation related */
	if (tcb_desc->bAMPDUEnable) {
		pTxFwInfo->AllowAggregation = 1;
		pTxFwInfo->RxMF = tcb_desc->ampdu_factor;
		pTxFwInfo->RxAMD = tcb_desc->ampdu_density;
	} else {
		pTxFwInfo->AllowAggregation = 0;
		pTxFwInfo->RxMF = 0;
		pTxFwInfo->RxAMD = 0;
	}

	/* Protection mode related */
	pTxFwInfo->RtsEnable = (tcb_desc->bRTSEnable) ? 1 : 0;
	pTxFwInfo->CtsEnable = (tcb_desc->bCTSEnable) ? 1 : 0;
	pTxFwInfo->RtsSTBC = (tcb_desc->bRTSSTBC) ? 1 : 0;
	pTxFwInfo->RtsHT = (tcb_desc->rts_rate&0x80) ? 1 : 0;
	pTxFwInfo->RtsRate = MRateToHwRate8190Pci((u8)tcb_desc->rts_rate);
	pTxFwInfo->RtsBandwidth = 0;
	pTxFwInfo->RtsSubcarrier = tcb_desc->RTSSC;
	pTxFwInfo->RtsShort = (pTxFwInfo->RtsHT == 0) ? (tcb_desc->bRTSUseShortPreamble ? 1 : 0) : (tcb_desc->bRTSUseShortGI? 1 : 0);

	/* Set Bandwidth and sub-channel settings. */
	if (priv->CurrentChannelBW == HT_CHANNEL_WIDTH_20_40) {
		if (tcb_desc->bPacketBW) {
			pTxFwInfo->TxBandwidth = 1;
			/* use duplicated mode */
			pTxFwInfo->TxSubCarrier = 0;
		} else {
			pTxFwInfo->TxBandwidth = 0;
			pTxFwInfo->TxSubCarrier = priv->nCur40MhzPrimeSC;
		}
	} else {
		pTxFwInfo->TxBandwidth = 0;
		pTxFwInfo->TxSubCarrier = 0;
	}

	spin_lock_irqsave(&priv->irq_th_lock, flags);
	ring = &priv->tx_ring[tcb_desc->queue_index];
	if (tcb_desc->queue_index != BEACON_QUEUE)
		idx = (ring->idx + skb_queue_len(&ring->queue)) % ring->entries;
	else
		idx = 0;

	pdesc = &ring->desc[idx];
	if ((pdesc->OWN == 1) && (tcb_desc->queue_index != BEACON_QUEUE)) {
		RT_TRACE(COMP_ERR, "No more TX desc@%d, ring->idx = %d,idx = %d,%x\n",
			 tcb_desc->queue_index, ring->idx, idx, skb->len);
		spin_unlock_irqrestore(&priv->irq_th_lock, flags);
		return skb->len;
	}

	/* fill tx descriptor */
	memset(pdesc, 0, 12);

	/*DWORD 0*/
	pdesc->LINIP = 0;
	pdesc->CmdInit = 1;
	pdesc->Offset = sizeof(TX_FWINFO_8190PCI) + 8; /* We must add 8!! */
	pdesc->PktSize = (u16)skb->len-sizeof(TX_FWINFO_8190PCI);

	/*DWORD 1*/
	pdesc->SecCAMID = 0;
	pdesc->RATid = tcb_desc->RATRIndex;

	pdesc->NoEnc = 1;
	pdesc->SecType = 0x0;
	if (tcb_desc->bHwSec) {
		switch (priv->ieee80211->pairwise_key_type) {
		case KEY_TYPE_WEP40:
		case KEY_TYPE_WEP104:
			pdesc->SecType = 0x1;
			pdesc->NoEnc = 0;
			break;
		case KEY_TYPE_TKIP:
			pdesc->SecType = 0x2;
			pdesc->NoEnc = 0;
			break;
		case KEY_TYPE_CCMP:
			pdesc->SecType = 0x3;
			pdesc->NoEnc = 0;
			break;
		case KEY_TYPE_NA:
			pdesc->SecType = 0x0;
			pdesc->NoEnc = 1;
			break;
		}
	}

	/* Set Packet ID */
	pdesc->PktId = 0x0;

	pdesc->QueueSelect = MapHwQueueToFirmwareQueue(tcb_desc->queue_index);
	pdesc->TxFWInfoSize = sizeof(TX_FWINFO_8190PCI);

	pdesc->DISFB = tcb_desc->bTxDisableRateFallBack;
	pdesc->USERATE = tcb_desc->bTxUseDriverAssingedRate;

	pdesc->FirstSeg = 1;
	pdesc->LastSeg = 1;
	pdesc->TxBufferSize = skb->len;

	pdesc->TxBuffAddr = cpu_to_le32(mapping);
	__skb_queue_tail(&ring->queue, skb);
	pdesc->OWN = 1;
	spin_unlock_irqrestore(&priv->irq_th_lock, flags);
	priv->ieee80211->dev->trans_start = jiffies;
	write_nic_word(priv, TPPoll, 0x01<<tcb_desc->queue_index);
	return 0;
}

static short rtl8192_alloc_rx_desc_ring(struct r8192_priv *priv)
{
    rx_desc_819x_pci *entry = NULL;
    int i;

    priv->rx_ring = pci_alloc_consistent(priv->pdev,
            sizeof(*priv->rx_ring) * priv->rxringcount, &priv->rx_ring_dma);

    if (!priv->rx_ring || (unsigned long)priv->rx_ring & 0xFF) {
        RT_TRACE(COMP_ERR,"Cannot allocate RX ring\n");
        return -ENOMEM;
    }

    memset(priv->rx_ring, 0, sizeof(*priv->rx_ring) * priv->rxringcount);
    priv->rx_idx = 0;

    for (i = 0; i < priv->rxringcount; i++) {
        struct sk_buff *skb = dev_alloc_skb(priv->rxbuffersize);
        dma_addr_t *mapping;
        entry = &priv->rx_ring[i];
        if (!skb)
            return 0;
        priv->rx_buf[i] = skb;
        mapping = (dma_addr_t *)skb->cb;
        *mapping = pci_map_single(priv->pdev, skb_tail_pointer(skb),
                priv->rxbuffersize, PCI_DMA_FROMDEVICE);

        entry->BufferAddress = cpu_to_le32(*mapping);

        entry->Length = priv->rxbuffersize;
        entry->OWN = 1;
    }

    entry->EOR = 1;
    return 0;
}

static int rtl8192_alloc_tx_desc_ring(struct r8192_priv *priv,
        unsigned int prio, unsigned int entries)
{
    tx_desc_819x_pci *ring;
    dma_addr_t dma;
    int i;

    ring = pci_alloc_consistent(priv->pdev, sizeof(*ring) * entries, &dma);
    if (!ring || (unsigned long)ring & 0xFF) {
        RT_TRACE(COMP_ERR, "Cannot allocate TX ring (prio = %d)\n", prio);
        return -ENOMEM;
    }

    memset(ring, 0, sizeof(*ring)*entries);
    priv->tx_ring[prio].desc = ring;
    priv->tx_ring[prio].dma = dma;
    priv->tx_ring[prio].idx = 0;
    priv->tx_ring[prio].entries = entries;
    skb_queue_head_init(&priv->tx_ring[prio].queue);

    for (i = 0; i < entries; i++)
        ring[i].NextDescAddress =
            cpu_to_le32((u32)dma + ((i + 1) % entries) * sizeof(*ring));

    return 0;
}

static short rtl8192_pci_initdescring(struct r8192_priv *priv)
{
	u32 ret;
	int i;

	ret = rtl8192_alloc_rx_desc_ring(priv);
	if (ret)
		return ret;

	/* general process for other queue */
	for (i = 0; i < MAX_TX_QUEUE_COUNT; i++) {
		ret = rtl8192_alloc_tx_desc_ring(priv, i, priv->txringcount);
		if (ret)
			goto err_free_rings;
	}

	return 0;

err_free_rings:
	rtl8192_free_rx_ring(priv);
	for (i = 0; i < MAX_TX_QUEUE_COUNT; i++)
		if (priv->tx_ring[i].desc)
			rtl8192_free_tx_ring(priv, i);
	return 1;
}

static void rtl8192_pci_resetdescring(struct r8192_priv *priv)
{
    int i;

    /* force the rx_idx to the first one */
    if(priv->rx_ring) {
        rx_desc_819x_pci *entry = NULL;
        for (i = 0; i < priv->rxringcount; i++) {
            entry = &priv->rx_ring[i];
            entry->OWN = 1;
        }
        priv->rx_idx = 0;
    }

    /* after reset, release previous pending packet, and force the
     * tx idx to the first one */
    for (i = 0; i < MAX_TX_QUEUE_COUNT; i++) {
        if (priv->tx_ring[i].desc) {
            struct rtl8192_tx_ring *ring = &priv->tx_ring[i];

            while (skb_queue_len(&ring->queue)) {
                tx_desc_819x_pci *entry = &ring->desc[ring->idx];
                struct sk_buff *skb = __skb_dequeue(&ring->queue);

                pci_unmap_single(priv->pdev, le32_to_cpu(entry->TxBuffAddr),
                        skb->len, PCI_DMA_TODEVICE);
                kfree_skb(skb);
                ring->idx = (ring->idx + 1) % ring->entries;
            }
            ring->idx = 0;
        }
    }
}

static void rtl8192_link_change(struct ieee80211_device *ieee)
{
	struct r8192_priv *priv = ieee80211_priv(ieee->dev);

	if (ieee->state == IEEE80211_LINKED)
	{
		rtl8192_net_update(priv);
		rtl8192_update_ratr_table(priv);

		//add this as in pure N mode, wep encryption will use software way, but there is no chance to set this as wep will not set group key in wext. WB.2008.07.08
		if ((KEY_TYPE_WEP40 == ieee->pairwise_key_type) || (KEY_TYPE_WEP104 == ieee->pairwise_key_type))
		EnableHWSecurityConfig8192(priv);
	}
	else
	{
		write_nic_byte(priv, 0x173, 0);
	}

	rtl8192_update_msr(priv);

	// 2007/10/16 MH MAC Will update TSF according to all received beacon, so we have
	//	// To set CBSSID bit when link with any AP or STA.
	if (ieee->iw_mode == IW_MODE_INFRA || ieee->iw_mode == IW_MODE_ADHOC)
	{
		u32 reg = 0;
		reg = read_nic_dword(priv, RCR);
		if (priv->ieee80211->state == IEEE80211_LINKED)
			priv->ReceiveConfig = reg |= RCR_CBSSID;
		else
			priv->ReceiveConfig = reg &= ~RCR_CBSSID;
		write_nic_dword(priv, RCR, reg);
	}
}

static const struct ieee80211_qos_parameters def_qos_parameters = {
        {3,3,3,3},/* cw_min */
        {7,7,7,7},/* cw_max */
        {2,2,2,2},/* aifs */
        {0,0,0,0},/* flags */
        {0,0,0,0} /* tx_op_limit */
};

static void rtl8192_update_beacon(struct work_struct * work)
{
        struct r8192_priv *priv = container_of(work, struct r8192_priv, update_beacon_wq.work);
 	struct ieee80211_device* ieee = priv->ieee80211;
	struct ieee80211_network* net = &ieee->current_network;

	if (ieee->pHTInfo->bCurrentHTSupport)
		HTUpdateSelfAndPeerSetting(ieee, net);
	ieee->pHTInfo->bCurrentRT2RTLongSlotTime = net->bssht.bdRT2RTLongSlotTime;
	rtl8192_update_cap(priv, net->capability);
}

/*
* background support to run QoS activate functionality
*/
static const int WDCAPARA_ADD[] = {EDCAPARA_BE,EDCAPARA_BK,EDCAPARA_VI,EDCAPARA_VO};
static void rtl8192_qos_activate(struct work_struct * work)
{
        struct r8192_priv *priv = container_of(work, struct r8192_priv, qos_activate);
        struct ieee80211_qos_parameters *qos_parameters = &priv->ieee80211->current_network.qos_data.parameters;
        u8 mode = priv->ieee80211->current_network.mode;
	u8  u1bAIFS;
	u32 u4bAcParam;
        int i;

        mutex_lock(&priv->mutex);
        if(priv->ieee80211->state != IEEE80211_LINKED)
		goto success;
	RT_TRACE(COMP_QOS,"qos active process with associate response received\n");
	/* It better set slot time at first */
	/* For we just support b/g mode at present, let the slot time at 9/20 selection */
	/* update the ac parameter to related registers */
	for(i = 0; i <  QOS_QUEUE_NUM; i++) {
		//Mode G/A: slotTimeTimer = 9; Mode B: 20
		u1bAIFS = qos_parameters->aifs[i] * ((mode&(IEEE_G|IEEE_N_24G)) ?9:20) + aSifsTime;
		u4bAcParam = ((((u32)(qos_parameters->tx_op_limit[i]))<< AC_PARAM_TXOP_LIMIT_OFFSET)|
				(((u32)(qos_parameters->cw_max[i]))<< AC_PARAM_ECW_MAX_OFFSET)|
				(((u32)(qos_parameters->cw_min[i]))<< AC_PARAM_ECW_MIN_OFFSET)|
				((u32)u1bAIFS << AC_PARAM_AIFS_OFFSET));
		write_nic_dword(priv, WDCAPARA_ADD[i], u4bAcParam);
	}

success:
        mutex_unlock(&priv->mutex);
}

static int rtl8192_qos_handle_probe_response(struct r8192_priv *priv,
		int active_network,
		struct ieee80211_network *network)
{
	int ret = 0;
	u32 size = sizeof(struct ieee80211_qos_parameters);

	if(priv->ieee80211->state !=IEEE80211_LINKED)
                return ret;

        if ((priv->ieee80211->iw_mode != IW_MODE_INFRA))
                return ret;

	if (network->flags & NETWORK_HAS_QOS_MASK) {
		if (active_network &&
				(network->flags & NETWORK_HAS_QOS_PARAMETERS))
			network->qos_data.active = network->qos_data.supported;

		if ((network->qos_data.active == 1) && (active_network == 1) &&
				(network->flags & NETWORK_HAS_QOS_PARAMETERS) &&
				(network->qos_data.old_param_count !=
				 network->qos_data.param_count)) {
			network->qos_data.old_param_count =
				network->qos_data.param_count;
			queue_work(priv->priv_wq, &priv->qos_activate);
			RT_TRACE (COMP_QOS, "QoS parameters change call "
					"qos_activate\n");
		}
	} else {
		memcpy(&priv->ieee80211->current_network.qos_data.parameters,
		       &def_qos_parameters, size);

		if ((network->qos_data.active == 1) && (active_network == 1)) {
			queue_work(priv->priv_wq, &priv->qos_activate);
			RT_TRACE(COMP_QOS, "QoS was disabled call qos_activate\n");
		}
		network->qos_data.active = 0;
		network->qos_data.supported = 0;
	}

	return 0;
}

/* handle manage frame frame beacon and probe response */
static int rtl8192_handle_beacon(struct ieee80211_device *ieee,
                              struct ieee80211_beacon * beacon,
                              struct ieee80211_network * network)
{
	struct r8192_priv *priv = ieee80211_priv(ieee->dev);

	rtl8192_qos_handle_probe_response(priv,1,network);

	queue_delayed_work(priv->priv_wq, &priv->update_beacon_wq, 0);
	return 0;

}

/*
 * handling the beaconing responses. if we get different QoS setting
 * off the network from the associated setting, adjust the QoS setting
 */
static int rtl8192_qos_association_resp(struct r8192_priv *priv,
                                    struct ieee80211_network *network)
{
	int ret = 0;
	unsigned long flags;
	u32 size = sizeof(struct ieee80211_qos_parameters);
	int set_qos_param = 0;

	if ((priv == NULL) || (network == NULL))
		return ret;

	if (priv->ieee80211->state != IEEE80211_LINKED)
		return ret;

	if ((priv->ieee80211->iw_mode != IW_MODE_INFRA))
		return ret;

	spin_lock_irqsave(&priv->ieee80211->lock, flags);
	if (network->flags & NETWORK_HAS_QOS_PARAMETERS) {
		memcpy(&priv->ieee80211->current_network.qos_data.parameters,
			 &network->qos_data.parameters,
			sizeof(struct ieee80211_qos_parameters));
		priv->ieee80211->current_network.qos_data.active = 1;
		set_qos_param = 1;
		/* update qos parameter for current network */
		priv->ieee80211->current_network.qos_data.old_param_count =
			priv->ieee80211->current_network.qos_data.param_count;
		priv->ieee80211->current_network.qos_data.param_count =
			network->qos_data.param_count;

	} else {
		memcpy(&priv->ieee80211->current_network.qos_data.parameters,
		       &def_qos_parameters, size);
		priv->ieee80211->current_network.qos_data.active = 0;
		priv->ieee80211->current_network.qos_data.supported = 0;
		set_qos_param = 1;
	}

	spin_unlock_irqrestore(&priv->ieee80211->lock, flags);

	RT_TRACE(COMP_QOS, "%s: network->flags = %d,%d\n", __FUNCTION__,
		network->flags, priv->ieee80211->current_network.qos_data.active);
	if (set_qos_param == 1)
		queue_work(priv->priv_wq, &priv->qos_activate);

	return ret;
}

static int rtl8192_handle_assoc_response(struct ieee80211_device *ieee,
                                     struct ieee80211_assoc_response_frame *resp,
                                     struct ieee80211_network *network)
{
        struct r8192_priv *priv = ieee80211_priv(ieee->dev);
        rtl8192_qos_association_resp(priv, network);
        return 0;
}

/* updateRATRTabel for MCS only. Basic rate is not implemented. */
static void rtl8192_update_ratr_table(struct r8192_priv* priv)
{
	struct ieee80211_device* ieee = priv->ieee80211;
	u8* pMcsRate = ieee->dot11HTOperationalRateSet;
	u32 ratr_value = 0;
	u8 rate_index = 0;

	rtl8192_config_rate(priv, (u16*)(&ratr_value));
	ratr_value |= (*(u16*)(pMcsRate)) << 12;

	switch (ieee->mode)
	{
		case IEEE_A:
			ratr_value &= 0x00000FF0;
			break;
		case IEEE_B:
			ratr_value &= 0x0000000F;
			break;
		case IEEE_G:
			ratr_value &= 0x00000FF7;
			break;
		case IEEE_N_24G:
		case IEEE_N_5G:
			if (ieee->pHTInfo->PeerMimoPs == 0) //MIMO_PS_STATIC
				ratr_value &= 0x0007F007;
			else{
				if (priv->rf_type == RF_1T2R)
					ratr_value &= 0x000FF007;
				else
					ratr_value &= 0x0F81F007;
			}
			break;
		default:
			break;
	}
	ratr_value &= 0x0FFFFFFF;
	if(ieee->pHTInfo->bCurTxBW40MHz && ieee->pHTInfo->bCurShortGI40MHz){
		ratr_value |= 0x80000000;
	}else if(!ieee->pHTInfo->bCurTxBW40MHz && ieee->pHTInfo->bCurShortGI20MHz){
		ratr_value |= 0x80000000;
	}
	write_nic_dword(priv, RATR0+rate_index*4, ratr_value);
	write_nic_byte(priv, UFWP, 1);
}

static bool GetNmodeSupportBySecCfg8190Pci(struct ieee80211_device *ieee)
{
	return !(ieee->rtllib_ap_sec_type &&
		 (ieee->rtllib_ap_sec_type(ieee)&(SEC_ALG_WEP|SEC_ALG_TKIP)));
}

static void rtl8192_refresh_supportrate(struct r8192_priv* priv)
{
	struct ieee80211_device* ieee = priv->ieee80211;
	//we donot consider set support rate for ABG mode, only HT MCS rate is set here.
	if (ieee->mode == WIRELESS_MODE_N_24G || ieee->mode == WIRELESS_MODE_N_5G)
	{
		memcpy(ieee->Regdot11HTOperationalRateSet, ieee->RegHTSuppRateSet, 16);
	}
	else
		memset(ieee->Regdot11HTOperationalRateSet, 0, 16);
}

static u8 rtl8192_getSupportedWireleeMode(void)
{
	return (WIRELESS_MODE_N_24G|WIRELESS_MODE_G|WIRELESS_MODE_B);
}

static void rtl8192_SetWirelessMode(struct ieee80211_device *ieee, u8 wireless_mode)
{
	struct r8192_priv *priv = ieee80211_priv(ieee->dev);
	u8 bSupportMode = rtl8192_getSupportedWireleeMode();

	if ((wireless_mode == WIRELESS_MODE_AUTO) || ((wireless_mode&bSupportMode)==0))
	{
		if(bSupportMode & WIRELESS_MODE_N_24G)
		{
			wireless_mode = WIRELESS_MODE_N_24G;
		}
		else if(bSupportMode & WIRELESS_MODE_N_5G)
		{
			wireless_mode = WIRELESS_MODE_N_5G;
		}
		else if((bSupportMode & WIRELESS_MODE_A))
		{
			wireless_mode = WIRELESS_MODE_A;
		}
		else if((bSupportMode & WIRELESS_MODE_G))
		{
			wireless_mode = WIRELESS_MODE_G;
		}
		else if((bSupportMode & WIRELESS_MODE_B))
		{
			wireless_mode = WIRELESS_MODE_B;
		}
		else{
			RT_TRACE(COMP_ERR, "%s(), No valid wireless mode supported, SupportedWirelessMode(%x)!!!\n", __FUNCTION__,bSupportMode);
			wireless_mode = WIRELESS_MODE_B;
		}
	}
	priv->ieee80211->mode = wireless_mode;

	if ((wireless_mode == WIRELESS_MODE_N_24G) ||  (wireless_mode == WIRELESS_MODE_N_5G))
		priv->ieee80211->pHTInfo->bEnableHT = 1;
	else
		priv->ieee80211->pHTInfo->bEnableHT = 0;
	RT_TRACE(COMP_INIT, "Current Wireless Mode is %x\n", wireless_mode);
	rtl8192_refresh_supportrate(priv);
}

static bool GetHalfNmodeSupportByAPs819xPci(struct ieee80211_device* ieee)
{
	return ieee->bHalfWirelessN24GMode;
}

static short rtl8192_is_tx_queue_empty(struct ieee80211_device *ieee)
{
	int i=0;
	struct r8192_priv *priv = ieee80211_priv(ieee->dev);

	for (i=0; i<=MGNT_QUEUE; i++)
	{
		if ((i== TXCMD_QUEUE) || (i == HCCA_QUEUE) )
			continue;
		if (skb_queue_len(&(&priv->tx_ring[i])->queue) > 0){
			printk("===>tx queue is not empty:%d, %d\n", i, skb_queue_len(&(&priv->tx_ring[i])->queue));
			return 0;
		}
	}
	return 1;
}

static void rtl8192_hw_sleep_down(struct r8192_priv *priv)
{
	MgntActSet_RF_State(priv, eRfSleep, RF_CHANGE_BY_PS);
}

static void rtl8192_hw_wakeup(struct ieee80211_device *ieee)
{
	struct r8192_priv *priv = ieee80211_priv(ieee->dev);
	MgntActSet_RF_State(priv, eRfOn, RF_CHANGE_BY_PS);
}

static void rtl8192_hw_wakeup_wq (struct work_struct *work)
{
	struct delayed_work *dwork = container_of(work,struct delayed_work,work);
	struct ieee80211_device *ieee = container_of(dwork,struct ieee80211_device,hw_wakeup_wq);

	rtl8192_hw_wakeup(ieee);
}

#define MIN_SLEEP_TIME 50
#define MAX_SLEEP_TIME 10000
static void rtl8192_hw_to_sleep(struct ieee80211_device *ieee, u32 th, u32 tl)
{
	struct r8192_priv *priv = ieee80211_priv(ieee->dev);
	u32 tmp;
	u32 rb = jiffies;

	// Writing HW register with 0 equals to disable
	// the timer, that is not really what we want
	//
	tl -= MSECS(8+16+7);

	// If the interval in witch we are requested to sleep is too
	// short then give up and remain awake
	// when we sleep after send null frame, the timer will be too short to sleep.
	//
	if(((tl>=rb)&& (tl-rb) <= MSECS(MIN_SLEEP_TIME))
			||((rb>tl)&& (rb-tl) < MSECS(MIN_SLEEP_TIME))) {
		printk("too short to sleep::%x, %x, %lx\n",tl, rb,  MSECS(MIN_SLEEP_TIME));
		return;
	}

	if(((tl > rb) && ((tl-rb) > MSECS(MAX_SLEEP_TIME)))||
			((tl < rb) && (tl>MSECS(69)) && ((rb-tl) > MSECS(MAX_SLEEP_TIME)))||
			((tl<rb)&&(tl<MSECS(69))&&((tl+0xffffffff-rb)>MSECS(MAX_SLEEP_TIME)))) {
		printk("========>too long to sleep:%x, %x, %lx\n", tl, rb,  MSECS(MAX_SLEEP_TIME));
		return;
	}

	tmp = (tl>rb)?(tl-rb):(rb-tl);
	queue_delayed_work(priv->ieee80211->wq,
			   &priv->ieee80211->hw_wakeup_wq,tmp);

        rtl8192_hw_sleep_down(priv);
}

static void rtl8192_init_priv_variable(struct r8192_priv *priv)
{
	u8 i;
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;

	// Default Halt the NIC if RF is OFF.
	pPSC->RegRfPsLevel |= RT_RF_OFF_LEVL_HALT_NIC;
	pPSC->RegRfPsLevel |= RT_RF_OFF_LEVL_CLK_REQ;
	pPSC->RegRfPsLevel |= RT_RF_OFF_LEVL_ASPM;
	pPSC->RegRfPsLevel |= RT_RF_LPS_LEVEL_ASPM;
	pPSC->bLeisurePs = true;
	priv->ieee80211->RegMaxLPSAwakeIntvl = 5;
	priv->bHwRadioOff = false;

	priv->being_init_adapter = false;
	priv->txringcount = 64;//32;
	priv->rxbuffersize = 9100;//2048;//1024;
	priv->rxringcount = MAX_RX_COUNT;//64;
	priv->chan = 1; //set to channel 1
	priv->RegWirelessMode = WIRELESS_MODE_AUTO;
	priv->RegChannelPlan = 0xf;
	priv->ieee80211->mode = WIRELESS_MODE_AUTO; //SET AUTO
	priv->ieee80211->iw_mode = IW_MODE_INFRA;
	priv->ieee80211->ieee_up=0;
	priv->retry_rts = DEFAULT_RETRY_RTS;
	priv->retry_data = DEFAULT_RETRY_DATA;
	priv->ieee80211->rts = DEFAULT_RTS_THRESHOLD;
	priv->ieee80211->rate = 110; //11 mbps
	priv->ieee80211->short_slot = 1;
	priv->promisc = (priv->ieee80211->dev->flags & IFF_PROMISC) ? 1:0;
	priv->bcck_in_ch14 = false;
	priv->CCKPresentAttentuation = 0;
	priv->rfa_txpowertrackingindex = 0;
	priv->rfc_txpowertrackingindex = 0;
	priv->CckPwEnl = 6;
	//added by amy for silent reset
	priv->ResetProgress = RESET_TYPE_NORESET;
	priv->bForcedSilentReset = 0;
	priv->bDisableNormalResetCheck = false;
	priv->force_reset = false;
	//added by amy for power save
	priv->RfOffReason = 0;
	priv->bHwRfOffAction = 0;
	priv->PowerSaveControl.bInactivePs = true;
	priv->PowerSaveControl.bIPSModeBackup = false;

	priv->ieee80211->current_network.beacon_interval = DEFAULT_BEACONINTERVAL;
	priv->ieee80211->iw_mode = IW_MODE_INFRA;
	priv->ieee80211->softmac_features  = IEEE_SOFTMAC_SCAN |
		IEEE_SOFTMAC_ASSOCIATE | IEEE_SOFTMAC_PROBERQ |
		IEEE_SOFTMAC_PROBERS | IEEE_SOFTMAC_TX_QUEUE;/* |
		IEEE_SOFTMAC_BEACONS;*///added by amy 080604 //|  //IEEE_SOFTMAC_SINGLE_QUEUE;

	priv->ieee80211->active_scan = 1;
	priv->ieee80211->modulation = IEEE80211_CCK_MODULATION | IEEE80211_OFDM_MODULATION;
	priv->ieee80211->host_encrypt = 1;
	priv->ieee80211->host_decrypt = 1;
	priv->ieee80211->start_send_beacons = rtl8192_start_beacon;
	priv->ieee80211->stop_send_beacons = rtl8192_stop_beacon;
	priv->ieee80211->softmac_hard_start_xmit = rtl8192_hard_start_xmit;
	priv->ieee80211->set_chan = rtl8192_set_chan;
	priv->ieee80211->link_change = rtl8192_link_change;
	priv->ieee80211->softmac_data_hard_start_xmit = rtl8192_hard_data_xmit;
	priv->ieee80211->data_hard_stop = rtl8192_data_hard_stop;
	priv->ieee80211->data_hard_resume = rtl8192_data_hard_resume;
	priv->ieee80211->init_wmmparam_flag = 0;
	priv->ieee80211->fts = DEFAULT_FRAG_THRESHOLD;
	priv->ieee80211->check_nic_enough_desc = check_nic_enough_desc;
	priv->ieee80211->tx_headroom = sizeof(TX_FWINFO_8190PCI);
	priv->ieee80211->qos_support = 1;
	priv->ieee80211->SetBWModeHandler = rtl8192_SetBWMode;
	priv->ieee80211->handle_assoc_response = rtl8192_handle_assoc_response;
	priv->ieee80211->handle_beacon = rtl8192_handle_beacon;

	priv->ieee80211->sta_wake_up = rtl8192_hw_wakeup;
	priv->ieee80211->enter_sleep_state = rtl8192_hw_to_sleep;
	priv->ieee80211->ps_is_queue_empty = rtl8192_is_tx_queue_empty;
	priv->ieee80211->GetNmodeSupportBySecCfg = GetNmodeSupportBySecCfg8190Pci;
	priv->ieee80211->SetWirelessMode = rtl8192_SetWirelessMode;
	priv->ieee80211->GetHalfNmodeSupportByAPsHandler = GetHalfNmodeSupportByAPs819xPci;

	priv->ieee80211->InitialGainHandler = InitialGain819xPci;

#ifdef ENABLE_IPS
	priv->ieee80211->ieee80211_ips_leave_wq = ieee80211_ips_leave_wq;
	priv->ieee80211->ieee80211_ips_leave = ieee80211_ips_leave;
#endif
#ifdef ENABLE_LPS
        priv->ieee80211->LeisurePSLeave            = LeisurePSLeave;
#endif

	priv->ieee80211->SetHwRegHandler = rtl8192e_SetHwReg;
	priv->ieee80211->rtllib_ap_sec_type = rtl8192e_ap_sec_type;

	priv->ShortRetryLimit = 0x30;
	priv->LongRetryLimit = 0x30;

	priv->ReceiveConfig = RCR_ADD3	|
		RCR_AMF | RCR_ADF |		//accept management/data
		RCR_AICV |			//accept control frame for SW AP needs PS-poll, 2005.07.07, by rcnjko.
		RCR_AB | RCR_AM | RCR_APM |	//accept BC/MC/UC
		RCR_AAP | ((u32)7<<RCR_MXDMA_OFFSET) |
		((u32)7 << RCR_FIFO_OFFSET) | RCR_ONLYERLPKT;

	priv->pFirmware = vzalloc(sizeof(rt_firmware));

	/* rx related queue */
	skb_queue_head_init(&priv->skb_queue);

	/* Tx related queue */
	for(i = 0; i < MAX_QUEUE_SIZE; i++) {
		skb_queue_head_init(&priv->ieee80211->skb_waitQ [i]);
	}
	for(i = 0; i < MAX_QUEUE_SIZE; i++) {
		skb_queue_head_init(&priv->ieee80211->skb_aggQ [i]);
	}
	priv->rf_set_chan = rtl8192_phy_SwChnl;
}

static void rtl8192_init_priv_lock(struct r8192_priv* priv)
{
	spin_lock_init(&priv->irq_th_lock);
	spin_lock_init(&priv->rf_ps_lock);
	sema_init(&priv->wx_sem,1);
	sema_init(&priv->rf_sem,1);
	mutex_init(&priv->mutex);
}

/* init tasklet and wait_queue here */
#define DRV_NAME "wlan0"
static void rtl8192_init_priv_task(struct r8192_priv *priv)
{
	priv->priv_wq = create_workqueue(DRV_NAME);

#ifdef ENABLE_IPS
	INIT_WORK(&priv->ieee80211->ips_leave_wq, IPSLeave_wq);
#endif

	INIT_WORK(&priv->reset_wq,  rtl8192_restart);
	INIT_DELAYED_WORK(&priv->watch_dog_wq, rtl819x_watchdog_wqcallback);
	INIT_DELAYED_WORK(&priv->txpower_tracking_wq,  dm_txpower_trackingcallback);
	INIT_DELAYED_WORK(&priv->rfpath_check_wq,  dm_rf_pathcheck_workitemcallback);
	INIT_DELAYED_WORK(&priv->update_beacon_wq, rtl8192_update_beacon);
	INIT_WORK(&priv->qos_activate, rtl8192_qos_activate);
	INIT_DELAYED_WORK(&priv->ieee80211->hw_wakeup_wq, rtl8192_hw_wakeup_wq);

	tasklet_init(&priv->irq_rx_tasklet, rtl8192_irq_rx_tasklet,
		     (unsigned long) priv);
	tasklet_init(&priv->irq_tx_tasklet, rtl8192_irq_tx_tasklet,
		     (unsigned long) priv);
        tasklet_init(&priv->irq_prepare_beacon_tasklet, rtl8192_prepare_beacon,
		     (unsigned long) priv);
}

static void rtl8192_get_eeprom_size(struct r8192_priv *priv)
{
	u16 curCR = 0;
	RT_TRACE(COMP_INIT, "===========>%s()\n", __FUNCTION__);
	curCR = read_nic_dword(priv, EPROM_CMD);
	RT_TRACE(COMP_INIT, "read from Reg Cmd9346CR(%x):%x\n", EPROM_CMD, curCR);
	//whether need I consider BIT5?
	priv->epromtype = (curCR & EPROM_CMD_9356SEL) ? EPROM_93c56 : EPROM_93c46;
	RT_TRACE(COMP_INIT, "<===========%s(), epromtype:%d\n", __FUNCTION__, priv->epromtype);
}

/*
 * Adapter->EEPROMAddressSize should be set before this function call.
 *  EEPROM address size can be got through GetEEPROMSize8185()
 */
static void rtl8192_read_eeprom_info(struct r8192_priv *priv)
{
	struct net_device *dev = priv->ieee80211->dev;
	u8			tempval;
	u8			ICVer8192, ICVer8256;
	u16			i,usValue, IC_Version;
	u16			EEPROMId;
	u8 bMac_Tmp_Addr[6] = {0x00, 0xe0, 0x4c, 0x00, 0x00, 0x01};
	RT_TRACE(COMP_INIT, "====> rtl8192_read_eeprom_info\n");

	// TODO: I don't know if we need to apply EF function to EEPROM read function

	//2 Read EEPROM ID to make sure autoload is success
	EEPROMId = eprom_read(priv, 0);
	if( EEPROMId != RTL8190_EEPROM_ID )
	{
		RT_TRACE(COMP_ERR, "EEPROM ID is invalid:%x, %x\n", EEPROMId, RTL8190_EEPROM_ID);
		priv->AutoloadFailFlag=true;
	}
	else
	{
		priv->AutoloadFailFlag=false;
	}

	//
	// Assign Chip Version ID
	//
	// Read IC Version && Channel Plan
	if(!priv->AutoloadFailFlag)
	{
		// VID, PID
		priv->eeprom_vid = eprom_read(priv, (EEPROM_VID >> 1));
		priv->eeprom_did = eprom_read(priv, (EEPROM_DID >> 1));

		usValue = eprom_read(priv, (u16)(EEPROM_Customer_ID>>1)) >> 8 ;
		priv->eeprom_CustomerID = (u8)( usValue & 0xff);
		usValue = eprom_read(priv, (EEPROM_ICVersion_ChannelPlan>>1));
		priv->eeprom_ChannelPlan = usValue&0xff;
		IC_Version = ((usValue&0xff00)>>8);

		ICVer8192 = (IC_Version&0xf);		//bit0~3; 1:A cut, 2:B cut, 3:C cut...
		ICVer8256 = ((IC_Version&0xf0)>>4);//bit4~6, bit7 reserved for other RF chip; 1:A cut, 2:B cut, 3:C cut...
		RT_TRACE(COMP_INIT, "ICVer8192 = 0x%x\n", ICVer8192);
		RT_TRACE(COMP_INIT, "ICVer8256 = 0x%x\n", ICVer8256);
		if(ICVer8192 == 0x2)	//B-cut
		{
			if(ICVer8256 == 0x5) //E-cut
				priv->card_8192_version= VERSION_8190_BE;
		}

		switch(priv->card_8192_version)
		{
			case VERSION_8190_BD:
			case VERSION_8190_BE:
				break;
			default:
				priv->card_8192_version = VERSION_8190_BD;
				break;
		}
		RT_TRACE(COMP_INIT, "\nIC Version = 0x%x\n", priv->card_8192_version);
	}
	else
	{
		priv->card_8192_version = VERSION_8190_BD;
		priv->eeprom_vid = 0;
		priv->eeprom_did = 0;
		priv->eeprom_CustomerID = 0;
		priv->eeprom_ChannelPlan = 0;
		RT_TRACE(COMP_INIT, "IC Version = 0x%x\n", 0xff);
	}

	RT_TRACE(COMP_INIT, "EEPROM VID = 0x%4x\n", priv->eeprom_vid);
	RT_TRACE(COMP_INIT, "EEPROM DID = 0x%4x\n", priv->eeprom_did);
	RT_TRACE(COMP_INIT,"EEPROM Customer ID: 0x%2x\n", priv->eeprom_CustomerID);

	//2 Read Permanent MAC address
	if(!priv->AutoloadFailFlag)
	{
		for(i = 0; i < 6; i += 2)
		{
			usValue = eprom_read(priv, (u16) ((EEPROM_NODE_ADDRESS_BYTE_0+i)>>1));
			*(u16*)(&dev->dev_addr[i]) = usValue;
		}
	} else {
		// when auto load failed,  the last address byte set to be a random one.
		// added by david woo.2007/11/7
		memcpy(dev->dev_addr, bMac_Tmp_Addr, 6);
	}

	RT_TRACE(COMP_INIT, "Permanent Address = %pM\n", dev->dev_addr);

		//2 TX Power Check EEPROM Fail or not
	if(priv->card_8192_version > VERSION_8190_BD) {
		priv->bTXPowerDataReadFromEEPORM = true;
	} else {
		priv->bTXPowerDataReadFromEEPORM = false;
	}

	// 2007/11/15 MH 8190PCI Default=2T4R, 8192PCIE default=1T2R
	priv->rf_type = RTL819X_DEFAULT_RF_TYPE;

	if(priv->card_8192_version > VERSION_8190_BD)
	{
		// Read RF-indication and Tx Power gain index diff of legacy to HT OFDM rate.
		if(!priv->AutoloadFailFlag)
		{
			tempval = (eprom_read(priv, (EEPROM_RFInd_PowerDiff>>1))) & 0xff;
			priv->EEPROMLegacyHTTxPowerDiff = tempval & 0xf;	// bit[3:0]

			if (tempval&0x80)	//RF-indication, bit[7]
				priv->rf_type = RF_1T2R;
			else
				priv->rf_type = RF_2T4R;
		}
		else
		{
			priv->EEPROMLegacyHTTxPowerDiff = EEPROM_Default_LegacyHTTxPowerDiff;
		}
		RT_TRACE(COMP_INIT, "EEPROMLegacyHTTxPowerDiff = %d\n",
			priv->EEPROMLegacyHTTxPowerDiff);

		// Read ThermalMeter from EEPROM
		if(!priv->AutoloadFailFlag)
		{
			priv->EEPROMThermalMeter = (u8)(((eprom_read(priv, (EEPROM_ThermalMeter>>1))) & 0xff00)>>8);
		}
		else
		{
			priv->EEPROMThermalMeter = EEPROM_Default_ThermalMeter;
		}
		RT_TRACE(COMP_INIT, "ThermalMeter = %d\n", priv->EEPROMThermalMeter);
		//vivi, for tx power track
		priv->TSSI_13dBm = priv->EEPROMThermalMeter *100;

		if(priv->epromtype == EPROM_93c46)
		{
		// Read antenna tx power offset of B/C/D to A and CrystalCap from EEPROM
		if(!priv->AutoloadFailFlag)
		{
				usValue = eprom_read(priv, (EEPROM_TxPwDiff_CrystalCap>>1));
				priv->EEPROMAntPwDiff = (usValue&0x0fff);
				priv->EEPROMCrystalCap = (u8)((usValue&0xf000)>>12);
		}
		else
		{
				priv->EEPROMAntPwDiff = EEPROM_Default_AntTxPowerDiff;
				priv->EEPROMCrystalCap = EEPROM_Default_TxPwDiff_CrystalCap;
		}
			RT_TRACE(COMP_INIT, "EEPROMAntPwDiff = %d\n", priv->EEPROMAntPwDiff);
			RT_TRACE(COMP_INIT, "EEPROMCrystalCap = %d\n", priv->EEPROMCrystalCap);

		//
		// Get per-channel Tx Power Level
		//
		for(i=0; i<14; i+=2)
		{
			if(!priv->AutoloadFailFlag)
			{
				usValue = eprom_read(priv, (u16) ((EEPROM_TxPwIndex_CCK+i)>>1) );
			}
			else
			{
				usValue = EEPROM_Default_TxPower;
			}
			*((u16*)(&priv->EEPROMTxPowerLevelCCK[i])) = usValue;
			RT_TRACE(COMP_INIT,"CCK Tx Power Level, Index %d = 0x%02x\n", i, priv->EEPROMTxPowerLevelCCK[i]);
			RT_TRACE(COMP_INIT, "CCK Tx Power Level, Index %d = 0x%02x\n", i+1, priv->EEPROMTxPowerLevelCCK[i+1]);
		}
		for(i=0; i<14; i+=2)
		{
			if(!priv->AutoloadFailFlag)
			{
				usValue = eprom_read(priv, (u16) ((EEPROM_TxPwIndex_OFDM_24G+i)>>1) );
			}
			else
			{
				usValue = EEPROM_Default_TxPower;
			}
			*((u16*)(&priv->EEPROMTxPowerLevelOFDM24G[i])) = usValue;
			RT_TRACE(COMP_INIT, "OFDM 2.4G Tx Power Level, Index %d = 0x%02x\n", i, priv->EEPROMTxPowerLevelOFDM24G[i]);
			RT_TRACE(COMP_INIT, "OFDM 2.4G Tx Power Level, Index %d = 0x%02x\n", i+1, priv->EEPROMTxPowerLevelOFDM24G[i+1]);
		}
		}

		//
		// Update HAL variables.
		//
		if(priv->epromtype == EPROM_93c46)
		{
			for(i=0; i<14; i++)
			{
				priv->TxPowerLevelCCK[i] = priv->EEPROMTxPowerLevelCCK[i];
				priv->TxPowerLevelOFDM24G[i] = priv->EEPROMTxPowerLevelOFDM24G[i];
			}
			priv->LegacyHTTxPowerDiff = priv->EEPROMLegacyHTTxPowerDiff;
		// Antenna B gain offset to antenna A, bit0~3
			priv->AntennaTxPwDiff[0] = (priv->EEPROMAntPwDiff & 0xf);
		// Antenna C gain offset to antenna A, bit4~7
			priv->AntennaTxPwDiff[1] = ((priv->EEPROMAntPwDiff & 0xf0)>>4);
		// Antenna D gain offset to antenna A, bit8~11
			priv->AntennaTxPwDiff[2] = ((priv->EEPROMAntPwDiff & 0xf00)>>8);
		// CrystalCap, bit12~15
			priv->CrystalCap = priv->EEPROMCrystalCap;
		// ThermalMeter, bit0~3 for RFIC1, bit4~7 for RFIC2
			priv->ThermalMeter[0] = (priv->EEPROMThermalMeter & 0xf);
			priv->ThermalMeter[1] = ((priv->EEPROMThermalMeter & 0xf0)>>4);
		}
		else if(priv->epromtype == EPROM_93c56)
		{
			for(i=0; i<3; i++)	// channel 1~3 use the same Tx Power Level.
			{
				priv->TxPowerLevelCCK_A[i]  = priv->EEPROMRfACCKChnl1TxPwLevel[0];
				priv->TxPowerLevelOFDM24G_A[i] = priv->EEPROMRfAOfdmChnlTxPwLevel[0];
				priv->TxPowerLevelCCK_C[i] =  priv->EEPROMRfCCCKChnl1TxPwLevel[0];
				priv->TxPowerLevelOFDM24G_C[i] = priv->EEPROMRfCOfdmChnlTxPwLevel[0];
			}
			for(i=3; i<9; i++)	// channel 4~9 use the same Tx Power Level
			{
				priv->TxPowerLevelCCK_A[i]  = priv->EEPROMRfACCKChnl1TxPwLevel[1];
				priv->TxPowerLevelOFDM24G_A[i] = priv->EEPROMRfAOfdmChnlTxPwLevel[1];
				priv->TxPowerLevelCCK_C[i] =  priv->EEPROMRfCCCKChnl1TxPwLevel[1];
				priv->TxPowerLevelOFDM24G_C[i] = priv->EEPROMRfCOfdmChnlTxPwLevel[1];
			}
			for(i=9; i<14; i++)	// channel 10~14 use the same Tx Power Level
			{
				priv->TxPowerLevelCCK_A[i]  = priv->EEPROMRfACCKChnl1TxPwLevel[2];
				priv->TxPowerLevelOFDM24G_A[i] = priv->EEPROMRfAOfdmChnlTxPwLevel[2];
				priv->TxPowerLevelCCK_C[i] =  priv->EEPROMRfCCCKChnl1TxPwLevel[2];
				priv->TxPowerLevelOFDM24G_C[i] = priv->EEPROMRfCOfdmChnlTxPwLevel[2];
			}
			for(i=0; i<14; i++)
				RT_TRACE(COMP_INIT, "priv->TxPowerLevelCCK_A[%d] = 0x%x\n", i, priv->TxPowerLevelCCK_A[i]);
			for(i=0; i<14; i++)
				RT_TRACE(COMP_INIT,"priv->TxPowerLevelOFDM24G_A[%d] = 0x%x\n", i, priv->TxPowerLevelOFDM24G_A[i]);
			for(i=0; i<14; i++)
				RT_TRACE(COMP_INIT, "priv->TxPowerLevelCCK_C[%d] = 0x%x\n", i, priv->TxPowerLevelCCK_C[i]);
			for(i=0; i<14; i++)
				RT_TRACE(COMP_INIT, "priv->TxPowerLevelOFDM24G_C[%d] = 0x%x\n", i, priv->TxPowerLevelOFDM24G_C[i]);
			priv->LegacyHTTxPowerDiff = priv->EEPROMLegacyHTTxPowerDiff;
			priv->AntennaTxPwDiff[0] = 0;
			priv->AntennaTxPwDiff[1] = 0;
			priv->AntennaTxPwDiff[2] = 0;
			priv->CrystalCap = priv->EEPROMCrystalCap;
			// ThermalMeter, bit0~3 for RFIC1, bit4~7 for RFIC2
			priv->ThermalMeter[0] = (priv->EEPROMThermalMeter & 0xf);
			priv->ThermalMeter[1] = ((priv->EEPROMThermalMeter & 0xf0)>>4);
		}
	}

	if(priv->rf_type == RF_1T2R)
	{
		RT_TRACE(COMP_INIT, "1T2R config\n");
	}
	else if (priv->rf_type == RF_2T4R)
	{
		RT_TRACE(COMP_INIT, "2T4R config\n");
	}

	// 2008/01/16 MH We can only know RF type in the function. So we have to init
	// DIG RATR table again.
	init_rate_adaptive(priv);

	//1 Make a copy for following variables and we can change them if we want

	if(priv->RegChannelPlan == 0xf)
	{
		priv->ChannelPlan = priv->eeprom_ChannelPlan;
	}
	else
	{
		priv->ChannelPlan = priv->RegChannelPlan;
	}

	//
	//  Used PID and DID to Set CustomerID
	//
	if( priv->eeprom_vid == 0x1186 &&  priv->eeprom_did == 0x3304 )
	{
		priv->CustomerID =  RT_CID_DLINK;
	}

	switch(priv->eeprom_CustomerID)
	{
		case EEPROM_CID_DEFAULT:
			priv->CustomerID = RT_CID_DEFAULT;
			break;
		case EEPROM_CID_CAMEO:
			priv->CustomerID = RT_CID_819x_CAMEO;
			break;
		case  EEPROM_CID_RUNTOP:
			priv->CustomerID = RT_CID_819x_RUNTOP;
			break;
		case EEPROM_CID_NetCore:
			priv->CustomerID = RT_CID_819x_Netcore;
			break;
		case EEPROM_CID_TOSHIBA:        // Merge by Jacken, 2008/01/31
			priv->CustomerID = RT_CID_TOSHIBA;
			if(priv->eeprom_ChannelPlan&0x80)
				priv->ChannelPlan = priv->eeprom_ChannelPlan&0x7f;
			else
				priv->ChannelPlan = 0x0;
			RT_TRACE(COMP_INIT, "Toshiba ChannelPlan = 0x%x\n",
				priv->ChannelPlan);
			break;
		case EEPROM_CID_Nettronix:
			priv->CustomerID = RT_CID_Nettronix;
			break;
		case EEPROM_CID_Pronet:
			priv->CustomerID = RT_CID_PRONET;
			break;
		case EEPROM_CID_DLINK:
			priv->CustomerID = RT_CID_DLINK;
			break;

		case EEPROM_CID_WHQL:
			break;
		default:
			// value from RegCustomerID
			break;
	}

	//Avoid the channel plan array overflow, by Bruce, 2007-08-27.
	if(priv->ChannelPlan > CHANNEL_PLAN_LEN - 1)
		priv->ChannelPlan = 0; //FCC

	if( priv->eeprom_vid == 0x1186 &&  priv->eeprom_did == 0x3304)
		priv->ieee80211->bSupportRemoteWakeUp = true;
	else
		priv->ieee80211->bSupportRemoteWakeUp = false;

	RT_TRACE(COMP_INIT, "RegChannelPlan(%d)\n", priv->RegChannelPlan);
	RT_TRACE(COMP_INIT, "ChannelPlan = %d\n", priv->ChannelPlan);
	RT_TRACE(COMP_TRACE, "<==== ReadAdapterInfo\n");
}

static short rtl8192_get_channel_map(struct r8192_priv *priv)
{
#ifdef ENABLE_DOT11D
	if(priv->ChannelPlan> COUNTRY_CODE_GLOBAL_DOMAIN){
		printk("rtl8180_init:Error channel plan! Set to default.\n");
		priv->ChannelPlan= 0;
	}
	RT_TRACE(COMP_INIT, "Channel plan is %d\n",priv->ChannelPlan);

	rtl819x_set_channel_map(priv->ChannelPlan, priv);
#else
	int ch,i;
	//Set Default Channel Plan
	if(!channels){
		DMESG("No channels, aborting");
		return -1;
	}
	ch=channels;
	priv->ChannelPlan= 0;//hikaru
	 // set channels 1..14 allowed in given locale
	for (i=1; i<=14; i++) {
		(priv->ieee80211->channel_map)[i] = (u8)(ch & 0x01);
		ch >>= 1;
	}
#endif
	return 0;
}

static short rtl8192_init(struct r8192_priv *priv)
{
	struct net_device *dev = priv->ieee80211->dev;

	memset(&(priv->stats),0,sizeof(struct Stats));
	rtl8192_init_priv_variable(priv);
	rtl8192_init_priv_lock(priv);
	rtl8192_init_priv_task(priv);
	rtl8192_get_eeprom_size(priv);
	rtl8192_read_eeprom_info(priv);
	rtl8192_get_channel_map(priv);
	init_hal_dm(priv);
	init_timer(&priv->watch_dog_timer);
	priv->watch_dog_timer.data = (unsigned long)priv;
	priv->watch_dog_timer.function = watch_dog_timer_callback;
        if (request_irq(dev->irq, rtl8192_interrupt, IRQF_SHARED, dev->name, priv)) {
		printk("Error allocating IRQ %d",dev->irq);
		return -1;
	}else{
		priv->irq=dev->irq;
		printk("IRQ %d",dev->irq);
	}
	if (rtl8192_pci_initdescring(priv) != 0){
		printk("Endopoints initialization failed");
		return -1;
	}

	return 0;
}

/*
 * Actually only set RRSR, RATR and BW_OPMODE registers
 *  not to do all the hw config as its name says
 * This part need to modified according to the rate set we filtered
 */
static void rtl8192_hwconfig(struct r8192_priv *priv)
{
	u32 regRATR = 0, regRRSR = 0;
	u8 regBwOpMode = 0, regTmp = 0;

// Set RRSR, RATR, and BW_OPMODE registers
	//
	switch (priv->ieee80211->mode)
	{
	case WIRELESS_MODE_B:
		regBwOpMode = BW_OPMODE_20MHZ;
		regRATR = RATE_ALL_CCK;
		regRRSR = RATE_ALL_CCK;
		break;
	case WIRELESS_MODE_A:
		regBwOpMode = BW_OPMODE_5G |BW_OPMODE_20MHZ;
		regRATR = RATE_ALL_OFDM_AG;
		regRRSR = RATE_ALL_OFDM_AG;
		break;
	case WIRELESS_MODE_G:
		regBwOpMode = BW_OPMODE_20MHZ;
		regRATR = RATE_ALL_CCK | RATE_ALL_OFDM_AG;
		regRRSR = RATE_ALL_CCK | RATE_ALL_OFDM_AG;
		break;
	case WIRELESS_MODE_AUTO:
	case WIRELESS_MODE_N_24G:
		// It support CCK rate by default.
		// CCK rate will be filtered out only when associated AP does not support it.
		regBwOpMode = BW_OPMODE_20MHZ;
			regRATR = RATE_ALL_CCK | RATE_ALL_OFDM_AG | RATE_ALL_OFDM_1SS | RATE_ALL_OFDM_2SS;
			regRRSR = RATE_ALL_CCK | RATE_ALL_OFDM_AG;
		break;
	case WIRELESS_MODE_N_5G:
		regBwOpMode = BW_OPMODE_5G;
		regRATR = RATE_ALL_OFDM_AG | RATE_ALL_OFDM_1SS | RATE_ALL_OFDM_2SS;
		regRRSR = RATE_ALL_OFDM_AG;
		break;
	}

	write_nic_byte(priv, BW_OPMODE, regBwOpMode);
	{
		u32 ratr_value = 0;
		ratr_value = regRATR;
		if (priv->rf_type == RF_1T2R)
		{
			ratr_value &= ~(RATE_ALL_OFDM_2SS);
		}
		write_nic_dword(priv, RATR0, ratr_value);
		write_nic_byte(priv, UFWP, 1);
	}
	regTmp = read_nic_byte(priv, 0x313);
	regRRSR = ((regTmp) << 24) | (regRRSR & 0x00ffffff);
	write_nic_dword(priv, RRSR, regRRSR);

	//
	// Set Retry Limit here
	//
	write_nic_word(priv, RETRY_LIMIT,
			priv->ShortRetryLimit << RETRY_LIMIT_SHORT_SHIFT |
			priv->LongRetryLimit << RETRY_LIMIT_LONG_SHIFT);
	// Set Contention Window here

	// Set Tx AGC

	// Set Tx Antenna including Feedback control

	// Set Auto Rate fallback control

}

static RT_STATUS rtl8192_adapter_start(struct r8192_priv *priv)
{
	struct net_device *dev = priv->ieee80211->dev;
	u32 ulRegRead;
	RT_STATUS rtStatus = RT_STATUS_SUCCESS;
	u8 tmpvalue;
	u8 ICVersion,SwitchingRegulatorOutput;
	bool bfirmwareok = true;
	u32	tmpRegA, tmpRegC, TempCCk;
	int	i =0;

	RT_TRACE(COMP_INIT, "====>%s()\n", __FUNCTION__);
	priv->being_init_adapter = true;
        rtl8192_pci_resetdescring(priv);
	// 2007/11/02 MH Before initalizing RF. We can not use FW to do RF-R/W.
	priv->Rf_Mode = RF_OP_By_SW_3wire;

        //dPLL on
        if(priv->ResetProgress == RESET_TYPE_NORESET)
        {
            write_nic_byte(priv, ANAPAR, 0x37);
            // Accordign to designer's explain, LBUS active will never > 10ms. We delay 10ms
            // Joseph increae the time to prevent firmware download fail
            mdelay(500);
        }

	//PlatformSleepUs(10000);
	// For any kind of InitializeAdapter process, we shall use system now!!
	priv->pFirmware->firmware_status = FW_STATUS_0_INIT;

	//
	//3 //Config CPUReset Register
	//3//
	//3 Firmware Reset Or Not
	ulRegRead = read_nic_dword(priv, CPU_GEN);
	if(priv->pFirmware->firmware_status == FW_STATUS_0_INIT)
	{	//called from MPInitialized. do nothing
		ulRegRead |= CPU_GEN_SYSTEM_RESET;
	}else if(priv->pFirmware->firmware_status == FW_STATUS_5_READY)
		ulRegRead |= CPU_GEN_FIRMWARE_RESET;	// Called from MPReset
	else
		RT_TRACE(COMP_ERR, "ERROR in %s(): undefined firmware state(%d)\n", __FUNCTION__,   priv->pFirmware->firmware_status);

	write_nic_dword(priv, CPU_GEN, ulRegRead);

	//3//
	//3 //Fix the issue of E-cut high temperature issue
	//3//
	// TODO: E cut only
	ICVersion = read_nic_byte(priv, IC_VERRSION);
	if(ICVersion >= 0x4) //E-cut only
	{
		// HW SD suggest that we should not wirte this register too often, so driver
		// should readback this register. This register will be modified only when
		// power on reset
		SwitchingRegulatorOutput = read_nic_byte(priv, SWREGULATOR);
		if(SwitchingRegulatorOutput  != 0xb8)
		{
			write_nic_byte(priv, SWREGULATOR, 0xa8);
			mdelay(1);
			write_nic_byte(priv, SWREGULATOR, 0xb8);
		}
	}

	//3//
	//3// Initialize BB before MAC
	//3//
	RT_TRACE(COMP_INIT, "BB Config Start!\n");
	rtStatus = rtl8192_BBConfig(priv);
	if(rtStatus != RT_STATUS_SUCCESS)
	{
		RT_TRACE(COMP_ERR, "BB Config failed\n");
		return rtStatus;
	}
	RT_TRACE(COMP_INIT,"BB Config Finished!\n");

	//3//Set Loopback mode or Normal mode
	//3//
	//2006.12.13 by emily. Note!We should not merge these two CPU_GEN register writings
	//	because setting of System_Reset bit reset MAC to default transmission mode.
		//Loopback mode or not
	priv->LoopbackMode = RTL819X_NO_LOOPBACK;
	if(priv->ResetProgress == RESET_TYPE_NORESET)
	{
	ulRegRead = read_nic_dword(priv, CPU_GEN);
	if(priv->LoopbackMode == RTL819X_NO_LOOPBACK)
	{
		ulRegRead = ((ulRegRead & CPU_GEN_NO_LOOPBACK_MSK) | CPU_GEN_NO_LOOPBACK_SET);
	}
	else if (priv->LoopbackMode == RTL819X_MAC_LOOPBACK )
	{
		ulRegRead |= CPU_CCK_LOOPBACK;
	}
	else
	{
		RT_TRACE(COMP_ERR,"Serious error: wrong loopback mode setting\n");
	}

	//2008.06.03, for WOL
	//ulRegRead &= (~(CPU_GEN_GPIO_UART));
	write_nic_dword(priv, CPU_GEN, ulRegRead);

	// 2006.11.29. After reset cpu, we sholud wait for a second, otherwise, it may fail to write registers. Emily
	udelay(500);
	}
	//3Set Hardware(Do nothing now)
	rtl8192_hwconfig(priv);
	//2=======================================================
	// Common Setting for all of the FPGA platform. (part 1)
	//2=======================================================
	// If there is changes, please make sure it applies to all of the FPGA version
	//3 Turn on Tx/Rx
	write_nic_byte(priv, CMDR, CR_RE|CR_TE);

	//2Set Tx dma burst
	write_nic_byte(priv, PCIF, ((MXDMA2_NoLimit<<MXDMA2_RX_SHIFT) |
				   (MXDMA2_NoLimit<<MXDMA2_TX_SHIFT) ));

	//set IDR0 here
	write_nic_dword(priv, MAC0, ((u32*)dev->dev_addr)[0]);
	write_nic_word(priv, MAC4, ((u16*)(dev->dev_addr + 4))[0]);
	//set RCR
	write_nic_dword(priv, RCR, priv->ReceiveConfig);

	//3 Initialize Number of Reserved Pages in Firmware Queue
		write_nic_dword(priv, RQPN1,  NUM_OF_PAGE_IN_FW_QUEUE_BK << RSVD_FW_QUEUE_PAGE_BK_SHIFT |
					NUM_OF_PAGE_IN_FW_QUEUE_BE << RSVD_FW_QUEUE_PAGE_BE_SHIFT |
					NUM_OF_PAGE_IN_FW_QUEUE_VI << RSVD_FW_QUEUE_PAGE_VI_SHIFT |
					NUM_OF_PAGE_IN_FW_QUEUE_VO <<RSVD_FW_QUEUE_PAGE_VO_SHIFT);
		write_nic_dword(priv, RQPN2, NUM_OF_PAGE_IN_FW_QUEUE_MGNT << RSVD_FW_QUEUE_PAGE_MGNT_SHIFT);
		write_nic_dword(priv, RQPN3, APPLIED_RESERVED_QUEUE_IN_FW|
					NUM_OF_PAGE_IN_FW_QUEUE_BCN<<RSVD_FW_QUEUE_PAGE_BCN_SHIFT|
					NUM_OF_PAGE_IN_FW_QUEUE_PUB<<RSVD_FW_QUEUE_PAGE_PUB_SHIFT);

	rtl8192_tx_enable(priv);
	rtl8192_rx_enable(priv);
	//3Set Response Rate Setting Register
	// CCK rate is supported by default.
	// CCK rate will be filtered out only when associated AP does not support it.
	ulRegRead = (0xFFF00000 & read_nic_dword(priv, RRSR))  | RATE_ALL_OFDM_AG | RATE_ALL_CCK;
	write_nic_dword(priv, RRSR, ulRegRead);
	write_nic_dword(priv, RATR0+4*7, (RATE_ALL_OFDM_AG | RATE_ALL_CCK));

	//2Set AckTimeout
	// TODO: (it value is only for FPGA version). need to be changed!!2006.12.18, by Emily
	write_nic_byte(priv, ACK_TIMEOUT, 0x30);

	if(priv->ResetProgress == RESET_TYPE_NORESET)
	rtl8192_SetWirelessMode(priv->ieee80211, priv->ieee80211->mode);
	//-----------------------------------------------------------------------------
	// Set up security related. 070106, by rcnjko:
	// 1. Clear all H/W keys.
	// 2. Enable H/W encryption/decryption.
	//-----------------------------------------------------------------------------
	CamResetAllEntry(priv);
	{
		u8 SECR_value = 0x0;
		SECR_value |= SCR_TxEncEnable;
		SECR_value |= SCR_RxDecEnable;
		SECR_value |= SCR_NoSKMC;
		write_nic_byte(priv, SECR, SECR_value);
	}
	//3Beacon related
	write_nic_word(priv, ATIMWND, 2);
	write_nic_word(priv, BCN_INTERVAL, 100);
	for (i=0; i<QOS_QUEUE_NUM; i++)
		write_nic_dword(priv, WDCAPARA_ADD[i], 0x005e4332);
	//
	// Switching regulator controller: This is set temporarily.
	// It's not sure if this can be removed in the future.
	// PJ advised to leave it by default.
	//
	write_nic_byte(priv, 0xbe, 0xc0);

	//2=======================================================
	// Set PHY related configuration defined in MAC register bank
	//2=======================================================
	rtl8192_phy_configmac(priv);

	if (priv->card_8192_version > (u8) VERSION_8190_BD) {
		rtl8192_phy_getTxPower(priv);
		rtl8192_phy_setTxPower(priv, priv->chan);
	}

	//if D or C cut
		tmpvalue = read_nic_byte(priv, IC_VERRSION);
		priv->IC_Cut = tmpvalue;
		RT_TRACE(COMP_INIT, "priv->IC_Cut = 0x%x\n", priv->IC_Cut);
		if(priv->IC_Cut >= IC_VersionCut_D)
		{
			//pHalData->bDcut = TRUE;
			if(priv->IC_Cut == IC_VersionCut_D)
				RT_TRACE(COMP_INIT, "D-cut\n");
			if(priv->IC_Cut == IC_VersionCut_E)
			{
				RT_TRACE(COMP_INIT, "E-cut\n");
				// HW SD suggest that we should not wirte this register too often, so driver
				// should readback this register. This register will be modified only when
				// power on reset
			}
		}
		else
		{
			//pHalData->bDcut = FALSE;
			RT_TRACE(COMP_INIT, "Before C-cut\n");
		}

	//Firmware download
	RT_TRACE(COMP_INIT, "Load Firmware!\n");
	bfirmwareok = init_firmware(priv);
	if(bfirmwareok != true) {
		rtStatus = RT_STATUS_FAILURE;
		return rtStatus;
	}
	RT_TRACE(COMP_INIT, "Load Firmware finished!\n");

	//RF config
	if(priv->ResetProgress == RESET_TYPE_NORESET)
	{
	RT_TRACE(COMP_INIT, "RF Config Started!\n");
	rtStatus = rtl8192_phy_RFConfig(priv);
	if(rtStatus != RT_STATUS_SUCCESS)
	{
		RT_TRACE(COMP_ERR, "RF Config failed\n");
			return rtStatus;
	}
	RT_TRACE(COMP_INIT, "RF Config Finished!\n");
	}
	rtl8192_phy_updateInitGain(priv);

	/*---- Set CCK and OFDM Block "ON"----*/
	rtl8192_setBBreg(priv, rFPGA0_RFMOD, bCCKEn, 0x1);
	rtl8192_setBBreg(priv, rFPGA0_RFMOD, bOFDMEn, 0x1);

	//Enable Led
	write_nic_byte(priv, 0x87, 0x0);

	//2=======================================================
	// RF Power Save
	//2=======================================================
#ifdef ENABLE_IPS

{
	if(priv->RfOffReason > RF_CHANGE_BY_PS)
	{ // H/W or S/W RF OFF before sleep.
		RT_TRACE((COMP_INIT|COMP_RF|COMP_POWER), "%s(): Turn off RF for RfOffReason(%d)\n", __FUNCTION__,priv->RfOffReason);
		MgntActSet_RF_State(priv, eRfOff, priv->RfOffReason);
	}
	else if(priv->RfOffReason >= RF_CHANGE_BY_IPS)
	{ // H/W or S/W RF OFF before sleep.
		RT_TRACE((COMP_INIT|COMP_RF|COMP_POWER), "%s(): Turn off RF for RfOffReason(%d)\n",  __FUNCTION__, priv->RfOffReason);
		MgntActSet_RF_State(priv, eRfOff, priv->RfOffReason);
	}
	else
	{
		RT_TRACE((COMP_INIT|COMP_RF|COMP_POWER), "%s(): RF-ON \n",__FUNCTION__);
		priv->eRFPowerState = eRfOn;
		priv->RfOffReason = 0;
	}
}
#endif
	// We can force firmware to do RF-R/W
	if(priv->ieee80211->FwRWRF)
		priv->Rf_Mode = RF_OP_By_FW;
	else
		priv->Rf_Mode = RF_OP_By_SW_3wire;

	if(priv->ResetProgress == RESET_TYPE_NORESET)
	{
		dm_initialize_txpower_tracking(priv);

		if(priv->IC_Cut >= IC_VersionCut_D)
		{
			tmpRegA = rtl8192_QueryBBReg(priv, rOFDM0_XATxIQImbalance, bMaskDWord);
			tmpRegC = rtl8192_QueryBBReg(priv, rOFDM0_XCTxIQImbalance, bMaskDWord);
			for(i = 0; i<TxBBGainTableLength; i++)
			{
				if(tmpRegA == priv->txbbgain_table[i].txbbgain_value)
				{
					priv->rfa_txpowertrackingindex= (u8)i;
					priv->rfa_txpowertrackingindex_real= (u8)i;
					priv->rfa_txpowertracking_default = priv->rfa_txpowertrackingindex;
					break;
				}
			}

		TempCCk = rtl8192_QueryBBReg(priv, rCCK0_TxFilter1, bMaskByte2);

		for(i=0 ; i<CCKTxBBGainTableLength ; i++)
		{
			if(TempCCk == priv->cck_txbbgain_table[i].ccktxbb_valuearray[0])
			{
				priv->CCKPresentAttentuation_20Mdefault =(u8) i;
				break;
			}
		}
		priv->CCKPresentAttentuation_40Mdefault = 0;
		priv->CCKPresentAttentuation_difference = 0;
		priv->CCKPresentAttentuation = priv->CCKPresentAttentuation_20Mdefault;
			RT_TRACE(COMP_POWER_TRACKING, "priv->rfa_txpowertrackingindex_initial = %d\n", priv->rfa_txpowertrackingindex);
			RT_TRACE(COMP_POWER_TRACKING, "priv->rfa_txpowertrackingindex_real__initial = %d\n", priv->rfa_txpowertrackingindex_real);
			RT_TRACE(COMP_POWER_TRACKING, "priv->CCKPresentAttentuation_difference_initial = %d\n", priv->CCKPresentAttentuation_difference);
			RT_TRACE(COMP_POWER_TRACKING, "priv->CCKPresentAttentuation_initial = %d\n", priv->CCKPresentAttentuation);
			priv->btxpower_tracking = FALSE;//TEMPLY DISABLE
		}
	}

	rtl8192_irq_enable(priv);
	priv->being_init_adapter = false;
	return rtStatus;

}

static void rtl8192_prepare_beacon(unsigned long arg)
{
	struct r8192_priv *priv = (struct r8192_priv*) arg;
	struct sk_buff *skb;
	cb_desc *tcb_desc;

	skb = ieee80211_get_beacon(priv->ieee80211);
	tcb_desc = (cb_desc *)(skb->cb + 8);
	/* prepare misc info for the beacon xmit */
	tcb_desc->queue_index = BEACON_QUEUE;
	/* IBSS does not support HT yet, use 1M defaultly */
	tcb_desc->data_rate = 2;
	tcb_desc->RATRIndex = 7;
	tcb_desc->bTxDisableRateFallBack = 1;
	tcb_desc->bTxUseDriverAssingedRate = 1;

	skb_push(skb, priv->ieee80211->tx_headroom);
	if(skb){
		rtl8192_tx(priv, skb);
	}
}

/*
 * configure registers for beacon tx and enables it via
 * rtl8192_beacon_tx_enable(). rtl8192_beacon_tx_disable() might
 * be used to stop beacon transmission
 */
static void rtl8192_start_beacon(struct ieee80211_device *ieee80211)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	struct ieee80211_network *net = &priv->ieee80211->current_network;
	u16 BcnTimeCfg = 0;
        u16 BcnCW = 6;
        u16 BcnIFS = 0xf;

	DMESG("Enabling beacon TX");
	rtl8192_irq_disable(priv);
	//rtl8192_beacon_tx_enable(dev);

	/* ATIM window */
	write_nic_word(priv, ATIMWND, 2);

	/* Beacon interval (in unit of TU) */
	write_nic_word(priv, BCN_INTERVAL, net->beacon_interval);

	/*
	 * DrvErlyInt (in unit of TU).
	 * (Time to send interrupt to notify driver to c
	 * hange beacon content)
	 * */
	write_nic_word(priv, BCN_DRV_EARLY_INT, 10);

	/*
	 * BcnDMATIM(in unit of us).
	 * Indicates the time before TBTT to perform beacon queue DMA
	 * */
	write_nic_word(priv, BCN_DMATIME, 256);

	/*
	 * Force beacon frame transmission even after receiving
	 * beacon frame from other ad hoc STA
	 * */
	write_nic_byte(priv, BCN_ERR_THRESH, 100);

	/* Set CW and IFS */
	BcnTimeCfg |= BcnCW<<BCN_TCFG_CW_SHIFT;
	BcnTimeCfg |= BcnIFS<<BCN_TCFG_IFS;
	write_nic_word(priv, BCN_TCFG, BcnTimeCfg);

	/* enable the interrupt for ad-hoc process */
	rtl8192_irq_enable(priv);
}

static bool HalRxCheckStuck8190Pci(struct r8192_priv *priv)
{
	u16 RegRxCounter = read_nic_word(priv, 0x130);
	bool				bStuck = FALSE;

	RT_TRACE(COMP_RESET,"%s(): RegRxCounter is %d,RxCounter is %d\n",__FUNCTION__,RegRxCounter,priv->RxCounter);
	// If rssi is small, we should check rx for long time because of bad rx.
	// or maybe it will continuous silent reset every 2 seconds.
	priv->rx_chk_cnt++;
	if(priv->undecorated_smoothed_pwdb >= (RateAdaptiveTH_High+5))
	{
		priv->rx_chk_cnt = 0;	/* high rssi, check rx stuck right now. */
	}
	else if(priv->undecorated_smoothed_pwdb < (RateAdaptiveTH_High+5) &&
		((priv->CurrentChannelBW!=HT_CHANNEL_WIDTH_20&&priv->undecorated_smoothed_pwdb>=RateAdaptiveTH_Low_40M) ||
		(priv->CurrentChannelBW==HT_CHANNEL_WIDTH_20&&priv->undecorated_smoothed_pwdb>=RateAdaptiveTH_Low_20M)) )

	{
		if(priv->rx_chk_cnt < 2)
		{
			return bStuck;
		}
		else
		{
			priv->rx_chk_cnt = 0;
		}
	}
	else if(((priv->CurrentChannelBW!=HT_CHANNEL_WIDTH_20&&priv->undecorated_smoothed_pwdb<RateAdaptiveTH_Low_40M) ||
		(priv->CurrentChannelBW==HT_CHANNEL_WIDTH_20&&priv->undecorated_smoothed_pwdb<RateAdaptiveTH_Low_20M)) &&
		priv->undecorated_smoothed_pwdb >= VeryLowRSSI)
	{
		if(priv->rx_chk_cnt < 4)
		{
			return bStuck;
		}
		else
		{
			priv->rx_chk_cnt = 0;
		}
	}
	else
	{
		if(priv->rx_chk_cnt < 8)
		{
			return bStuck;
		}
		else
		{
			priv->rx_chk_cnt = 0;
		}
	}
	if(priv->RxCounter==RegRxCounter)
		bStuck = TRUE;

	priv->RxCounter = RegRxCounter;

	return bStuck;
}

static RESET_TYPE RxCheckStuck(struct r8192_priv *priv)
{

	if(HalRxCheckStuck8190Pci(priv))
	{
		RT_TRACE(COMP_RESET, "RxStuck Condition\n");
		return RESET_TYPE_SILENT;
	}

	return RESET_TYPE_NORESET;
}

static RESET_TYPE rtl819x_check_reset(struct r8192_priv *priv)
{
	RESET_TYPE RxResetType = RESET_TYPE_NORESET;
	RT_RF_POWER_STATE rfState;

	rfState = priv->eRFPowerState;

	if (rfState != eRfOff && (priv->ieee80211->iw_mode != IW_MODE_ADHOC)) {
		/*
		 * If driver is in the status of firmware download failure,
		 * driver skips RF initialization and RF is in turned off state.
		 * Driver should check whether Rx stuck and do silent reset. And
		 * if driver is in firmware download failure status, driver
		 * should initialize RF in the following silent reset procedure
		 *
		 * Driver should not check RX stuck in IBSS mode because it is
		 * required to set Check BSSID in order to send beacon, however,
		 * if check BSSID is set, STA cannot hear any packet a all.
		 */
		RxResetType = RxCheckStuck(priv);
	}

	RT_TRACE(COMP_RESET, "%s():  RxResetType is %d\n", __FUNCTION__, RxResetType);

	return RxResetType;
}

#ifdef ENABLE_IPS
static void InactivePsWorkItemCallback(struct r8192_priv *priv)
{
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;

	RT_TRACE(COMP_POWER, "InactivePsWorkItemCallback() --------->\n");
	//
	// This flag "bSwRfProcessing", indicates the status of IPS procedure, should be set if the IPS workitem
	// is really scheduled.
	// The old code, sets this flag before scheduling the IPS workitem and however, at the same time the
	// previous IPS workitem did not end yet, fails to schedule the current workitem. Thus, bSwRfProcessing
	// blocks the IPS procedure of switching RF.
	// By Bruce, 2007-12-25.
	//
	pPSC->bSwRfProcessing = TRUE;

	RT_TRACE(COMP_RF, "InactivePsWorkItemCallback(): Set RF to %s.\n",
			pPSC->eInactivePowerState == eRfOff?"OFF":"ON");

	MgntActSet_RF_State(priv, pPSC->eInactivePowerState, RF_CHANGE_BY_IPS);

	//
	// To solve CAM values miss in RF OFF, rewrite CAM values after RF ON. By Bruce, 2007-09-20.
	//
	pPSC->bSwRfProcessing = FALSE;
	RT_TRACE(COMP_POWER, "InactivePsWorkItemCallback() <---------\n");
}

#ifdef ENABLE_LPS
/* Change current and default preamble mode. */
bool MgntActSet_802_11_PowerSaveMode(struct r8192_priv *priv, u8 rtPsMode)
{

	// Currently, we do not change power save mode on IBSS mode.
	if(priv->ieee80211->iw_mode == IW_MODE_ADHOC)
	{
		return false;
	}

	//
	// <RJ_NOTE> If we make HW to fill up the PwrMgt bit for us,
	// some AP will not response to our mgnt frames with PwrMgt bit set,
	// e.g. cannot associate the AP.
	// So I commented out it. 2005.02.16, by rcnjko.
	//
//	// Change device's power save mode.
//	Adapter->HalFunc.SetPSModeHandler( Adapter, rtPsMode );

	// Update power save mode configured.
	//RT_TRACE(COMP_LPS,"%s(): set ieee->ps = %x\n",__FUNCTION__,rtPsMode);
	if(!priv->ps_force) {
		priv->ieee80211->ps = rtPsMode;
	}

	// Awake immediately
	if(priv->ieee80211->sta_sleep != 0 && rtPsMode == IEEE80211_PS_DISABLED)
	{
		// Notify the AP we awke.
		rtl8192_hw_wakeup(priv->ieee80211);
		priv->ieee80211->sta_sleep = 0;

                spin_lock(&priv->ieee80211->mgmt_tx_lock);
		printk("LPS leave: notify AP we are awaked ++++++++++ SendNullFunctionData\n");
		ieee80211_sta_ps_send_null_frame(priv->ieee80211, 0);
                spin_unlock(&priv->ieee80211->mgmt_tx_lock);
	}

	return true;
}

/* Enter the leisure power save mode. */
void LeisurePSEnter(struct ieee80211_device *ieee80211)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;

	if(!((priv->ieee80211->iw_mode == IW_MODE_INFRA) &&
		(priv->ieee80211->state == IEEE80211_LINKED)) ||
		(priv->ieee80211->iw_mode == IW_MODE_ADHOC) ||
		(priv->ieee80211->iw_mode == IW_MODE_MASTER))
		return;

	if (pPSC->bLeisurePs)
	{
		// Idle for a while if we connect to AP a while ago.
		if(pPSC->LpsIdleCount >= RT_CHECK_FOR_HANG_PERIOD) //  4 Sec
		{

			if(priv->ieee80211->ps == IEEE80211_PS_DISABLED)
			{
				MgntActSet_802_11_PowerSaveMode(priv, IEEE80211_PS_MBCAST|IEEE80211_PS_UNICAST);

			}
		}
		else
			pPSC->LpsIdleCount++;
	}
}

/* Leave leisure power save mode. */
void LeisurePSLeave(struct ieee80211_device *ieee80211)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;

	if (pPSC->bLeisurePs)
	{
		if(priv->ieee80211->ps != IEEE80211_PS_DISABLED)
		{
			// move to lps_wakecomplete()
			MgntActSet_802_11_PowerSaveMode(priv, IEEE80211_PS_DISABLED);

		}
	}
}
#endif

/* Enter the inactive power save mode. RF will be off */
void IPSEnter(struct r8192_priv *priv)
{
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;
	RT_RF_POWER_STATE 			rtState;

	if (pPSC->bInactivePs)
	{
		rtState = priv->eRFPowerState;
		//
		// Added by Bruce, 2007-12-25.
		// Do not enter IPS in the following conditions:
		// (1) RF is already OFF or Sleep
		// (2) bSwRfProcessing (indicates the IPS is still under going)
		// (3) Connectted (only disconnected can trigger IPS)
		// (4) IBSS (send Beacon)
		// (5) AP mode (send Beacon)
		//
		if (rtState == eRfOn && !pPSC->bSwRfProcessing
			&& (priv->ieee80211->state != IEEE80211_LINKED) )
		{
			RT_TRACE(COMP_RF,"IPSEnter(): Turn off RF.\n");
			pPSC->eInactivePowerState = eRfOff;
//			queue_work(priv->priv_wq,&(pPSC->InactivePsWorkItem));
			InactivePsWorkItemCallback(priv);
		}
	}
}

//
//	Description:
//		Leave the inactive power save mode, RF will be on.
//	2007.08.17, by shien chang.
//
void IPSLeave(struct r8192_priv *priv)
{
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;
	RT_RF_POWER_STATE 	rtState;

	if (pPSC->bInactivePs)
	{
		rtState = priv->eRFPowerState;
		if (rtState != eRfOn && !pPSC->bSwRfProcessing && priv->RfOffReason <= RF_CHANGE_BY_IPS)
		{
			RT_TRACE(COMP_POWER, "IPSLeave(): Turn on RF.\n");
			pPSC->eInactivePowerState = eRfOn;
			InactivePsWorkItemCallback(priv);
		}
	}
}

void IPSLeave_wq(struct work_struct *work)
{
	struct ieee80211_device *ieee = container_of(work, struct ieee80211_device, ips_leave_wq);
	struct net_device *dev = ieee->dev;

	struct r8192_priv *priv = (struct r8192_priv *)ieee80211_priv(dev);
	down(&priv->ieee80211->ips_sem);
	IPSLeave(priv);
	up(&priv->ieee80211->ips_sem);
}

void ieee80211_ips_leave_wq(struct ieee80211_device *ieee80211)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	RT_RF_POWER_STATE	rtState;
	rtState = priv->eRFPowerState;

	if (priv->PowerSaveControl.bInactivePs){
		if(rtState == eRfOff){
			if(priv->RfOffReason > RF_CHANGE_BY_IPS)
			{
				RT_TRACE(COMP_ERR, "%s(): RF is OFF.\n",__FUNCTION__);
				return;
			}
			else{
				printk("=========>%s(): IPSLeave\n",__FUNCTION__);
				queue_work(priv->ieee80211->wq,&priv->ieee80211->ips_leave_wq);
			}
		}
	}
}
//added by amy 090331 end
void ieee80211_ips_leave(struct ieee80211_device *ieee80211)
{
	struct r8192_priv *priv = ieee80211_priv(ieee80211->dev);
	down(&ieee80211->ips_sem);
	IPSLeave(priv);
	up(&ieee80211->ips_sem);
}
#endif

static void rtl819x_update_rxcounts(
	struct r8192_priv *priv,
	u32* TotalRxBcnNum,
	u32* TotalRxDataNum
)
{
	u16 			SlotIndex;
	u8			i;

	*TotalRxBcnNum = 0;
	*TotalRxDataNum = 0;

	SlotIndex = (priv->ieee80211->LinkDetectInfo.SlotIndex++)%(priv->ieee80211->LinkDetectInfo.SlotNum);
	priv->ieee80211->LinkDetectInfo.RxBcnNum[SlotIndex] = priv->ieee80211->LinkDetectInfo.NumRecvBcnInPeriod;
	priv->ieee80211->LinkDetectInfo.RxDataNum[SlotIndex] = priv->ieee80211->LinkDetectInfo.NumRecvDataInPeriod;
	for( i=0; i<priv->ieee80211->LinkDetectInfo.SlotNum; i++ ){
		*TotalRxBcnNum += priv->ieee80211->LinkDetectInfo.RxBcnNum[i];
		*TotalRxDataNum += priv->ieee80211->LinkDetectInfo.RxDataNum[i];
	}
}

static void rtl819x_watchdog_wqcallback(struct work_struct *work)
{
	struct delayed_work *dwork = container_of(work,struct delayed_work,work);
       struct r8192_priv *priv = container_of(dwork,struct r8192_priv,watch_dog_wq);
	struct ieee80211_device* ieee = priv->ieee80211;
	RESET_TYPE	ResetType = RESET_TYPE_NORESET;
	bool bBusyTraffic = false;
	bool bEnterPS = false;

	if ((!priv->up) || priv->bHwRadioOff)
		return;

	if(!priv->up)
		return;
	hal_dm_watchdog(priv);
#ifdef ENABLE_IPS
	if(ieee->actscanning == false){
		if((ieee->iw_mode == IW_MODE_INFRA) && (ieee->state == IEEE80211_NOLINK) &&
		    (priv->eRFPowerState == eRfOn) && !ieee->is_set_key &&
		    (!ieee->proto_stoppping) && !ieee->wx_set_enc){
			if (priv->PowerSaveControl.ReturnPoint == IPS_CALLBACK_NONE){
				IPSEnter(priv);
			}
		}
	}
#endif
	{//to get busy traffic condition
		if(ieee->state == IEEE80211_LINKED)
		{
			if(	ieee->LinkDetectInfo.NumRxOkInPeriod> 100 ||
				ieee->LinkDetectInfo.NumTxOkInPeriod> 100 ) {
				bBusyTraffic = true;
			}

#ifdef ENABLE_LPS
			//added by amy for Leisure PS
			if(	((ieee->LinkDetectInfo.NumRxUnicastOkInPeriod + ieee->LinkDetectInfo.NumTxOkInPeriod) > 8 ) ||
				(ieee->LinkDetectInfo.NumRxUnicastOkInPeriod > 2) )
			{
				bEnterPS= false;
			}
			else
			{
				bEnterPS= true;
			}

			// LeisurePS only work in infra mode.
			if(bEnterPS)
			{
				LeisurePSEnter(priv->ieee80211);
			}
			else
			{
				LeisurePSLeave(priv->ieee80211);
			}
#endif

		}
		else
		{
#ifdef ENABLE_LPS
			LeisurePSLeave(priv->ieee80211);
#endif
		}

	        ieee->LinkDetectInfo.NumRxOkInPeriod = 0;
	        ieee->LinkDetectInfo.NumTxOkInPeriod = 0;
		ieee->LinkDetectInfo.NumRxUnicastOkInPeriod = 0;
		ieee->LinkDetectInfo.bBusyTraffic = bBusyTraffic;
	}

	//added by amy for AP roaming
		if(ieee->state == IEEE80211_LINKED && ieee->iw_mode == IW_MODE_INFRA)
		{
			u32	TotalRxBcnNum = 0;
			u32	TotalRxDataNum = 0;

			rtl819x_update_rxcounts(priv, &TotalRxBcnNum, &TotalRxDataNum);
			if((TotalRxBcnNum+TotalRxDataNum) == 0)
			{
				if (priv->eRFPowerState == eRfOff)
					RT_TRACE(COMP_ERR,"========>%s()\n",__FUNCTION__);
				printk("===>%s(): AP is power off,connect another one\n",__FUNCTION__);
				//		Dot11d_Reset(dev);
				ieee->state = IEEE80211_ASSOCIATING;
				notify_wx_assoc_event(priv->ieee80211);
				RemovePeerTS(priv->ieee80211,priv->ieee80211->current_network.bssid);
				ieee->is_roaming = true;
				ieee->is_set_key = false;
				ieee->link_change(ieee);
				queue_work(ieee->wq, &ieee->associate_procedure_wq);
			}
		}
	      ieee->LinkDetectInfo.NumRecvBcnInPeriod=0;
              ieee->LinkDetectInfo.NumRecvDataInPeriod=0;

	//check if reset the driver
	if (priv->watchdog_check_reset_cnt++ >= 3 && !ieee->is_roaming &&
	    priv->watchdog_last_time != 1)
	{
		ResetType = rtl819x_check_reset(priv);
		priv->watchdog_check_reset_cnt = 3;
	}
	if(!priv->bDisableNormalResetCheck && ResetType == RESET_TYPE_NORMAL)
	{
		priv->ResetProgress = RESET_TYPE_NORMAL;
		RT_TRACE(COMP_RESET,"%s(): NOMAL RESET\n",__FUNCTION__);
		return;
	}
	/* disable silent reset temply 2008.9.11*/

	if( ((priv->force_reset) || (!priv->bDisableNormalResetCheck && ResetType==RESET_TYPE_SILENT))) // This is control by OID set in Pomelo
	{
		priv->watchdog_last_time = 1;
	}
	else
		priv->watchdog_last_time = 0;

	priv->force_reset = false;
	priv->bForcedSilentReset = false;
	priv->bResetInProgress = false;
	RT_TRACE(COMP_TRACE, " <==RtUsbCheckForHangWorkItemCallback()\n");

}

void watch_dog_timer_callback(unsigned long data)
{
	struct r8192_priv *priv = (struct r8192_priv *) data;
	queue_delayed_work(priv->priv_wq,&priv->watch_dog_wq,0);
	mod_timer(&priv->watch_dog_timer, jiffies + MSECS(IEEE80211_WATCH_DOG_TIME));

}

static int _rtl8192_up(struct r8192_priv *priv)
{
	RT_STATUS init_status = RT_STATUS_SUCCESS;
	struct net_device *dev = priv->ieee80211->dev;

	priv->up=1;
	priv->ieee80211->ieee_up=1;
	priv->bdisable_nic = false;  //YJ,add,091111
	RT_TRACE(COMP_INIT, "Bringing up iface\n");

	init_status = rtl8192_adapter_start(priv);
	if(init_status != RT_STATUS_SUCCESS)
	{
		RT_TRACE(COMP_ERR,"ERR!!! %s(): initialization is failed!\n",__FUNCTION__);
		return -1;
	}
	RT_TRACE(COMP_INIT, "start adapter finished\n");

	if (priv->eRFPowerState != eRfOn)
		MgntActSet_RF_State(priv, eRfOn, priv->RfOffReason);

	if(priv->ieee80211->state != IEEE80211_LINKED)
	ieee80211_softmac_start_protocol(priv->ieee80211);
	ieee80211_reset_queue(priv->ieee80211);
	watch_dog_timer_callback((unsigned long) priv);
	if(!netif_queue_stopped(dev))
		netif_start_queue(dev);
	else
		netif_wake_queue(dev);

	return 0;
}

static int rtl8192_open(struct net_device *dev)
{
	struct r8192_priv *priv = ieee80211_priv(dev);
	int ret;

	down(&priv->wx_sem);
	ret = rtl8192_up(dev);
	up(&priv->wx_sem);
	return ret;

}

int rtl8192_up(struct net_device *dev)
{
	struct r8192_priv *priv = ieee80211_priv(dev);

	if (priv->up == 1) return -1;

	return _rtl8192_up(priv);
}

static int rtl8192_close(struct net_device *dev)
{
	struct r8192_priv *priv = ieee80211_priv(dev);
	int ret;

	down(&priv->wx_sem);

	ret = rtl8192_down(dev);

	up(&priv->wx_sem);

	return ret;

}

int rtl8192_down(struct net_device *dev)
{
	struct r8192_priv *priv = ieee80211_priv(dev);

	if (priv->up == 0) return -1;

#ifdef ENABLE_LPS
	//LZM for PS-Poll AID issue. 090429
	if(priv->ieee80211->state == IEEE80211_LINKED)
		LeisurePSLeave(priv->ieee80211);
#endif

	priv->up=0;
	priv->ieee80211->ieee_up = 0;
	RT_TRACE(COMP_DOWN, "==========>%s()\n", __FUNCTION__);
/* FIXME */
	if (!netif_queue_stopped(dev))
		netif_stop_queue(dev);

	rtl8192_irq_disable(priv);
	rtl8192_cancel_deferred_work(priv);
	deinit_hal_dm(priv);
	del_timer_sync(&priv->watch_dog_timer);

	ieee80211_softmac_stop_protocol(priv->ieee80211,true);

	rtl8192_halt_adapter(priv, false);
	memset(&priv->ieee80211->current_network, 0 , offsetof(struct ieee80211_network, list));

	RT_TRACE(COMP_DOWN, "<==========%s()\n", __FUNCTION__);

	return 0;
}

void rtl8192_commit(struct r8192_priv *priv)
{
	if (priv->up == 0) return ;

	ieee80211_softmac_stop_protocol(priv->ieee80211,true);

	rtl8192_irq_disable(priv);
	rtl8192_halt_adapter(priv, true);
	_rtl8192_up(priv);
}

static void rtl8192_restart(struct work_struct *work)
{
        struct r8192_priv *priv = container_of(work, struct r8192_priv, reset_wq);

	down(&priv->wx_sem);

	rtl8192_commit(priv);

	up(&priv->wx_sem);
}

static void r8192_set_multicast(struct net_device *dev)
{
	struct r8192_priv *priv = ieee80211_priv(dev);

	priv->promisc = (dev->flags & IFF_PROMISC) ? 1 : 0;
}

static int r8192_set_mac_adr(struct net_device *dev, void *mac)
{
	struct r8192_priv *priv = ieee80211_priv(dev);
	struct sockaddr *addr = mac;

	down(&priv->wx_sem);

	memcpy(dev->dev_addr, addr->sa_data, ETH_ALEN);

	schedule_work(&priv->reset_wq);
	up(&priv->wx_sem);

	return 0;
}

static void r8192e_set_hw_key(struct r8192_priv *priv, struct ieee_param *ipw)
{
	struct ieee80211_device *ieee = priv->ieee80211;
	u8 broadcast_addr[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
	u32 key[4];

	if (ipw->u.crypt.set_tx) {
		if (strcmp(ipw->u.crypt.alg, "CCMP") == 0)
			ieee->pairwise_key_type = KEY_TYPE_CCMP;
		else if (strcmp(ipw->u.crypt.alg, "TKIP") == 0)
			ieee->pairwise_key_type = KEY_TYPE_TKIP;
		else if (strcmp(ipw->u.crypt.alg, "WEP") == 0) {
			if (ipw->u.crypt.key_len == 13)
				ieee->pairwise_key_type = KEY_TYPE_WEP104;
			else if (ipw->u.crypt.key_len == 5)
				ieee->pairwise_key_type = KEY_TYPE_WEP40;
		} else
			ieee->pairwise_key_type = KEY_TYPE_NA;

		if (ieee->pairwise_key_type) {
			memcpy(key, ipw->u.crypt.key, 16);
			EnableHWSecurityConfig8192(priv);
			/*
			 * We fill both index entry and 4th entry for pairwise
			 * key as in IPW interface, adhoc will only get here,
			 * so we need index entry for its default key serching!
			 */
			setKey(priv, 4, ipw->u.crypt.idx,
			       ieee->pairwise_key_type,
			       (u8*)ieee->ap_mac_addr, 0, key);

			/* LEAP WEP will never set this. */
			if (ieee->auth_mode != 2)
				setKey(priv, ipw->u.crypt.idx, ipw->u.crypt.idx,
				       ieee->pairwise_key_type,
				       (u8*)ieee->ap_mac_addr, 0, key);
		}
		if ((ieee->pairwise_key_type == KEY_TYPE_CCMP) &&
		    ieee->pHTInfo->bCurrentHTSupport) {
			write_nic_byte(priv, 0x173, 1); /* fix aes bug */
		}
	} else {
		memcpy(key, ipw->u.crypt.key, 16);
		if (strcmp(ipw->u.crypt.alg, "CCMP") == 0)
			ieee->group_key_type= KEY_TYPE_CCMP;
		else if (strcmp(ipw->u.crypt.alg, "TKIP") == 0)
			ieee->group_key_type = KEY_TYPE_TKIP;
		else if (strcmp(ipw->u.crypt.alg, "WEP") == 0) {
			if (ipw->u.crypt.key_len == 13)
				ieee->group_key_type = KEY_TYPE_WEP104;
			else if (ipw->u.crypt.key_len == 5)
				ieee->group_key_type = KEY_TYPE_WEP40;
		} else
			ieee->group_key_type = KEY_TYPE_NA;

		if (ieee->group_key_type) {
			setKey(priv, ipw->u.crypt.idx, ipw->u.crypt.idx,
			       ieee->group_key_type, broadcast_addr, 0, key);
		}
	}
}

/* based on ipw2200 driver */
static int rtl8192_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct r8192_priv *priv = (struct r8192_priv *)ieee80211_priv(dev);
	struct iwreq *wrq = (struct iwreq *)rq;
	int ret=-1;
	struct iw_point *p = &wrq->u.data;
	struct ieee_param *ipw = NULL;//(struct ieee_param *)wrq->u.data.pointer;

	down(&priv->wx_sem);

     if (p->length < sizeof(struct ieee_param) || !p->pointer){
             ret = -EINVAL;
             goto out;
     }

     ipw = kmalloc(p->length, GFP_KERNEL);
     if (ipw == NULL){
             ret = -ENOMEM;
             goto out;
     }
     if (copy_from_user(ipw, p->pointer, p->length)) {
            kfree(ipw);
            ret = -EFAULT;
            goto out;
     }

	switch (cmd) {
	case RTL_IOCTL_WPA_SUPPLICANT:
		/* parse here for HW security */
		if (ipw->cmd == IEEE_CMD_SET_ENCRYPTION)
			r8192e_set_hw_key(priv, ipw);
		ret = ieee80211_wpa_supplicant_ioctl(priv->ieee80211, &wrq->u.data);
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	kfree(ipw);
out:
	up(&priv->wx_sem);

	return ret;
}

static u8 HwRateToMRate90(bool bIsHT, u8 rate)
{
	u8  ret_rate = 0x02;

	if(!bIsHT) {
		switch(rate) {
			case DESC90_RATE1M:   ret_rate = MGN_1M;         break;
			case DESC90_RATE2M:   ret_rate = MGN_2M;         break;
			case DESC90_RATE5_5M: ret_rate = MGN_5_5M;       break;
			case DESC90_RATE11M:  ret_rate = MGN_11M;        break;
			case DESC90_RATE6M:   ret_rate = MGN_6M;         break;
			case DESC90_RATE9M:   ret_rate = MGN_9M;         break;
			case DESC90_RATE12M:  ret_rate = MGN_12M;        break;
			case DESC90_RATE18M:  ret_rate = MGN_18M;        break;
			case DESC90_RATE24M:  ret_rate = MGN_24M;        break;
			case DESC90_RATE36M:  ret_rate = MGN_36M;        break;
			case DESC90_RATE48M:  ret_rate = MGN_48M;        break;
			case DESC90_RATE54M:  ret_rate = MGN_54M;        break;

			default:
					      RT_TRACE(COMP_RECV, "HwRateToMRate90(): Non supported Rate [%x], bIsHT = %d!!!\n", rate, bIsHT);
					      break;
		}

	} else {
		switch(rate) {
			case DESC90_RATEMCS0:   ret_rate = MGN_MCS0;    break;
			case DESC90_RATEMCS1:   ret_rate = MGN_MCS1;    break;
			case DESC90_RATEMCS2:   ret_rate = MGN_MCS2;    break;
			case DESC90_RATEMCS3:   ret_rate = MGN_MCS3;    break;
			case DESC90_RATEMCS4:   ret_rate = MGN_MCS4;    break;
			case DESC90_RATEMCS5:   ret_rate = MGN_MCS5;    break;
			case DESC90_RATEMCS6:   ret_rate = MGN_MCS6;    break;
			case DESC90_RATEMCS7:   ret_rate = MGN_MCS7;    break;
			case DESC90_RATEMCS8:   ret_rate = MGN_MCS8;    break;
			case DESC90_RATEMCS9:   ret_rate = MGN_MCS9;    break;
			case DESC90_RATEMCS10:  ret_rate = MGN_MCS10;   break;
			case DESC90_RATEMCS11:  ret_rate = MGN_MCS11;   break;
			case DESC90_RATEMCS12:  ret_rate = MGN_MCS12;   break;
			case DESC90_RATEMCS13:  ret_rate = MGN_MCS13;   break;
			case DESC90_RATEMCS14:  ret_rate = MGN_MCS14;   break;
			case DESC90_RATEMCS15:  ret_rate = MGN_MCS15;   break;
			case DESC90_RATEMCS32:  ret_rate = (0x80|0x20); break;

			default:
						RT_TRACE(COMP_RECV, "HwRateToMRate90(): Non supported Rate [%x], bIsHT = %d!!!\n",rate, bIsHT);
						break;
		}
	}

	return ret_rate;
}

/* Record the TSF time stamp when receiving a packet */
static void UpdateRxPktTimeStamp8190(struct r8192_priv *priv, struct ieee80211_rx_stats *stats)
{

	if(stats->bIsAMPDU && !stats->bFirstMPDU) {
		stats->mac_time[0] = priv->LastRxDescTSFLow;
		stats->mac_time[1] = priv->LastRxDescTSFHigh;
	} else {
		priv->LastRxDescTSFLow = stats->mac_time[0];
		priv->LastRxDescTSFHigh = stats->mac_time[1];
	}
}

static long rtl819x_translate_todbm(u8 signal_strength_index)// 0-100 index.
{
	long	signal_power; // in dBm.

	// Translate to dBm (x=0.5y-95).
	signal_power = (long)((signal_strength_index + 1) >> 1);
	signal_power -= 95;

	return signal_power;
}

/* 2008/01/22 MH We can not delcare RSSI/EVM total value of sliding window to
	be a local static. Otherwise, it may increase when we return from S3/S4. The
	value will be kept in memory or disk. We must delcare the value in adapter
	and it will be reinitialized when return from S3/S4. */
static void rtl8192_process_phyinfo(struct r8192_priv * priv, u8* buffer,struct ieee80211_rx_stats * pprevious_stats, struct ieee80211_rx_stats * pcurrent_stats)
{
	bool bcheck = false;
	u8	rfpath;
	u32 nspatial_stream, tmp_val;
	static u32 slide_rssi_index=0, slide_rssi_statistics=0;
	static u32 slide_evm_index=0, slide_evm_statistics=0;
	static u32 last_rssi=0, last_evm=0;
	//cosa add for beacon rssi smoothing
	static u32 slide_beacon_adc_pwdb_index=0, slide_beacon_adc_pwdb_statistics=0;
	static u32 last_beacon_adc_pwdb=0;

	struct ieee80211_hdr_3addr *hdr;
	u16 sc ;
	unsigned int frag,seq;
	hdr = (struct ieee80211_hdr_3addr *)buffer;
	sc = le16_to_cpu(hdr->seq_ctl);
	frag = WLAN_GET_SEQ_FRAG(sc);
	seq = WLAN_GET_SEQ_SEQ(sc);

	//
	// Check whether we should take the previous packet into accounting
	//
	if(!pprevious_stats->bIsAMPDU)
	{
		// if previous packet is not aggregated packet
		bcheck = true;
	}

	if(slide_rssi_statistics++ >= PHY_RSSI_SLID_WIN_MAX)
	{
		slide_rssi_statistics = PHY_RSSI_SLID_WIN_MAX;
		last_rssi = priv->stats.slide_signal_strength[slide_rssi_index];
		priv->stats.slide_rssi_total -= last_rssi;
	}
	priv->stats.slide_rssi_total += pprevious_stats->SignalStrength;

	priv->stats.slide_signal_strength[slide_rssi_index++] = pprevious_stats->SignalStrength;
	if(slide_rssi_index >= PHY_RSSI_SLID_WIN_MAX)
		slide_rssi_index = 0;

	// <1> Showed on UI for user, in dbm
	tmp_val = priv->stats.slide_rssi_total/slide_rssi_statistics;
	priv->stats.signal_strength = rtl819x_translate_todbm((u8)tmp_val);
	pcurrent_stats->rssi = priv->stats.signal_strength;
	//
	// If the previous packet does not match the criteria, neglect it
	//
	if(!pprevious_stats->bPacketMatchBSSID)
	{
		if(!pprevious_stats->bToSelfBA)
			return;
	}

	if(!bcheck)
		return;

	// <2> Showed on UI for engineering
	// hardware does not provide rssi information for each rf path in CCK
	if(!pprevious_stats->bIsCCK && pprevious_stats->bPacketToSelf)
	{
		for (rfpath = RF90_PATH_A; rfpath < RF90_PATH_C; rfpath++)
		{
			if (!rtl8192_phy_CheckIsLegalRFPath(priv, rfpath))
				continue;
			RT_TRACE(COMP_DBG, "pPreviousstats->RxMIMOSignalStrength[rfpath] = %d\n", pprevious_stats->RxMIMOSignalStrength[rfpath]);
			//Fixed by Jacken 2008-03-20
			if(priv->stats.rx_rssi_percentage[rfpath] == 0)
			{
				priv->stats.rx_rssi_percentage[rfpath] = pprevious_stats->RxMIMOSignalStrength[rfpath];
			}
			if(pprevious_stats->RxMIMOSignalStrength[rfpath]  > priv->stats.rx_rssi_percentage[rfpath])
			{
				priv->stats.rx_rssi_percentage[rfpath] =
					( (priv->stats.rx_rssi_percentage[rfpath]*(Rx_Smooth_Factor-1)) +
					(pprevious_stats->RxMIMOSignalStrength[rfpath])) /(Rx_Smooth_Factor);
				priv->stats.rx_rssi_percentage[rfpath] = priv->stats.rx_rssi_percentage[rfpath]  + 1;
			}
			else
			{
				priv->stats.rx_rssi_percentage[rfpath] =
					( (priv->stats.rx_rssi_percentage[rfpath]*(Rx_Smooth_Factor-1)) +
					(pprevious_stats->RxMIMOSignalStrength[rfpath])) /(Rx_Smooth_Factor);
			}
			RT_TRACE(COMP_DBG, "priv->RxStats.RxRSSIPercentage[rfPath] = %d \n" , priv->stats.rx_rssi_percentage[rfpath]);
		}
	}

	//
	// Check PWDB.
	//
	//cosa add for beacon rssi smoothing by average.
	if(pprevious_stats->bPacketBeacon)
	{
		/* record the beacon pwdb to the sliding window. */
		if(slide_beacon_adc_pwdb_statistics++ >= PHY_Beacon_RSSI_SLID_WIN_MAX)
		{
			slide_beacon_adc_pwdb_statistics = PHY_Beacon_RSSI_SLID_WIN_MAX;
			last_beacon_adc_pwdb = priv->stats.Slide_Beacon_pwdb[slide_beacon_adc_pwdb_index];
			priv->stats.Slide_Beacon_Total -= last_beacon_adc_pwdb;
			//	slide_beacon_adc_pwdb_index, last_beacon_adc_pwdb, Adapter->RxStats.Slide_Beacon_Total);
		}
		priv->stats.Slide_Beacon_Total += pprevious_stats->RxPWDBAll;
		priv->stats.Slide_Beacon_pwdb[slide_beacon_adc_pwdb_index] = pprevious_stats->RxPWDBAll;
		slide_beacon_adc_pwdb_index++;
		if(slide_beacon_adc_pwdb_index >= PHY_Beacon_RSSI_SLID_WIN_MAX)
			slide_beacon_adc_pwdb_index = 0;
		pprevious_stats->RxPWDBAll = priv->stats.Slide_Beacon_Total/slide_beacon_adc_pwdb_statistics;
		if(pprevious_stats->RxPWDBAll >= 3)
			pprevious_stats->RxPWDBAll -= 3;
	}

	RT_TRACE(COMP_RXDESC, "Smooth %s PWDB = %d\n",
				pprevious_stats->bIsCCK? "CCK": "OFDM",
				pprevious_stats->RxPWDBAll);

	if(pprevious_stats->bPacketToSelf || pprevious_stats->bPacketBeacon || pprevious_stats->bToSelfBA)
	{
		if(priv->undecorated_smoothed_pwdb < 0)	// initialize
		{
			priv->undecorated_smoothed_pwdb = pprevious_stats->RxPWDBAll;
		}

		if(pprevious_stats->RxPWDBAll > (u32)priv->undecorated_smoothed_pwdb)
		{
			priv->undecorated_smoothed_pwdb =
					( ((priv->undecorated_smoothed_pwdb)*(Rx_Smooth_Factor-1)) +
					(pprevious_stats->RxPWDBAll)) /(Rx_Smooth_Factor);
			priv->undecorated_smoothed_pwdb = priv->undecorated_smoothed_pwdb + 1;
		}
		else
		{
			priv->undecorated_smoothed_pwdb =
					( ((priv->undecorated_smoothed_pwdb)*(Rx_Smooth_Factor-1)) +
					(pprevious_stats->RxPWDBAll)) /(Rx_Smooth_Factor);
		}
	}

	//
	// Check EVM
	//
	/* record the general EVM to the sliding window. */
	if(pprevious_stats->SignalQuality == 0)
	{
	}
	else
	{
		if(pprevious_stats->bPacketToSelf || pprevious_stats->bPacketBeacon || pprevious_stats->bToSelfBA){
			if(slide_evm_statistics++ >= PHY_RSSI_SLID_WIN_MAX){
				slide_evm_statistics = PHY_RSSI_SLID_WIN_MAX;
				last_evm = priv->stats.slide_evm[slide_evm_index];
				priv->stats.slide_evm_total -= last_evm;
			}

			priv->stats.slide_evm_total += pprevious_stats->SignalQuality;

			priv->stats.slide_evm[slide_evm_index++] = pprevious_stats->SignalQuality;
			if(slide_evm_index >= PHY_RSSI_SLID_WIN_MAX)
				slide_evm_index = 0;

			// <1> Showed on UI for user, in percentage.
			tmp_val = priv->stats.slide_evm_total/slide_evm_statistics;
			//cosa add 10/11/2007, Showed on UI for user in Windows Vista, for Link quality.
		}

		// <2> Showed on UI for engineering
		if(pprevious_stats->bPacketToSelf || pprevious_stats->bPacketBeacon || pprevious_stats->bToSelfBA)
		{
			for(nspatial_stream = 0; nspatial_stream<2 ; nspatial_stream++) // 2 spatial stream
			{
				if(pprevious_stats->RxMIMOSignalQuality[nspatial_stream] != -1)
				{
					if(priv->stats.rx_evm_percentage[nspatial_stream] == 0)	// initialize
					{
						priv->stats.rx_evm_percentage[nspatial_stream] = pprevious_stats->RxMIMOSignalQuality[nspatial_stream];
					}
					priv->stats.rx_evm_percentage[nspatial_stream] =
						( (priv->stats.rx_evm_percentage[nspatial_stream]* (Rx_Smooth_Factor-1)) +
						(pprevious_stats->RxMIMOSignalQuality[nspatial_stream]* 1)) / (Rx_Smooth_Factor);
				}
			}
		}
	}

}

static u8 rtl819x_query_rxpwrpercentage(
	char		antpower
	)
{
	if ((antpower <= -100) || (antpower >= 20))
	{
		return	0;
	}
	else if (antpower >= 0)
	{
		return	100;
	}
	else
	{
		return	(100+antpower);
	}

}

static u8
rtl819x_evm_dbtopercentage(
	char value
	)
{
	char ret_val;

	ret_val = value;

	if(ret_val >= 0)
		ret_val = 0;
	if(ret_val <= -33)
		ret_val = -33;
	ret_val = 0 - ret_val;
	ret_val*=3;
	if(ret_val == 99)
		ret_val = 100;
	return ret_val;
}

/* We want good-looking for signal strength/quality */
static long rtl819x_signal_scale_mapping(long currsig)
{
	long retsig;

	// Step 1. Scale mapping.
	if(currsig >= 61 && currsig <= 100)
	{
		retsig = 90 + ((currsig - 60) / 4);
	}
	else if(currsig >= 41 && currsig <= 60)
	{
		retsig = 78 + ((currsig - 40) / 2);
	}
	else if(currsig >= 31 && currsig <= 40)
	{
		retsig = 66 + (currsig - 30);
	}
	else if(currsig >= 21 && currsig <= 30)
	{
		retsig = 54 + (currsig - 20);
	}
	else if(currsig >= 5 && currsig <= 20)
	{
		retsig = 42 + (((currsig - 5) * 2) / 3);
	}
	else if(currsig == 4)
	{
		retsig = 36;
	}
	else if(currsig == 3)
	{
		retsig = 27;
	}
	else if(currsig == 2)
	{
		retsig = 18;
	}
	else if(currsig == 1)
	{
		retsig = 9;
	}
	else
	{
		retsig = currsig;
	}

	return retsig;
}

static void rtl8192_query_rxphystatus(
	struct r8192_priv * priv,
	struct ieee80211_rx_stats * pstats,
	prx_desc_819x_pci  pdesc,
	prx_fwinfo_819x_pci   pdrvinfo,
	struct ieee80211_rx_stats * precord_stats,
	bool bpacket_match_bssid,
	bool bpacket_toself,
	bool bPacketBeacon,
	bool bToSelfBA
	)
{
	//PRT_RFD_STATUS		pRtRfdStatus = &(pRfd->Status);
	phy_sts_ofdm_819xpci_t* pofdm_buf;
	phy_sts_cck_819xpci_t	*	pcck_buf;
	phy_ofdm_rx_status_rxsc_sgien_exintfflag* prxsc;
	u8				*prxpkt;
	u8				i,max_spatial_stream, tmp_rxsnr, tmp_rxevm, rxsc_sgien_exflg;
	char				rx_pwr[4], rx_pwr_all=0;
	//long				rx_avg_pwr = 0;
	char				rx_snrX, rx_evmX;
	u8				evm, pwdb_all;
	u32 			RSSI, total_rssi=0;//, total_evm=0;
//	long				signal_strength_index = 0;
	u8				is_cck_rate=0;
	u8				rf_rx_num = 0;

	is_cck_rate = rx_hal_is_cck_rate(pdrvinfo);

	// Record it for next packet processing
	memset(precord_stats, 0, sizeof(struct ieee80211_rx_stats));
	pstats->bPacketMatchBSSID = precord_stats->bPacketMatchBSSID = bpacket_match_bssid;
	pstats->bPacketToSelf = precord_stats->bPacketToSelf = bpacket_toself;
	pstats->bIsCCK = precord_stats->bIsCCK = is_cck_rate;//RX_HAL_IS_CCK_RATE(pDrvInfo);
	pstats->bPacketBeacon = precord_stats->bPacketBeacon = bPacketBeacon;
	pstats->bToSelfBA = precord_stats->bToSelfBA = bToSelfBA;
	/*2007.08.30 requested by SD3 Jerry */
	if (priv->phy_check_reg824 == 0)
	{
		priv->phy_reg824_bit9 = rtl8192_QueryBBReg(priv, rFPGA0_XA_HSSIParameter2, 0x200);
		priv->phy_check_reg824 = 1;
	}

	prxpkt = (u8*)pdrvinfo;

	/* Move pointer to the 16th bytes. Phy status start address. */
	prxpkt += sizeof(rx_fwinfo_819x_pci);

	/* Initial the cck and ofdm buffer pointer */
	pcck_buf = (phy_sts_cck_819xpci_t *)prxpkt;
	pofdm_buf = (phy_sts_ofdm_819xpci_t *)prxpkt;

	pstats->RxMIMOSignalQuality[0] = -1;
	pstats->RxMIMOSignalQuality[1] = -1;
	precord_stats->RxMIMOSignalQuality[0] = -1;
	precord_stats->RxMIMOSignalQuality[1] = -1;

	if(is_cck_rate)
	{
		//
		// (1)Hardware does not provide RSSI for CCK
		//

		//
		// (2)PWDB, Average PWDB cacluated by hardware (for rate adaptive)
		//
		u8 report;//, cck_agc_rpt;

		if (!priv->phy_reg824_bit9)
		{
			report = pcck_buf->cck_agc_rpt & 0xc0;
			report = report>>6;
			switch(report)
			{
				//Fixed by Jacken from Bryant 2008-03-20
				//Original value is -38 , -26 , -14 , -2
				//Fixed value is -35 , -23 , -11 , 6
				case 0x3:
					rx_pwr_all = -35 - (pcck_buf->cck_agc_rpt & 0x3e);
					break;
				case 0x2:
					rx_pwr_all = -23 - (pcck_buf->cck_agc_rpt & 0x3e);
					break;
				case 0x1:
					rx_pwr_all = -11 - (pcck_buf->cck_agc_rpt & 0x3e);
					break;
				case 0x0:
					rx_pwr_all = 8 - (pcck_buf->cck_agc_rpt & 0x3e);
					break;
			}
		}
		else
		{
			report = pcck_buf->cck_agc_rpt & 0x60;
			report = report>>5;
			switch(report)
			{
				case 0x3:
					rx_pwr_all = -35 - ((pcck_buf->cck_agc_rpt & 0x1f)<<1) ;
					break;
				case 0x2:
					rx_pwr_all = -23 - ((pcck_buf->cck_agc_rpt & 0x1f)<<1);
					break;
				case 0x1:
					rx_pwr_all = -11 - ((pcck_buf->cck_agc_rpt & 0x1f)<<1) ;
					break;
				case 0x0:
					rx_pwr_all = -8 - ((pcck_buf->cck_agc_rpt & 0x1f)<<1) ;
					break;
			}
		}

		pwdb_all = rtl819x_query_rxpwrpercentage(rx_pwr_all);
		pstats->RxPWDBAll = precord_stats->RxPWDBAll = pwdb_all;
		pstats->RecvSignalPower = rx_pwr_all;

		//
		// (3) Get Signal Quality (EVM)
		//
		if(bpacket_match_bssid)
		{
			u8	sq;

			if(pstats->RxPWDBAll > 40)
			{
				sq = 100;
			}else
			{
				sq = pcck_buf->sq_rpt;

				if(pcck_buf->sq_rpt > 64)
					sq = 0;
				else if (pcck_buf->sq_rpt < 20)
					sq = 100;
				else
					sq = ((64-sq) * 100) / 44;
			}
			pstats->SignalQuality = precord_stats->SignalQuality = sq;
			pstats->RxMIMOSignalQuality[0] = precord_stats->RxMIMOSignalQuality[0] = sq;
			pstats->RxMIMOSignalQuality[1] = precord_stats->RxMIMOSignalQuality[1] = -1;
		}
	}
	else
	{
		//
		// (1)Get RSSI for HT rate
		//
		for(i=RF90_PATH_A; i<RF90_PATH_MAX; i++)
		{
			// 2008/01/30 MH we will judge RF RX path now.
			if (priv->brfpath_rxenable[i])
				rf_rx_num++;
			//else
				//continue;

			//Fixed by Jacken from Bryant 2008-03-20
			//Original value is 106
			rx_pwr[i] = ((pofdm_buf->trsw_gain_X[i]&0x3F)*2) - 110;

			//Get Rx snr value in DB
			tmp_rxsnr = pofdm_buf->rxsnr_X[i];
			rx_snrX = (char)(tmp_rxsnr);
			rx_snrX /= 2;

			/* Translate DBM to percentage. */
			RSSI = rtl819x_query_rxpwrpercentage(rx_pwr[i]);
			if (priv->brfpath_rxenable[i])
				total_rssi += RSSI;

			/* Record Signal Strength for next packet */
			if(bpacket_match_bssid)
			{
				pstats->RxMIMOSignalStrength[i] =(u8) RSSI;
				precord_stats->RxMIMOSignalStrength[i] =(u8) RSSI;
			}
		}

		//
		// (2)PWDB, Average PWDB cacluated by hardware (for rate adaptive)
		//
		//Fixed by Jacken from Bryant 2008-03-20
		//Original value is 106
		rx_pwr_all = (((pofdm_buf->pwdb_all ) >> 1 )& 0x7f) -106;
		pwdb_all = rtl819x_query_rxpwrpercentage(rx_pwr_all);

		pstats->RxPWDBAll = precord_stats->RxPWDBAll = pwdb_all;
		pstats->RxPower = precord_stats->RxPower =	rx_pwr_all;
		pstats->RecvSignalPower = rx_pwr_all;
		//
		// (3)EVM of HT rate
		//
		if(pdrvinfo->RxHT && pdrvinfo->RxRate>=DESC90_RATEMCS8 &&
			pdrvinfo->RxRate<=DESC90_RATEMCS15)
			max_spatial_stream = 2; //both spatial stream make sense
		else
			max_spatial_stream = 1; //only spatial stream 1 makes sense

		for(i=0; i<max_spatial_stream; i++)
		{
			tmp_rxevm = pofdm_buf->rxevm_X[i];
			rx_evmX = (char)(tmp_rxevm);

			// Do not use shift operation like "rx_evmX >>= 1" because the compilor of free build environment
			// fill most significant bit to "zero" when doing shifting operation which may change a negative
			// value to positive one, then the dbm value (which is supposed to be negative)  is not correct anymore.
			rx_evmX /= 2;	//dbm

			evm = rtl819x_evm_dbtopercentage(rx_evmX);
			if(bpacket_match_bssid)
			{
				if(i==0) // Fill value in RFD, Get the first spatial stream only
					pstats->SignalQuality = precord_stats->SignalQuality = (u8)(evm & 0xff);
				pstats->RxMIMOSignalQuality[i] = precord_stats->RxMIMOSignalQuality[i] = (u8)(evm & 0xff);
			}
		}

		/* record rx statistics for debug */
		rxsc_sgien_exflg = pofdm_buf->rxsc_sgien_exflg;
		prxsc = (phy_ofdm_rx_status_rxsc_sgien_exintfflag *)&rxsc_sgien_exflg;
	}

	//UI BSS List signal strength(in percentage), make it good looking, from 0~100.
	//It is assigned to the BSS List in GetValueFromBeaconOrProbeRsp().
	if(is_cck_rate)
	{
		pstats->SignalStrength = precord_stats->SignalStrength = (u8)(rtl819x_signal_scale_mapping((long)pwdb_all));//PWDB_ALL;

	}
	else
	{
		//pRfd->Status.SignalStrength = pRecordRfd->Status.SignalStrength = (u1Byte)(SignalScaleMapping(total_rssi/=RF90_PATH_MAX));//(u1Byte)(total_rssi/=RF90_PATH_MAX);
		// We can judge RX path number now.
		if (rf_rx_num != 0)
			pstats->SignalStrength = precord_stats->SignalStrength = (u8)(rtl819x_signal_scale_mapping((long)(total_rssi/=rf_rx_num)));
	}
}

static void
rtl8192_record_rxdesc_forlateruse(
	struct ieee80211_rx_stats * psrc_stats,
	struct ieee80211_rx_stats * ptarget_stats
)
{
	ptarget_stats->bIsAMPDU = psrc_stats->bIsAMPDU;
	ptarget_stats->bFirstMPDU = psrc_stats->bFirstMPDU;
}

static void TranslateRxSignalStuff819xpci(struct r8192_priv *priv,
        struct sk_buff *skb,
        struct ieee80211_rx_stats * pstats,
        prx_desc_819x_pci pdesc,
        prx_fwinfo_819x_pci pdrvinfo)
{
    // TODO: We must only check packet for current MAC address. Not finish
    bool bpacket_match_bssid, bpacket_toself;
    bool bPacketBeacon=false, bToSelfBA=false;
    struct ieee80211_hdr_3addr *hdr;
    u16 fc,type;

    // Get Signal Quality for only RX data queue (but not command queue)

    u8* tmp_buf;
    u8	*praddr;

    /* Get MAC frame start address. */
    tmp_buf = skb->data;

    hdr = (struct ieee80211_hdr_3addr *)tmp_buf;
    fc = le16_to_cpu(hdr->frame_ctl);
    type = WLAN_FC_GET_TYPE(fc);
    praddr = hdr->addr1;

    /* Check if the received packet is acceptabe. */
    bpacket_match_bssid = ((IEEE80211_FTYPE_CTL != type) &&
            (!compare_ether_addr(priv->ieee80211->current_network.bssid,	(fc & IEEE80211_FCTL_TODS)? hdr->addr1 : (fc & IEEE80211_FCTL_FROMDS )? hdr->addr2 : hdr->addr3))
            && (!pstats->bHwError) && (!pstats->bCRC)&& (!pstats->bICV));
    bpacket_toself =  bpacket_match_bssid & (!compare_ether_addr(praddr, priv->ieee80211->dev->dev_addr));

    if(WLAN_FC_GET_FRAMETYPE(fc)== IEEE80211_STYPE_BEACON)
    {
        bPacketBeacon = true;
    }
    if(WLAN_FC_GET_FRAMETYPE(fc) == IEEE80211_STYPE_BLOCKACK)
    {
        if (!compare_ether_addr(praddr, priv->ieee80211->dev->dev_addr))
            bToSelfBA = true;
    }

    //
    // Process PHY information for previous packet (RSSI/PWDB/EVM)
    //
    // Because phy information is contained in the last packet of AMPDU only, so driver
    // should process phy information of previous packet
    rtl8192_process_phyinfo(priv, tmp_buf, &priv->previous_stats, pstats);
    rtl8192_query_rxphystatus(priv, pstats, pdesc, pdrvinfo, &priv->previous_stats, bpacket_match_bssid,
            bpacket_toself ,bPacketBeacon, bToSelfBA);
    rtl8192_record_rxdesc_forlateruse(pstats, &priv->previous_stats);

}

static void rtl8192_tx_resume(struct r8192_priv *priv)
{
	struct ieee80211_device *ieee = priv->ieee80211;
	struct sk_buff *skb;
	int i;

	for (i = BK_QUEUE; i < TXCMD_QUEUE; i++) {
		while ((!skb_queue_empty(&ieee->skb_waitQ[i])) &&
		       (priv->ieee80211->check_nic_enough_desc(ieee, i) > 0)) {
			/* 1. dequeue the packet from the wait queue */
			skb = skb_dequeue(&ieee->skb_waitQ[i]);
			/* 2. tx the packet directly */
			ieee->softmac_data_hard_start_xmit(skb, ieee, 0);
		}
	}
}

static void rtl8192_irq_tx_tasklet(unsigned long arg)
{
	struct r8192_priv *priv = (struct r8192_priv*) arg;
	struct rtl8192_tx_ring *mgnt_ring = &priv->tx_ring[MGNT_QUEUE];
	unsigned long flags;

	/* check if we need to report that the management queue is drained */
	spin_lock_irqsave(&priv->irq_th_lock, flags);

	if (!skb_queue_len(&mgnt_ring->queue) &&
	    priv->ieee80211->ack_tx_to_ieee &&
	    rtl8192_is_tx_queue_empty(priv->ieee80211)) {
		priv->ieee80211->ack_tx_to_ieee = 0;
		ieee80211_ps_tx_ack(priv->ieee80211, 1);
	}

	spin_unlock_irqrestore(&priv->irq_th_lock, flags);

	rtl8192_tx_resume(priv);
}

/* Record the received data rate */
static void UpdateReceivedRateHistogramStatistics8190(
	struct r8192_priv *priv,
	struct ieee80211_rx_stats* pstats
	)
{
	u32 rcvType=1;   //0: Total, 1:OK, 2:CRC, 3:ICV
	u32 rateIndex;
	u32 preamble_guardinterval;  //1: short preamble/GI, 0: long preamble/GI

	if(pstats->bCRC)
		rcvType = 2;
	else if(pstats->bICV)
		rcvType = 3;

	if(pstats->bShortPreamble)
		preamble_guardinterval = 1;// short
	else
		preamble_guardinterval = 0;// long

	switch(pstats->rate)
	{
		//
		// CCK rate
		//
		case MGN_1M:    rateIndex = 0;  break;
	    	case MGN_2M:    rateIndex = 1;  break;
	    	case MGN_5_5M:  rateIndex = 2;  break;
	    	case MGN_11M:   rateIndex = 3;  break;
		//
		// Legacy OFDM rate
		//
	    	case MGN_6M:    rateIndex = 4;  break;
	    	case MGN_9M:    rateIndex = 5;  break;
	    	case MGN_12M:   rateIndex = 6;  break;
	    	case MGN_18M:   rateIndex = 7;  break;
	    	case MGN_24M:   rateIndex = 8;  break;
	    	case MGN_36M:   rateIndex = 9;  break;
	    	case MGN_48M:   rateIndex = 10; break;
	    	case MGN_54M:   rateIndex = 11; break;
		//
		// 11n High throughput rate
		//
	    	case MGN_MCS0:  rateIndex = 12; break;
	    	case MGN_MCS1:  rateIndex = 13; break;
	    	case MGN_MCS2:  rateIndex = 14; break;
	    	case MGN_MCS3:  rateIndex = 15; break;
	    	case MGN_MCS4:  rateIndex = 16; break;
	    	case MGN_MCS5:  rateIndex = 17; break;
	    	case MGN_MCS6:  rateIndex = 18; break;
	    	case MGN_MCS7:  rateIndex = 19; break;
	    	case MGN_MCS8:  rateIndex = 20; break;
	    	case MGN_MCS9:  rateIndex = 21; break;
	    	case MGN_MCS10: rateIndex = 22; break;
	    	case MGN_MCS11: rateIndex = 23; break;
	    	case MGN_MCS12: rateIndex = 24; break;
	    	case MGN_MCS13: rateIndex = 25; break;
	    	case MGN_MCS14: rateIndex = 26; break;
	    	case MGN_MCS15: rateIndex = 27; break;
		default:        rateIndex = 28; break;
	}
	priv->stats.received_rate_histogram[0][rateIndex]++; //total
	priv->stats.received_rate_histogram[rcvType][rateIndex]++;
}

static void rtl8192_rx(struct r8192_priv *priv)
{
    struct ieee80211_hdr_1addr *ieee80211_hdr = NULL;
    bool unicast_packet = false;
    struct ieee80211_rx_stats stats = {
        .signal = 0,
        .noise = -98,
        .rate = 0,
        .freq = IEEE80211_24GHZ_BAND,
    };
    unsigned int count = priv->rxringcount;
    prx_fwinfo_819x_pci pDrvInfo = NULL;
    struct sk_buff *new_skb;

    while (count--) {
        rx_desc_819x_pci *pdesc = &priv->rx_ring[priv->rx_idx];//rx descriptor
        struct sk_buff *skb = priv->rx_buf[priv->rx_idx];//rx pkt

        if (pdesc->OWN)
            /* wait data to be filled by hardware */
            return;

            stats.bICV = pdesc->ICV;
            stats.bCRC = pdesc->CRC32;
            stats.bHwError = pdesc->CRC32 | pdesc->ICV;

            stats.Length = pdesc->Length;
            if(stats.Length < 24)
                stats.bHwError |= 1;

            if(stats.bHwError) {
                stats.bShift = false;
                goto done;
            }
                pDrvInfo = NULL;
                new_skb = dev_alloc_skb(priv->rxbuffersize);

                if (unlikely(!new_skb))
                    goto done;

                stats.RxDrvInfoSize = pdesc->RxDrvInfoSize;
                stats.RxBufShift = ((pdesc->Shift)&0x03);
                stats.Decrypted = !pdesc->SWDec;

                pci_dma_sync_single_for_cpu(priv->pdev,
                     *((dma_addr_t *)skb->cb),
                     priv->rxbuffersize,
                     PCI_DMA_FROMDEVICE);
                skb_put(skb, pdesc->Length);
                pDrvInfo = (rx_fwinfo_819x_pci *)(skb->data + stats.RxBufShift);
                skb_reserve(skb, stats.RxDrvInfoSize + stats.RxBufShift);

                stats.rate = HwRateToMRate90((bool)pDrvInfo->RxHT, (u8)pDrvInfo->RxRate);
                stats.bShortPreamble = pDrvInfo->SPLCP;

                /* it is debug only. It should be disabled in released driver.
                 * 2007.1.11 by Emily
                 * */
                UpdateReceivedRateHistogramStatistics8190(priv, &stats);

                stats.bIsAMPDU = (pDrvInfo->PartAggr==1);
                stats.bFirstMPDU = (pDrvInfo->PartAggr==1) && (pDrvInfo->FirstAGGR==1);

                stats.TimeStampLow = pDrvInfo->TSFL;
                stats.TimeStampHigh = read_nic_dword(priv, TSFR+4);

                UpdateRxPktTimeStamp8190(priv, &stats);

                //
                // Get Total offset of MPDU Frame Body
                //
                if((stats.RxBufShift + stats.RxDrvInfoSize) > 0)
                    stats.bShift = 1;

                /* ???? */
                TranslateRxSignalStuff819xpci(priv, skb, &stats, pdesc, pDrvInfo);

                /* Rx A-MPDU */
                if(pDrvInfo->FirstAGGR==1 || pDrvInfo->PartAggr == 1)
                    RT_TRACE(COMP_RXDESC, "pDrvInfo->FirstAGGR = %d, pDrvInfo->PartAggr = %d\n",
                            pDrvInfo->FirstAGGR, pDrvInfo->PartAggr);
		   skb_trim(skb, skb->len - 4/*sCrcLng*/);
                /* rx packets statistics */
                ieee80211_hdr = (struct ieee80211_hdr_1addr *)skb->data;
                unicast_packet = false;

                if(is_broadcast_ether_addr(ieee80211_hdr->addr1)) {
                    //TODO
                }else if(is_multicast_ether_addr(ieee80211_hdr->addr1)){
                    //TODO
                }else {
                    /* unicast packet */
                    unicast_packet = true;
                }

                if(!ieee80211_rtl_rx(priv->ieee80211, skb, &stats)){
                    dev_kfree_skb_any(skb);
                } else {
                    priv->stats.rxok++;
                    if(unicast_packet) {
                        priv->stats.rxbytesunicast += skb->len;
                    }
                }

		pci_unmap_single(priv->pdev, *((dma_addr_t *) skb->cb),
			priv->rxbuffersize, PCI_DMA_FROMDEVICE);

                skb = new_skb;
                priv->rx_buf[priv->rx_idx] = skb;
                *((dma_addr_t *) skb->cb) = pci_map_single(priv->pdev, skb_tail_pointer(skb), priv->rxbuffersize, PCI_DMA_FROMDEVICE);

done:
        pdesc->BufferAddress = cpu_to_le32(*((dma_addr_t *)skb->cb));
        pdesc->OWN = 1;
        pdesc->Length = priv->rxbuffersize;
        if (priv->rx_idx == priv->rxringcount-1)
            pdesc->EOR = 1;
        priv->rx_idx = (priv->rx_idx + 1) % priv->rxringcount;
    }

}

static void rtl8192_irq_rx_tasklet(unsigned long arg)
{
	struct r8192_priv *priv = (struct r8192_priv*) arg;
	rtl8192_rx(priv);
	/* unmask RDU */
       write_nic_dword(priv, INTA_MASK, read_nic_dword(priv, INTA_MASK) | IMR_RDU);
}

static const struct net_device_ops rtl8192_netdev_ops = {
	.ndo_open =			rtl8192_open,
	.ndo_stop =			rtl8192_close,
	.ndo_tx_timeout =		tx_timeout,
	.ndo_do_ioctl =			rtl8192_ioctl,
	.ndo_set_multicast_list =	r8192_set_multicast,
	.ndo_set_mac_address =		r8192_set_mac_adr,
	.ndo_start_xmit = 		ieee80211_rtl_xmit,
};

static int __devinit rtl8192_pci_probe(struct pci_dev *pdev,
			 const struct pci_device_id *id)
{
	struct net_device *dev = NULL;
	struct r8192_priv *priv= NULL;
	u8 unit = 0;
	int ret = -ENODEV;
	unsigned long pmem_start, pmem_len, pmem_flags;
	u8 revisionid;

	RT_TRACE(COMP_INIT,"Configuring chip resources\n");

	if( pci_enable_device (pdev) ){
		RT_TRACE(COMP_ERR,"Failed to enable PCI device");
		return -EIO;
	}

	pci_set_master(pdev);
	//pci_set_wmi(pdev);
	pci_set_dma_mask(pdev, 0xffffff00ULL);
	pci_set_consistent_dma_mask(pdev,0xffffff00ULL);
	dev = alloc_ieee80211(sizeof(struct r8192_priv));
	if (!dev) {
		ret = -ENOMEM;
		goto fail_free;
	}

	pci_set_drvdata(pdev, dev);
	SET_NETDEV_DEV(dev, &pdev->dev);
	priv = ieee80211_priv(dev);
	priv->ieee80211 = netdev_priv(dev);
	priv->pdev=pdev;
	if((pdev->subsystem_vendor == PCI_VENDOR_ID_DLINK)&&(pdev->subsystem_device == 0x3304)){
		priv->ieee80211->bSupportRemoteWakeUp = 1;
	} else
	{
		priv->ieee80211->bSupportRemoteWakeUp = 0;
	}

	pmem_start = pci_resource_start(pdev, 1);
	pmem_len = pci_resource_len(pdev, 1);
	pmem_flags = pci_resource_flags (pdev, 1);

	if (!(pmem_flags & IORESOURCE_MEM)) {
		RT_TRACE(COMP_ERR, "region #1 not a MMIO resource, aborting\n");
		goto fail;
	}

	//DMESG("Memory mapped space @ 0x%08lx ", pmem_start);
	if( ! request_mem_region(pmem_start, pmem_len, RTL819xE_MODULE_NAME)) {
		RT_TRACE(COMP_ERR,"request_mem_region failed!\n");
		goto fail;
	}

	priv->mem_start = ioremap_nocache(pmem_start, pmem_len);
	if (!priv->mem_start) {
		RT_TRACE(COMP_ERR,"ioremap failed!\n");
		goto fail1;
	}

	dev->mem_start = (unsigned long) priv->mem_start;
	dev->mem_end = (unsigned long) (priv->mem_start +
					pci_resource_len(pdev, 0));

        /* We disable the RETRY_TIMEOUT register (0x41) to keep
         * PCI Tx retries from interfering with C3 CPU state */
         pci_write_config_byte(pdev, 0x41, 0x00);

	pci_read_config_byte(pdev, 0x08, &revisionid);
	/* If the revisionid is 0x10, the device uses rtl8192se. */
	if (pdev->device == 0x8192 && revisionid == 0x10)
		goto fail1;

	pci_read_config_byte(pdev, 0x05, &unit);
	pci_write_config_byte(pdev, 0x05, unit & (~0x04));

	dev->irq = pdev->irq;
	priv->irq = 0;

	dev->netdev_ops = &rtl8192_netdev_ops;

	dev->wireless_handlers = &r8192_wx_handlers_def;
	dev->type=ARPHRD_ETHER;

	dev->watchdog_timeo = HZ*3;

	if (dev_alloc_name(dev, ifname) < 0){
                RT_TRACE(COMP_INIT, "Oops: devname already taken! Trying wlan%%d...\n");
		strcpy(ifname, "wlan%d");
		dev_alloc_name(dev, ifname);
        }

	RT_TRACE(COMP_INIT, "Driver probe completed1\n");
	if (rtl8192_init(priv)!=0) {
		RT_TRACE(COMP_ERR, "Initialization failed\n");
		goto fail;
	}

	register_netdev(dev);
	RT_TRACE(COMP_INIT, "dev name=======> %s\n",dev->name);
	rtl8192_proc_init_one(priv);

	RT_TRACE(COMP_INIT, "Driver probe completed\n");
	return 0;

fail1:

	if (priv->mem_start) {
		iounmap(priv->mem_start);
		release_mem_region( pci_resource_start(pdev, 1),
				    pci_resource_len(pdev, 1) );
	}

fail:
	if(dev){

		if (priv->irq) {
			free_irq(priv->irq, priv);
			priv->irq = 0;
		}
		free_ieee80211(dev);
	}

fail_free:
	pci_disable_device(pdev);

	DMESG("wlan driver load failed\n");
	pci_set_drvdata(pdev, NULL);
	return ret;

}

/* detach all the work and timer structure declared or inititialized
 * in r8192_init function.
 * */
static void rtl8192_cancel_deferred_work(struct r8192_priv* priv)
{
	/* call cancel_work_sync instead of cancel_delayed_work if and only if Linux_version_code
         * is  or is newer than 2.6.20 and work structure is defined to be struct work_struct.
         * Otherwise call cancel_delayed_work is enough.
         * FIXME (2.6.20 should 2.6.22, work_struct should not cancel)
         * */
	cancel_delayed_work(&priv->watch_dog_wq);
	cancel_delayed_work(&priv->update_beacon_wq);
	cancel_delayed_work(&priv->ieee80211->hw_wakeup_wq);
	cancel_delayed_work(&priv->gpio_change_rf_wq);
	cancel_work_sync(&priv->reset_wq);
	cancel_work_sync(&priv->qos_activate);
}

static void __devexit rtl8192_pci_disconnect(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct r8192_priv *priv ;
	u32 i;

	if (dev) {

		unregister_netdev(dev);

		priv = ieee80211_priv(dev);

		rtl8192_proc_remove_one(priv);

		rtl8192_down(dev);
		if (priv->pFirmware)
		{
			vfree(priv->pFirmware);
			priv->pFirmware = NULL;
		}
		destroy_workqueue(priv->priv_wq);

		/* free tx/rx rings */
		rtl8192_free_rx_ring(priv);
		for (i = 0; i < MAX_TX_QUEUE_COUNT; i++)
			rtl8192_free_tx_ring(priv, i);

		if (priv->irq) {
			printk("Freeing irq %d\n", priv->irq);
			free_irq(priv->irq, priv);
			priv->irq = 0;
		}

		if (priv->mem_start) {
			iounmap(priv->mem_start);
			release_mem_region( pci_resource_start(pdev, 1),
					    pci_resource_len(pdev, 1) );
		}

		free_ieee80211(dev);
	}

	pci_disable_device(pdev);
	RT_TRACE(COMP_DOWN, "wlan driver removed\n");
}

extern int ieee80211_rtl_init(void);
extern void ieee80211_rtl_exit(void);

static int __init rtl8192_pci_module_init(void)
{
	int retval;

	retval = ieee80211_rtl_init();
	if (retval)
		return retval;

	printk(KERN_INFO "\nLinux kernel driver for RTL8192 based WLAN cards\n");
	printk(KERN_INFO "Copyright (c) 2007-2008, Realsil Wlan\n");
	RT_TRACE(COMP_INIT, "Initializing module\n");
	rtl8192_proc_module_init();
      if(0!=pci_register_driver(&rtl8192_pci_driver))
	{
		DMESG("No device found");
		/*pci_unregister_driver (&rtl8192_pci_driver);*/
		return -ENODEV;
	}
	return 0;
}

static void __exit rtl8192_pci_module_exit(void)
{
	pci_unregister_driver(&rtl8192_pci_driver);

	RT_TRACE(COMP_DOWN, "Exiting\n");
	rtl8192_proc_module_remove();
	ieee80211_rtl_exit();
}

static irqreturn_t rtl8192_interrupt(int irq, void *param)
{
	struct r8192_priv *priv = param;
	struct net_device *dev = priv->ieee80211->dev;
	unsigned long flags;
	u32 inta;
	irqreturn_t ret = IRQ_HANDLED;

	spin_lock_irqsave(&priv->irq_th_lock, flags);

	/* ISR: 4bytes */

	inta = read_nic_dword(priv, ISR); /* & priv->IntrMask; */
	write_nic_dword(priv, ISR, inta); /* reset int situation */

	if (!inta) {
		/*
		 * most probably we can safely return IRQ_NONE,
		 * but for now is better to avoid problems
		 */
		goto out_unlock;
	}

	if (inta == 0xffff) {
		/* HW disappared */
		goto out_unlock;
	}

	if (!netif_running(dev))
		goto out_unlock;

	if (inta & IMR_TBDOK) {
		RT_TRACE(COMP_INTR, "beacon ok interrupt!\n");
		rtl8192_tx_isr(priv, BEACON_QUEUE);
		priv->stats.txbeaconokint++;
	}

	if (inta & IMR_TBDER) {
		RT_TRACE(COMP_INTR, "beacon ok interrupt!\n");
		rtl8192_tx_isr(priv, BEACON_QUEUE);
		priv->stats.txbeaconerr++;
	}

	if (inta & IMR_MGNTDOK ) {
		RT_TRACE(COMP_INTR, "Manage ok interrupt!\n");
		priv->stats.txmanageokint++;
		rtl8192_tx_isr(priv, MGNT_QUEUE);
	}

	if (inta & IMR_COMDOK)
	{
		priv->stats.txcmdpktokint++;
		rtl8192_tx_isr(priv, TXCMD_QUEUE);
	}

	if (inta & IMR_ROK) {
		priv->stats.rxint++;
		tasklet_schedule(&priv->irq_rx_tasklet);
	}

	if (inta & IMR_BcnInt) {
		RT_TRACE(COMP_INTR, "prepare beacon for interrupt!\n");
		tasklet_schedule(&priv->irq_prepare_beacon_tasklet);
	}

	if (inta & IMR_RDU) {
		RT_TRACE(COMP_INTR, "rx descriptor unavailable!\n");
		priv->stats.rxrdu++;
		/* reset int situation */
		write_nic_dword(priv, INTA_MASK, read_nic_dword(priv, INTA_MASK) & ~IMR_RDU);
		tasklet_schedule(&priv->irq_rx_tasklet);
	}

	if (inta & IMR_RXFOVW) {
		RT_TRACE(COMP_INTR, "rx overflow !\n");
		priv->stats.rxoverflow++;
		tasklet_schedule(&priv->irq_rx_tasklet);
	}

	if (inta & IMR_TXFOVW)
		priv->stats.txoverflow++;

	if (inta & IMR_BKDOK) {
		RT_TRACE(COMP_INTR, "BK Tx OK interrupt!\n");
		priv->stats.txbkokint++;
		priv->ieee80211->LinkDetectInfo.NumTxOkInPeriod++;
		rtl8192_tx_isr(priv, BK_QUEUE);
	}

	if (inta & IMR_BEDOK) {
		RT_TRACE(COMP_INTR, "BE TX OK interrupt!\n");
		priv->stats.txbeokint++;
		priv->ieee80211->LinkDetectInfo.NumTxOkInPeriod++;
		rtl8192_tx_isr(priv, BE_QUEUE);
	}

	if (inta & IMR_VIDOK) {
		RT_TRACE(COMP_INTR, "VI TX OK interrupt!\n");
		priv->stats.txviokint++;
		priv->ieee80211->LinkDetectInfo.NumTxOkInPeriod++;
		rtl8192_tx_isr(priv, VI_QUEUE);
	}

	if (inta & IMR_VODOK) {
		priv->stats.txvookint++;
		priv->ieee80211->LinkDetectInfo.NumTxOkInPeriod++;
		rtl8192_tx_isr(priv, VO_QUEUE);
	}

out_unlock:
	spin_unlock_irqrestore(&priv->irq_th_lock, flags);

	return ret;
}

void EnableHWSecurityConfig8192(struct r8192_priv *priv)
{
        u8 SECR_value = 0x0;
	struct ieee80211_device* ieee = priv->ieee80211;

	SECR_value = SCR_TxEncEnable | SCR_RxDecEnable;

	if (((KEY_TYPE_WEP40 == ieee->pairwise_key_type) || (KEY_TYPE_WEP104 == ieee->pairwise_key_type)) && (priv->ieee80211->auth_mode != 2))
	{
		SECR_value |= SCR_RxUseDK;
		SECR_value |= SCR_TxUseDK;
	}
	else if ((ieee->iw_mode == IW_MODE_ADHOC) && (ieee->pairwise_key_type & (KEY_TYPE_CCMP | KEY_TYPE_TKIP)))
	{
		SECR_value |= SCR_RxUseDK;
		SECR_value |= SCR_TxUseDK;
	}

        //add HWSec active enable here.
//default using hwsec. when peer AP is in N mode only and pairwise_key_type is none_aes(which HT_IOT_ACT_PURE_N_MODE indicates it), use software security. when peer AP is in b,g,n mode mixed and pairwise_key_type is none_aes, use g mode hw security. WB on 2008.7.4
	ieee->hwsec_active = 1;

	if ((ieee->pHTInfo->IOTAction&HT_IOT_ACT_PURE_N_MODE) || !hwwep)//!ieee->hwsec_support) //add hwsec_support flag to totol control hw_sec on/off
	{
		ieee->hwsec_active = 0;
		SECR_value &= ~SCR_RxDecEnable;
	}

	RT_TRACE(COMP_SEC,"%s:, hwsec:%d, pairwise_key:%d, SECR_value:%x\n", __FUNCTION__,
			ieee->hwsec_active, ieee->pairwise_key_type, SECR_value);
	{
                write_nic_byte(priv, SECR,  SECR_value);//SECR_value |  SCR_UseDK );
        }

}
#define TOTAL_CAM_ENTRY 32
//#define CAM_CONTENT_COUNT 8
void setKey(struct r8192_priv *priv, u8 EntryNo, u8 KeyIndex, u16 KeyType,
	    const u8 *MacAddr, u8 DefaultKey, u32 *KeyContent)
{
	u32 TargetCommand = 0;
	u32 TargetContent = 0;
	u16 usConfig = 0;
	u8 i;
#ifdef ENABLE_IPS
	RT_RF_POWER_STATE	rtState;

	rtState = priv->eRFPowerState;
	if (priv->PowerSaveControl.bInactivePs){
		if(rtState == eRfOff){
			if(priv->RfOffReason > RF_CHANGE_BY_IPS)
			{
				RT_TRACE(COMP_ERR, "%s(): RF is OFF.\n",__FUNCTION__);
				//up(&priv->wx_sem);
				return ;
			}
			else{
				down(&priv->ieee80211->ips_sem);
				IPSLeave(priv);
				up(&priv->ieee80211->ips_sem);
			}
		}
	}
	priv->ieee80211->is_set_key = true;
#endif
	if (EntryNo >= TOTAL_CAM_ENTRY)
		RT_TRACE(COMP_ERR, "cam entry exceeds in setKey()\n");

	RT_TRACE(COMP_SEC, "====>to setKey(), priv:%p, EntryNo:%d, KeyIndex:%d, KeyType:%d, MacAddr%pM\n", priv, EntryNo, KeyIndex, KeyType, MacAddr);

	if (DefaultKey)
		usConfig |= BIT15 | (KeyType<<2);
	else
		usConfig |= BIT15 | (KeyType<<2) | KeyIndex;
//	usConfig |= BIT15 | (KeyType<<2) | (DefaultKey<<5) | KeyIndex;

	for(i=0 ; i<CAM_CONTENT_COUNT; i++){
		TargetCommand  = i+CAM_CONTENT_COUNT*EntryNo;
		TargetCommand |= BIT31|BIT16;

		if(i==0){//MAC|Config
			TargetContent = (u32)(*(MacAddr+0)) << 16|
					(u32)(*(MacAddr+1)) << 24|
					(u32)usConfig;

			write_nic_dword(priv, WCAMI, TargetContent);
			write_nic_dword(priv, RWCAM, TargetCommand);
		}
		else if(i==1){//MAC
                        TargetContent = (u32)(*(MacAddr+2)) 	 |
                                        (u32)(*(MacAddr+3)) <<  8|
                                        (u32)(*(MacAddr+4)) << 16|
                                        (u32)(*(MacAddr+5)) << 24;
			write_nic_dword(priv, WCAMI, TargetContent);
			write_nic_dword(priv, RWCAM, TargetCommand);
		}
		else {	//Key Material
			if(KeyContent != NULL)
			{
			write_nic_dword(priv, WCAMI, (u32)(*(KeyContent+i-2)) );
			write_nic_dword(priv, RWCAM, TargetCommand);
		}
	}
	}
	RT_TRACE(COMP_SEC,"=========>after set key, usconfig:%x\n", usConfig);
}

bool NicIFEnableNIC(struct r8192_priv *priv)
{
	RT_STATUS init_status = RT_STATUS_SUCCESS;
	PRT_POWER_SAVE_CONTROL pPSC = &priv->PowerSaveControl;

	//YJ,add,091109
	if (priv->up == 0){
		RT_TRACE(COMP_ERR, "ERR!!! %s(): Driver is already down!\n",__FUNCTION__);
		priv->bdisable_nic = false;  //YJ,add,091111
		return false;
	}
	// <1> Reset memory: descriptor, buffer,..
	//NicIFResetMemory(Adapter);

	// <2> Enable Adapter
	//priv->bfirst_init = true;
	init_status = rtl8192_adapter_start(priv);
	if (init_status != RT_STATUS_SUCCESS) {
		RT_TRACE(COMP_ERR,"ERR!!! %s(): initialization is failed!\n",__FUNCTION__);
		priv->bdisable_nic = false;  //YJ,add,091111
		return -1;
	}
	RT_CLEAR_PS_LEVEL(pPSC, RT_RF_OFF_LEVL_HALT_NIC);
	//priv->bfirst_init = false;

	// <3> Enable Interrupt
	rtl8192_irq_enable(priv);
	priv->bdisable_nic = false;

	return (init_status == RT_STATUS_SUCCESS);
}

bool NicIFDisableNIC(struct r8192_priv *priv)
{
	bool	status = true;
	u8 tmp_state = 0;
	// <1> Disable Interrupt

	priv->bdisable_nic = true;	//YJ,move,091109
	tmp_state = priv->ieee80211->state;

	ieee80211_softmac_stop_protocol(priv->ieee80211, false);

	priv->ieee80211->state = tmp_state;
	rtl8192_cancel_deferred_work(priv);
	rtl8192_irq_disable(priv);
	// <2> Stop all timer

	// <3> Disable Adapter
	rtl8192_halt_adapter(priv, false);
//	priv->bdisable_nic = true;

	return status;
}

module_init(rtl8192_pci_module_init);
module_exit(rtl8192_pci_module_exit);
