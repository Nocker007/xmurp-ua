#include "common.h"

static struct nf_hook_ops nfho[3];		// 需要在 INPUT、OUTPUT、FORWARD 各挂一个
static struct rkpManager* rkpm;

unsigned int hook_funcion(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
	unsigned rtn;

	static unsigned n_skb_captured = 0, n_skb_captured_lastPrint = 1;

	if(!rkpSetting_capture(skb))
		return NF_ACCEPT;
	rtn = rkpManager_execute(rkpm, skb);

	n_skb_captured++;
	if(n_skb_captured == n_skb_captured_lastPrint * 2)
	{
		if(verbose)
			printk("rkp-ua: Captured %d packets.\n", n_skb_captured);
		n_skb_captured_lastPrint *= 2;
	}
	return rtn;
}

static int __init hook_init(void)
{
	int ret;
	unsigned i;

	rkpm = rkpManager_new();

	sprintf(str_uaRkp, "Mozilla/%d.0 (Windows NT 10.0; WOW64; Trident/7.0; rv:11.0) like Gecko", VERSION);

	nfho[0].hooknum = NF_INET_LOCAL_IN;
	nfho[1].hooknum = NF_INET_LOCAL_OUT;
	nfho[2].hooknum = NF_INET_FORWARD;
	for(i = 0; i < 3; i++)
	{
		nfho[i].hook = hook_funcion;
		nfho[i].pf = NFPROTO_IPV4;
		nfho[i].priority = NF_IP_PRI_MANGLE + 1;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
    	ret = nf_register_net_hooks(&init_net, nfho, 3);
#else
    	ret = nf_register_hooks(nfho, 3);
#endif

	printk("rkp-ua: Started, version %d\n", VERSION);
	printk("rkp-ua: nf_register_hook returnd %d.\n", ret);
	printk("rkp-ua: autocapture=%c, mark_capture=0x%x, mark_ack=0x%x\n",
			'n' + autocapture * ('y' - 'n'), mark_capture, mark_ack);
	printk("rkp-ua: str_preserve: %d\n", n_str_preserve);
	for(ret = 0; ret < n_str_preserve; ret++)
		printk("\t%s\n", str_preserve[ret]);
	printk("rkp-ua: time_keepalive=%d, len_ua=%d\n", time_keepalive, len_ua);
	printk("rkp-ua: verbose=%c, debug=%c\n", 'n' + verbose * ('y' - 'n'), 'n' + debug * ('y' - 'n'));
	printk("rkp-ua: str_preserve: %d\n", n_str_preserve);
	printk("str_ua_rkp: %s\n", str_uaRkp);

	return 0;
}

static void __exit hook_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
	nf_unregister_net_hooks(&init_net, nfho, 3);
#else
	nf_unregister_hooks(nfho, 3);
#endif
	if(rkpm != 0)
		rkpManager_delete(rkpm);
	printk("rkp-ua: Stopped.\n");
}

module_init(hook_init);
module_exit(hook_exit);

MODULE_AUTHOR("Haonan Chen");
MODULE_DESCRIPTION("Modify UA in HTTP for anti-detection about amount of devices behind NAT.");
MODULE_LICENSE("GPL");
