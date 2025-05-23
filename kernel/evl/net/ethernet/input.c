/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/bitmap.h>
#include <evl/net/skb.h>
#include <evl/net/input.h>
#include <evl/net/packet.h>
#include <evl/net/ipv4.h>

static DECLARE_BITMAP(vlan_map, VLAN_N_VID);

static struct evl_net_handler evl_net_ether;

static void untag_packet(struct sk_buff *skb,
			unsigned char *mac_hdr, struct vlan_ethhdr *ehdr)
{
	int mac_len;

	/*
	 * We run very early in the RX path, eth_type_trans() already
	 * pulled the MAC header at this point though. We accept
	 * ETH_P_IP encapsulation only so that ARP and friends still
	 * flow through the regular network stack. Fix up the protocol
	 * tag in the skb manually, cache the VLAN information in the
	 * skb, then reorder the MAC header eventually.
	 */
	skb->protocol = ehdr->h_vlan_encapsulated_proto;
	__vlan_hwaccel_put_tag(skb, ehdr->h_vlan_proto,
			ntohs(ehdr->h_vlan_TCI));
	skb_pull_inline(skb, VLAN_HLEN);
	mac_len = skb->data - mac_hdr;
	if (likely(mac_len > VLAN_HLEN + ETH_TLEN)) {
		memmove(mac_hdr + VLAN_HLEN, mac_hdr,
			mac_len - VLAN_HLEN - ETH_TLEN);
	}
	skb->mac_header += VLAN_HLEN;
}

/**
 * evl_net_ether_accept - Unconditionally accept an ethernet packet
 * for the out-of-band stack, stripping out the VLAN information if
 * present.
 *
 * @skb the packet to deliver. May be linked to some upstream queue.
 */
bool evl_net_ether_accept(struct sk_buff *skb)
{
	struct vlan_ethhdr *ehdr;
	unsigned char *mac_hdr;
	u16 vlan_tci;

	/* If accelerated, the VLAN header is already out. */
	if (!__vlan_hwaccel_get_tag(skb, &vlan_tci))
		goto pick;

	/*
	 * Deal manually with input from adapters without hw
	 * accelerated VLAN processing, in this case we need to pull
	 * the VLAN header from the packet. See comment in
	 * evl_net_ether_accept_vlan().
	 */
	if (skb_vlan_tag_present(skb) || !eth_type_vlan(skb->protocol))
		goto pick;

	mac_hdr = skb_mac_header(skb);
	ehdr = (struct vlan_ethhdr *)mac_hdr;
	if (ehdr->h_vlan_encapsulated_proto != htons(ETH_P_IP))
		return false;

	untag_packet(skb, mac_hdr, ehdr);
pick:
	evl_net_receive(skb, &evl_net_ether);

	return true;
}

/**
 * evl_net_ether_accept_vlan - Accept an ethernet packet if tagged for
 * an out-of-band VLAN.
 *
 * Decide whether an incoming ethernet packet should be handled by the
 * out-of-band networking stack instead of the in-band one. This
 * routine checks whether some VLAN information stored into the packet
 * matches one of the VIDs reserved for out-of-band traffic.
 *
 * @skb the packet to deliver. May be linked to some upstream queue.
 *
 * Returns true if the out-of-band stack should handle the packet.
 */
bool evl_net_ether_accept_vlan(struct sk_buff *skb)
{
	struct vlan_ethhdr *ehdr;
	unsigned char *mac_hdr;
	u16 vlan_tci;

	/* Try the accelerated way first. */
	if (!__vlan_hwaccel_get_tag(skb, &vlan_tci) &&
		test_bit(vlan_tci & VLAN_VID_MASK, vlan_map))
		goto pick;

	/*
	 * Deal manually with input from adapters without hw
	 * accelerated VLAN processing. Only if we should handle this
	 * packet, pull the VLAN header from it.
	 */
	if (!skb_vlan_tag_present(skb) &&
		eth_type_vlan(skb->protocol)) {
		mac_hdr = skb_mac_header(skb);
		ehdr = (struct vlan_ethhdr *)mac_hdr;
		if (ehdr->h_vlan_encapsulated_proto == htons(ETH_P_IP)) {
			vlan_tci = ntohs(ehdr->h_vlan_TCI);
			if (test_bit(vlan_tci & VLAN_VID_MASK, vlan_map))
				goto untag;
		}
	}

	return false;
untag:
	untag_packet(skb, mac_hdr, ehdr);
pick:
	evl_net_receive(skb, &evl_net_ether);

	return true;
}

/**
 *	net_ether_ingress - pass an ethernet packet upward to the
 *	stack.
 *
 *	We are called from the RX kthread from oob context, hard irqs
 *	on.  skb is not linked to any queue.
 */
static void net_ether_ingress(struct sk_buff *skb) /* oob */
{
	/* Try to deliver to a raw packet socket first. */
	if (evl_net_packet_deliver(skb))
		return;

	switch (ntohs(skb->protocol)) {
	case ETH_P_IP:
		if (!evl_net_ipv4_deliver(skb))
			return;
	}

	evl_net_free_skb(skb);	/* Dropped. */
}

static struct evl_net_handler evl_net_ether = {
	.ingress = net_ether_ingress,
};

static inline bool contains_reserved_vid(unsigned long *map)
{
	/* VID 0, 1 and 4095 are reserved. */
	return test_bit(0, map) || test_bit(1, map) || test_bit(VLAN_VID_MASK, map);
}

ssize_t evl_net_store_vlans(const char *buf, size_t len)
{
	unsigned long *new_map;
	ssize_t ret;

	new_map = bitmap_zalloc(VLAN_N_VID, GFP_KERNEL);
	if (new_map == NULL)
		return -ENOMEM;

	ret = bitmap_parselist(buf, new_map, VLAN_N_VID);
	if (!ret && contains_reserved_vid(new_map))
		ret = -EINVAL;
	if (ret) {
		bitmap_free(new_map);
		return ret;
	}

	/*
	 * We don't have to provide for atomic update wrt our net
	 * stack when updating the vlan map. We use the VID as a
	 * shortlived information early for filtering
	 * input. Serializing writes/stores which the vfs does for us
	 * is enough.
	 */
	bitmap_copy(vlan_map, new_map, VLAN_N_VID);
	bitmap_free(new_map);

	return len;
}

ssize_t evl_net_show_vlans(char *buf, size_t len)
{
	return scnprintf(buf, len, "%*pbl\n", VLAN_N_VID, vlan_map);
}
