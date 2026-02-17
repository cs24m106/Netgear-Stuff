#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/skbuff.h>

static unsigned int hook_func(void *priv, struct sk_buff *skb,
                              const struct nf_hook_state *state)
{
    struct ethhdr *eth;
    struct iphdr *iph;
    
    // Safety checks
    if (!skb || !skb_mac_header(skb) || skb->len < sizeof(struct ethhdr))
        return NF_ACCEPT;
    
    // Get Ethernet header (Layer 2)
    eth = eth_hdr(skb);
    if (!eth)
        return NF_ACCEPT;
    
    // Get IP header (Layer 3) - only for EtherType validation
    iph = ip_hdr(skb);
    
    if (eth) {
        // %pM is a special kernel formatter for MAC addresses
        printk(KERN_INFO "SRC MAC: %pM\n", eth->h_source);
        printk(KERN_INFO "DST MAC: %pM\n", eth->h_dest);
        
        // ntohs converts "Network Byte Order" to "Host Byte Order"
        printk(KERN_INFO "EtherType: 0x%04x\n", ntohs(eth->h_proto));
        
        printk(KERN_INFO "--------------------------------------\n");
    }
    
    return NF_ACCEPT;  // Allow packet to continue
}

// Netfilter hook registration
static struct nf_hook_ops nfho = {
    .hook = hook_func,
    .hooknum = NF_INET_PRE_ROUTING,  // Hook at ingress before routing
    .pf = NFPROTO_IPV4,              // IPv4 packets only
    .priority = NF_IP_PRI_FIRST      // Highest priority
};

static int __init eth_capture_init(void)
{
    nf_register_net_hook(&init_net, &nfho);
    printk(KERN_INFO "Ethernet Capture Module: LOADED\n");
    return 0;
}

static void __exit eth_capture_exit(void)
{
    nf_unregister_net_hook(&init_net, &nfho);
    printk(KERN_INFO "Ethernet Capture Module: UNLOADED\n");
}

module_init(eth_capture_init);
module_exit(eth_capture_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Logesh");
MODULE_DESCRIPTION("Task 2: Ethernet Frame Capture via Netfilter");
