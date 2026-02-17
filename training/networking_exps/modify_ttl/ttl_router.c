#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/skbuff.h>
#include <linux/inet.h>
#include <net/route.h>
#include <net/ip.h>

static unsigned int hook_func(void *priv, struct sk_buff *skb,
                              const struct nf_hook_state *state)
{
    struct iphdr *iph;
    struct sk_buff *reply_skb;
    struct iphdr *reply_iph;
    struct icmphdr *reply_icmph;
    __be32 orig_daddr;
    
    // Get IP header
    iph = ip_hdr(skb);
    if (!iph)
        return NF_ACCEPT;
    
    // Save original destination before modification
    orig_daddr = iph->daddr;
    
    // Decrement TTL
    iph->ttl--;
    
    // Check if TTL expired
    if (iph->ttl <= 0) {
        printk(KERN_INFO "ROUTER: TTL EXPIRED (was 1) from %pI4 - Dropping packet\n",
               &iph->saddr);
        
        // Allocate skb for ICMP Time Exceeded reply
        reply_skb = alloc_skb(sizeof(struct iphdr) + sizeof(struct icmphdr) + 28, GFP_ATOMIC);
        if (!reply_skb)
            return NF_DROP;
        
        // Build ICMP header
        reply_icmph = (struct icmphdr *)skb_put(reply_skb, sizeof(struct icmphdr));
        reply_icmph->type = ICMP_TIME_EXCEEDED;
        reply_icmph->code = ICMP_EXC_TTL;
        reply_icmph->checksum = 0;
        reply_icmph->checksum = ip_compute_csum((void *)reply_icmph, sizeof(struct icmphdr));
        
        // Build IP header for reply
        reply_iph = (struct iphdr *)skb_push(reply_skb, sizeof(struct iphdr));
        reply_iph->version = 4;
        reply_iph->ihl = 5;
        reply_iph->tos = 0;
        reply_iph->tot_len = htons(reply_skb->len);
        reply_iph->id = htons(0);
        reply_iph->frag_off = 0;
        reply_iph->ttl = 64;
        reply_iph->protocol = IPPROTO_ICMP;
        reply_iph->saddr = orig_daddr;  // Our "router" IP
        reply_iph->daddr = iph->saddr;  // Original sender
        reply_iph->check = 0;
        reply_iph->check = ip_fast_csum((void *)reply_iph, reply_iph->ihl);
        
        // Send ICMP reply
        ip_local_out(state->net, state->sk, reply_skb);
        
        return NF_DROP;  // Drop original packet
    }
    
    // Recalculate IP checksum after TTL change
    iph->check = 0;
    iph->check = ip_fast_csum((void *)iph, iph->ihl);
    
    printk(KERN_INFO "ROUTER: Forwarded packet from %pI4 TTL=%u\n",
           &iph->saddr, iph->ttl);
    
    return NF_ACCEPT;
}

static struct nf_hook_ops nfho = {
    .hook = hook_func,
    .hooknum = NF_INET_FORWARD,  // Critical: Use FORWARD hook for router behavior
    .pf = NFPROTO_IPV4,
    .priority = NF_IP_PRI_FIRST
};

static int __init ttl_router_init(void)
{
    nf_register_net_hook(&init_net, &nfho);
    printk(KERN_INFO "TTL Router Module: LOADED (simulating router hop)\n");
    return 0;
}

static void __exit ttl_router_exit(void)
{
    nf_unregister_net_hook(&init_net, &nfho);
    printk(KERN_INFO "TTL Router Module: UNLOADED\n");
}

module_init(ttl_router_init);
module_exit(ttl_router_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Logesh");
MODULE_DESCRIPTION("Task 6: TTL Decrement Router Simulation");