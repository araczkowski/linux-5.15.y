/*
 * Copyright (C) 2018-2021 Felix Fietkau <nbd@nbd.name>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/netfilter.h>
#include <linux/netfilter/xt_FLOWOFFLOAD.h>
#include <net/ip.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_extend.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_flow_table.h>

struct xt_flowoffload_hook {
	struct hlist_node list;
	struct nf_hook_ops ops;
	struct net *net;
	bool registered;
	bool used;
};

struct xt_flowoffload_table {
	struct nf_flowtable ft;
	struct hlist_head hooks;
	struct delayed_work work;
};

static DEFINE_SPINLOCK(hooks_lock);

struct xt_flowoffload_table flowtable[2];

static unsigned int
xt_flowoffload_net_hook(void *priv, struct sk_buff *skb,
			const struct nf_hook_state *state)
{
	struct nf_flowtable *ft = priv;

	if (!atomic_read(&ft->rhashtable.nelems))
		return NF_ACCEPT;

	switch (skb->protocol) {
	case htons(ETH_P_IP):
		return nf_flow_offload_ip_hook(priv, skb, state);
	case htons(ETH_P_IPV6):
		return nf_flow_offload_ipv6_hook(priv, skb, state);
	}

	return NF_ACCEPT;
}

static int
xt_flowoffload_create_hook(struct xt_flowoffload_table *table,
			   struct net_device *dev)
{
	struct xt_flowoffload_hook *hook;
	struct nf_hook_ops *ops;

	hook = kzalloc(sizeof(*hook), GFP_ATOMIC);
	if (!hook)
		return -ENOMEM;

	ops = &hook->ops;
	ops->pf = NFPROTO_NETDEV;
	ops->hooknum = NF_NETDEV_INGRESS;
	ops->priority = 10;
	ops->priv = &table->ft;
	ops->hook = xt_flowoffload_net_hook;
	ops->dev = dev;

	hlist_add_head(&hook->list, &table->hooks);
	mod_delayed_work(system_power_efficient_wq, &table->work, 0);

	return 0;
}

static struct xt_flowoffload_hook *
flow_offload_lookup_hook(struct xt_flowoffload_table *table,
			 struct net_device *dev)
{
	struct xt_flowoffload_hook *hook;

	hlist_for_each_entry(hook, &table->hooks, list) {
		if (hook->ops.dev == dev)
			return hook;
	}

	return NULL;
}

static void
xt_flowoffload_check_device(struct xt_flowoffload_table *table,
			    struct net_device *dev)
{
	struct xt_flowoffload_hook *hook;

	if (!dev)
		return;

	spin_lock_bh(&hooks_lock);
	hook = flow_offload_lookup_hook(table, dev);
	if (hook)
		hook->used = true;
	else
		xt_flowoffload_create_hook(table, dev);
	spin_unlock_bh(&hooks_lock);
}

static void
xt_flowoffload_register_hooks(struct xt_flowoffload_table *table)
{
	struct xt_flowoffload_hook *hook;

restart:
	hlist_for_each_entry(hook, &table->hooks, list) {
		if (hook->registered)
			continue;

		hook->registered = true;
		hook->net = dev_net(hook->ops.dev);
		spin_unlock_bh(&hooks_lock);
		nf_register_net_hook(hook->net, &hook->ops);
		if (table->ft.flags & NF_FLOWTABLE_HW_OFFLOAD)
			table->ft.type->setup(&table->ft, hook->ops.dev,
					      FLOW_BLOCK_BIND);
		spin_lock_bh(&hooks_lock);
		goto restart;
	}

}

static bool
xt_flowoffload_cleanup_hooks(struct xt_flowoffload_table *table)
{
	struct xt_flowoffload_hook *hook;
	bool active = false;

restart:
	spin_lock_bh(&hooks_lock);
	hlist_for_each_entry(hook, &table->hooks, list) {
		if (hook->used || !hook->registered) {
			active = true;
			continue;
		}

		hlist_del(&hook->list);
		spin_unlock_bh(&hooks_lock);
		if (table->ft.flags & NF_FLOWTABLE_HW_OFFLOAD)
			table->ft.type->setup(&table->ft, hook->ops.dev,
					      FLOW_BLOCK_UNBIND);
		nf_unregister_net_hook(hook->net, &hook->ops);
		kfree(hook);
		goto restart;
	}
	spin_unlock_bh(&hooks_lock);

	return active;
}

static void
xt_flowoffload_check_hook(struct flow_offload *flow, void *data)
{
	struct xt_flowoffload_table *table = data;
	struct flow_offload_tuple *tuple0 = &flow->tuplehash[0].tuple;
	struct flow_offload_tuple *tuple1 = &flow->tuplehash[1].tuple;
	struct xt_flowoffload_hook *hook;

	spin_lock_bh(&hooks_lock);
	hlist_for_each_entry(hook, &table->hooks, list) {
		if (hook->ops.dev->ifindex != tuple0->iifidx &&
		    hook->ops.dev->ifindex != tuple1->iifidx)
			continue;

		hook->used = true;
	}
	spin_unlock_bh(&hooks_lock);
}

static void
xt_flowoffload_hook_work(struct work_struct *work)
{
	struct xt_flowoffload_table *table;
	struct xt_flowoffload_hook *hook;
	int err;

	table = container_of(work, struct xt_flowoffload_table, work.work);

	spin_lock_bh(&hooks_lock);
	xt_flowoffload_register_hooks(table);
	hlist_for_each_entry(hook, &table->hooks, list)
		hook->used = false;
	spin_unlock_bh(&hooks_lock);

	err = nf_flow_table_iterate(&table->ft, xt_flowoffload_check_hook,
				    table);
	if (err && err != -EAGAIN)
		goto out;

	if (!xt_flowoffload_cleanup_hooks(table))
		return;

out:
	queue_delayed_work(system_power_efficient_wq, &table->work, HZ);
}

static bool
xt_flowoffload_skip(struct sk_buff *skb, int family)
{
	if (skb_sec_path(skb))
		return true;

	if (family == NFPROTO_IPV4) {
		const struct ip_options *opt = &(IPCB(skb)->opt);

		if (unlikely(opt->optlen))
			return true;
	}

	return false;
}

static bool flow_is_valid_ether_device(const struct net_device *dev)
{
	if (!dev || (dev->flags & IFF_LOOPBACK) || dev->type != ARPHRD_ETHER ||
	    dev->addr_len != ETH_ALEN || !is_valid_ether_addr(dev->dev_addr))
		return false;

	return true;
}

static void
xt_flowoffload_route_check_path(struct nf_flow_route *route,
				const struct nf_conn *ct,
				enum ip_conntrack_dir dir,
				struct net_device **out_dev)
{
	const struct dst_entry *dst = route->tuple[dir].dst;
	const void *daddr = &ct->tuplehash[!dir].tuple.src.u3;
	struct net_device_path_stack stack;
	enum net_device_path_type prev_type;
	struct net_device *dev = dst->dev;
	struct neighbour *n;
	bool last = false;
	u8 nud_state;
	int i;

	route->tuple[!dir].in.ifindex = dev->ifindex;
	route->tuple[dir].out.ifindex = dev->ifindex;

	if (route->tuple[dir].xmit_type == FLOW_OFFLOAD_XMIT_XFRM)
		return;

	if ((dev->flags & IFF_LOOPBACK) ||
	    dev->type != ARPHRD_ETHER || dev->addr_len != ETH_ALEN ||
	    !is_valid_ether_addr(dev->dev_addr))
		return;

	n = dst_neigh_lookup(dst, daddr);
	if (!n)
		return;

	read_lock_bh(&n->lock);
	nud_state = n->nud_state;
	memcpy(route->tuple[dir].out.h_dest, n->ha, ETH_ALEN);
	read_unlock_bh(&n->lock);
	neigh_release(n);

	if (!(nud_state & NUD_VALID))
		return;

	if (dev_fill_forward_path(dev, route->tuple[dir].out.h_dest, &stack) ||
	    !stack.num_paths)
		return;

	prev_type = DEV_PATH_ETHERNET;
	for (i = 0; i <= stack.num_paths; i++) {
		const struct net_device_path *path = &stack.path[i];
		int n_encaps = route->tuple[!dir].in.num_encaps;

		dev = (struct net_device *)path->dev;
		if (flow_is_valid_ether_device(dev)) {
			if (route->tuple[dir].xmit_type != FLOW_OFFLOAD_XMIT_DIRECT) {
				memcpy(route->tuple[dir].out.h_source,
				       dev->dev_addr, ETH_ALEN);
				route->tuple[dir].out.ifindex = dev->ifindex;
			}
			route->tuple[dir].xmit_type = FLOW_OFFLOAD_XMIT_DIRECT;
		}

		switch (path->type) {
		case DEV_PATH_PPPOE:
		case DEV_PATH_VLAN:
			if (n_encaps >= NF_FLOW_TABLE_ENCAP_MAX ||
			    i == stack.num_paths) {
				last = true;
				break;
			}

			route->tuple[!dir].in.num_encaps++;
			route->tuple[!dir].in.encap[n_encaps].id = path->encap.id;
			route->tuple[!dir].in.encap[n_encaps].proto = path->encap.proto;
			if (path->type == DEV_PATH_PPPOE)
				memcpy(route->tuple[dir].out.h_dest,
				       path->encap.h_dest, ETH_ALEN);
			break;
		case DEV_PATH_BRIDGE:
			switch (path->bridge.vlan_mode) {
			case DEV_PATH_BR_VLAN_TAG:
				if (n_encaps >= NF_FLOW_TABLE_ENCAP_MAX ||
				    i == stack.num_paths) {
					last = true;
					break;
				}

				route->tuple[!dir].in.num_encaps++;
				route->tuple[!dir].in.encap[n_encaps].id =
					path->bridge.vlan_id;
				route->tuple[!dir].in.encap[n_encaps].proto =
					path->bridge.vlan_proto;
				break;
			case DEV_PATH_BR_VLAN_UNTAG:
				route->tuple[!dir].in.num_encaps--;
				break;
			case DEV_PATH_BR_VLAN_UNTAG_HW:
				route->tuple[!dir].in.ingress_vlans |= BIT(n_encaps - 1);
				break;
			case DEV_PATH_BR_VLAN_KEEP:
				break;
			}
			break;
		default:
			last = true;
			break;
		}

		if (last)
			break;
	}

	*out_dev = dev;
	route->tuple[dir].out.hw_ifindex = dev->ifindex;
	route->tuple[!dir].in.ifindex = dev->ifindex;
}

static int
xt_flowoffload_route_dir(struct nf_flow_route *route, const struct nf_conn *ct,
			 enum ip_conntrack_dir dir,
			 const struct xt_action_param *par, int ifindex)
{
	struct dst_entry *dst = NULL;
	struct flowi fl;

	memset(&fl, 0, sizeof(fl));
	switch (xt_family(par)) {
	case NFPROTO_IPV4:
		fl.u.ip4.daddr = ct->tuplehash[!dir].tuple.src.u3.ip;
		fl.u.ip4.flowi4_oif = ifindex;
		break;
	case NFPROTO_IPV6:
		fl.u.ip6.saddr = ct->tuplehash[!dir].tuple.dst.u3.in6;
		fl.u.ip6.daddr = ct->tuplehash[!dir].tuple.src.u3.in6;
		fl.u.ip6.flowi6_oif = ifindex;
		break;
	}

	nf_route(xt_net(par), &dst, &fl, false, xt_family(par));
	if (!dst)
		return -ENOENT;

	route->tuple[dir].dst = dst;
	if (dst_xfrm(dst))
		route->tuple[dir].xmit_type = FLOW_OFFLOAD_XMIT_XFRM;
	else
		route->tuple[dir].xmit_type = FLOW_OFFLOAD_XMIT_NEIGH;

	return 0;
}

static int
xt_flowoffload_route(struct sk_buff *skb, const struct nf_conn *ct,
		     const struct xt_action_param *par,
		     struct nf_flow_route *route, enum ip_conntrack_dir dir,
		     struct net_device **dev)
{
	int ret;

	ret = xt_flowoffload_route_dir(route, ct, dir, par,
				       dev[dir]->ifindex);
	if (ret)
		return ret;

	ret = xt_flowoffload_route_dir(route, ct, !dir, par,
				       dev[!dir]->ifindex);
	if (ret)
		return ret;

	xt_flowoffload_route_check_path(route, ct, dir, &dev[!dir]);
	xt_flowoffload_route_check_path(route, ct, !dir, &dev[dir]);

	return 0;
}

static unsigned int
flowoffload_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	struct xt_flowoffload_table *table;
	const struct xt_flowoffload_target_info *info = par->targinfo;
	struct tcphdr _tcph, *tcph = NULL;
	enum ip_conntrack_info ctinfo;
	enum ip_conntrack_dir dir;
	struct nf_flow_route route = {};
	struct flow_offload *flow = NULL;
	struct net_device *devs[2] = {};
	struct nf_conn *ct;
	struct net *net;

	if (xt_flowoffload_skip(skb, xt_family(par)))
		return XT_CONTINUE;

	ct = nf_ct_get(skb, &ctinfo);
	if (ct == NULL)
		return XT_CONTINUE;

	switch (ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.protonum) {
	case IPPROTO_TCP:
		if (ct->proto.tcp.state != TCP_CONNTRACK_ESTABLISHED)
			return XT_CONTINUE;

		tcph = skb_header_pointer(skb, par->thoff,
					  sizeof(_tcph), &_tcph);
		if (unlikely(!tcph || tcph->fin || tcph->rst))
			return XT_CONTINUE;
		break;
	case IPPROTO_UDP:
		break;
	default:
		return XT_CONTINUE;
	}

	if (nf_ct_ext_exist(ct, NF_CT_EXT_HELPER) ||
	    ct->status & IPS_SEQ_ADJUST)
		return XT_CONTINUE;

	if (!nf_ct_is_confirmed(ct))
		return XT_CONTINUE;

	dir = CTINFO2DIR(ctinfo);
	devs[dir] = xt_out(par);
	devs[!dir] = xt_in(par);

	if (!devs[dir] || !devs[!dir])
		return XT_CONTINUE;

	if (test_and_set_bit(IPS_OFFLOAD_BIT, &ct->status))
		return XT_CONTINUE;

	dir = CTINFO2DIR(ctinfo);

	if (xt_flowoffload_route(skb, ct, par, &route, dir, devs) < 0)
		goto err_flow_route;

	flow = flow_offload_alloc(ct);
	if (!flow)
		goto err_flow_alloc;

	if (flow_offload_route_init(flow, &route) < 0)
		goto err_flow_add;

	if (tcph) {
		ct->proto.tcp.seen[0].flags |= IP_CT_TCP_FLAG_BE_LIBERAL;
		ct->proto.tcp.seen[1].flags |= IP_CT_TCP_FLAG_BE_LIBERAL;
	}

	table = &flowtable[!!(info->flags & XT_FLOWOFFLOAD_HW)];
	if (flow_offload_add(&table->ft, flow) < 0)
		goto err_flow_add;

	xt_flowoffload_check_device(table, devs[0]);
	xt_flowoffload_check_device(table, devs[1]);

	net = read_pnet(&table->ft.net);
	if (!net)
		write_pnet(&table->ft.net, xt_net(par));

	dst_release(route.tuple[dir].dst);
	dst_release(route.tuple[!dir].dst);

	return XT_CONTINUE;

err_flow_add:
	flow_offload_free(flow);
err_flow_alloc:
	dst_release(route.tuple[dir].dst);
	dst_release(route.tuple[!dir].dst);
err_flow_route:
	clear_bit(IPS_OFFLOAD_BIT, &ct->status);

	return XT_CONTINUE;
}

static int flowoffload_chk(const struct xt_tgchk_param *par)
{
	struct xt_flowoffload_target_info *info = par->targinfo;

	if (info->flags & ~XT_FLOWOFFLOAD_MASK)
		return -EINVAL;

	return 0;
}

static struct xt_target offload_tg_reg __read_mostly = {
	.family		= NFPROTO_UNSPEC,
	.name		= "FLOWOFFLOAD",
	.revision	= 0,
	.targetsize	= sizeof(struct xt_flowoffload_target_info),
	.usersize	= sizeof(struct xt_flowoffload_target_info),
	.checkentry	= flowoffload_chk,
	.target		= flowoffload_tg,
	.me		= THIS_MODULE,
};

static int flow_offload_netdev_event(struct notifier_block *this,
				     unsigned long event, void *ptr)
{
	struct xt_flowoffload_hook *hook0, *hook1;
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (event != NETDEV_UNREGISTER)
		return NOTIFY_DONE;

	spin_lock_bh(&hooks_lock);
	hook0 = flow_offload_lookup_hook(&flowtable[0], dev);
	if (hook0)
		hlist_del(&hook0->list);

	hook1 = flow_offload_lookup_hook(&flowtable[1], dev);
	if (hook1)
		hlist_del(&hook1->list);
	spin_unlock_bh(&hooks_lock);

	if (hook0) {
		nf_unregister_net_hook(hook0->net, &hook0->ops);
		kfree(hook0);
	}

	if (hook1) {
		nf_unregister_net_hook(hook1->net, &hook1->ops);
		kfree(hook1);
	}

	nf_flow_table_cleanup(dev);

	return NOTIFY_DONE;
}

static struct notifier_block flow_offload_netdev_notifier = {
	.notifier_call	= flow_offload_netdev_event,
};

static unsigned int
nf_flow_offload_inet_hook(void *priv, struct sk_buff *skb,
			  const struct nf_hook_state *state)
{
	switch (skb->protocol) {
	case htons(ETH_P_IP):
		return nf_flow_offload_ip_hook(priv, skb, state);
	case htons(ETH_P_IPV6):
		return nf_flow_offload_ipv6_hook(priv, skb, state);
	}

	return NF_ACCEPT;
}

static int nf_flow_rule_route_inet(struct net *net,
				   const struct flow_offload *flow,
				   enum flow_offload_tuple_dir dir,
				   struct nf_flow_rule *flow_rule)
{
	const struct flow_offload_tuple *flow_tuple = &flow->tuplehash[dir].tuple;
	int err;

	switch (flow_tuple->l3proto) {
	case NFPROTO_IPV4:
		err = nf_flow_rule_route_ipv4(net, flow, dir, flow_rule);
		break;
	case NFPROTO_IPV6:
		err = nf_flow_rule_route_ipv6(net, flow, dir, flow_rule);
		break;
	default:
		err = -1;
		break;
	}

	return err;
}

static struct nf_flowtable_type flowtable_inet = {
	.family		= NFPROTO_INET,
	.init		= nf_flow_table_init,
	.setup		= nf_flow_table_offload_setup,
	.action		= nf_flow_rule_route_inet,
	.free		= nf_flow_table_free,
	.hook		= nf_flow_offload_inet_hook,
	.owner		= THIS_MODULE,
};

static int init_flowtable(struct xt_flowoffload_table *tbl)
{
	INIT_DELAYED_WORK(&tbl->work, xt_flowoffload_hook_work);
	tbl->ft.type = &flowtable_inet;

	return nf_flow_table_init(&tbl->ft);
}

static int __init xt_flowoffload_tg_init(void)
{
	int ret;

	register_netdevice_notifier(&flow_offload_netdev_notifier);

	ret = init_flowtable(&flowtable[0]);
	if (ret)
		return ret;

	ret = init_flowtable(&flowtable[1]);
	if (ret)
		goto cleanup;

	flowtable[1].ft.flags = NF_FLOWTABLE_HW_OFFLOAD;

	ret = xt_register_target(&offload_tg_reg);
	if (ret)
		goto cleanup2;

	return 0;

cleanup2:
	nf_flow_table_free(&flowtable[1].ft);
cleanup:
	nf_flow_table_free(&flowtable[0].ft);
	return ret;
}

static void __exit xt_flowoffload_tg_exit(void)
{
	xt_unregister_target(&offload_tg_reg);
	unregister_netdevice_notifier(&flow_offload_netdev_notifier);
	nf_flow_table_free(&flowtable[0].ft);
	nf_flow_table_free(&flowtable[1].ft);
}

MODULE_LICENSE("GPL");
module_init(xt_flowoffload_tg_init);
module_exit(xt_flowoffload_tg_exit);