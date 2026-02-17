#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/skbuff.h>

static unsigned int hook_func(void *priv, struct sk_buff *skb,
                              const struct nf_hook_state *state)
{
    struct iphdr *iph;
    struct icmphdr *icmph;
    
    // Get IP header
    iph = ip_hdr(skb);
    if (!iph)
        return NF_ACCEPT;
    
    // Check for ICMP protocol (protocol number 1)
    if (iph->protocol == IPPROTO_ICMP) {
        // Get ICMP header
        icmph = icmp_hdr(skb);
        if (!icmph)
            return NF_ACCEPT;
        
        // Log blocked ICMP type (e.g., 8 = Echo Request)
        printk(KERN_INFO "FIREWALL: BLOCKED ICMP type=%u code=%u from %pI4\n",
               icmph->type, icmph->code, &iph->saddr);
        
        // DROP the packet (firewall action)
        return NF_DROP;
    }
    
    // Allow all other traffic (TCP/UDP/etc.)
    return NF_ACCEPT;
}

static struct nf_hook_ops nfho = {
    .hook = hook_func,
    .hooknum = NF_INET_PRE_ROUTING,
    .pf = NFPROTO_IPV4,
    .priority = NF_IP_PRI_FILTER  // Standard firewall priority
};

static int __init icmp_block_init(void)
{
    nf_register_net_hook(&init_net, &nfho);
    printk(KERN_INFO "ICMP Firewall Module: LOADED (blocking all ICMP)\n");
    return 0;
}

static void __exit icmp_block_exit(void)
{
    nf_unregister_net_hook(&init_net, &nfho);
    printk(KERN_INFO "ICMP Firewall Module: UNLOADED\n");
}

module_init(icmp_block_init);
module_exit(icmp_block_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Logesh");
MODULE_DESCRIPTION("Task 5: ICMP Firewall using Netfilter");