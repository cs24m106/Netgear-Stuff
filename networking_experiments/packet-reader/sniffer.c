// gcc sniffer.c -lpcap
// to read pcap files
#define _DEFAULT_SOURCE  // Enables BSD-style struct definitions on Linux
#include <stdio.h>
#include <pcap.h>
#include <arpa/inet.h>
#include <netinet/in.h>      // Required for IP address structures
#include <net/ethernet.h>    // DEFINES: struct ether_header, ETHERTYPE_IP
#include <net/if_arp.h>      // Required for struct arphdr
#include <netinet/ip.h>       // For L3 IPv4
#include <netinet/tcp.h>      // For L4 TCP
#include <netinet/udp.h>      // For L4 UDP
#include <netinet/ip_icmp.h>  // For ICMP structs
#include <netinet/igmp.h>     // For IGMP structs

#define ETHERTYPE_QINQ 0x88a8 // not defined in std libs
#define header_scale 4 // header length field scale for ip and tcp
#define fragment_scale 8 // fragment offset field scale for ip

static uint n = 0;
void process_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet) {
    printf("\n=== PACKET FRAME %d ===\n", ++n);

    // Layer 2: Ethernet Header
    struct ether_header *eth = (struct ether_header *) packet;
    printf("[L2 Ethernet]\n");
    printf("\t|-Source MAC      : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           eth->ether_shost[0], eth->ether_shost[1], eth->ether_shost[2], 
           eth->ether_shost[3], eth->ether_shost[4], eth->ether_shost[5]);
    printf("\t|-Destination MAC : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           eth->ether_dhost[0], eth->ether_dhost[1], eth->ether_dhost[2], 
           eth->ether_dhost[3], eth->ether_dhost[4], eth->ether_dhost[5]);
           
    uint16_t current_eth_type = ntohs(eth->ether_type);
    uint eth_header_len = 14; int vlan_count = 0;

    // Loop while the EtherType matches either standard VLAN (0x8100) or Q-in-Q (0x88a8)
    while (current_eth_type == ETHERTYPE_VLAN || current_eth_type == ETHERTYPE_QINQ) {
        struct vlan_tag {
            uint16_t tci;       // Tag Control Information
            uint16_t next_type; // Encapsulated EtherType
        } *vlan = (struct vlan_tag *)(packet + eth_header_len);

        uint16_t tci = ntohs(vlan->tci);
        vlan_count++;

        printf("\t[VLAN Tag #%d] => ", vlan_count);
        printf("|Protocol(TPID):0x%04X",current_eth_type);
        printf("|Priority(PCP):%d", (tci >> 13) & 0x07); // first 3 bits = pcp (shift 16-3)
        printf("|Drop-Eligible(DEI):%d", (tci >> 12) & 0x01); // second 1 bit = dei (shift 16-(3+1))
        printf("|VID:%d\n", (tci & 0x0FFF)); // last 12 bits mask = vlan number

        // Update current EtherType to the one inside this VLAN tag
        current_eth_type = ntohs(vlan->next_type);
        // Advance the offset by 4 bytes (size of a VLAN tag)
        eth_header_len += 4;
    }
    printf("\t|-EtherType  : 0x%04X\n",current_eth_type);

    // L3^ ARP & RARP Handling
    if (current_eth_type == ETHERTYPE_ARP || current_eth_type == ETHERTYPE_REVARP) {
        struct arphdr *arp = (struct arphdr *)(packet + eth_header_len);
        
        printf("[%s Header]\n", (current_eth_type == ETHERTYPE_ARP) ? "ARP" : "RARP");
        printf("\t|-Hardware Type     : %d (Ethernet=1)\n", ntohs(arp->ar_hrd));
        printf("\t|-Protocol Type     : 0x%04X (IPv4=0800)\n", ntohs(arp->ar_pro));
        printf("\t|-Hardware Size     : %d\n", arp->ar_hln);
        printf("\t|-Protocol Size     : %d\n", arp->ar_pln);
        
        uint16_t op = ntohs(arp->ar_op);
        printf("\t|-Opcode            : %d ", op);
        if(op == ARPOP_REQUEST) printf("(ARP Request)\n");
        else if(op == ARPOP_REPLY) printf("(ARP Reply)\n");
        else if(op == ARPOP_RREQUEST) printf("(RARP Request)\n");
        else if(op == ARPOP_RREPLY) printf("(RARP Reply)\n");
        else printf("(Unknown)\n");

        // ARP/RARP addresses follow the fixed header
        unsigned char *sha = (unsigned char *)(arp + 1);            // Sender Hardware Address
        unsigned char *spa = sha + arp->ar_hln;                     // Sender Protocol Address
        unsigned char *tha = spa + arp->ar_pln;                     // Target Hardware Address
        unsigned char *tpa = tha + arp->ar_hln;                     // Target Protocol Address

        printf("\t|-Sender MAC        : %02X:%02X:%02X:%02X:%02X:%02X\n", sha[0],sha[1],sha[2],sha[3],sha[4],sha[5]);
        printf("\t|-Sender IP         : %d.%d.%d.%d\n", spa[0], spa[1], spa[2], spa[3]);
        printf("\t|-Target MAC        : %02X:%02X:%02X:%02X:%02X:%02X\n", tha[0],tha[1],tha[2],tha[3],tha[4],tha[5]);
        printf("\t|-Target IP         : %d.%d.%d.%d\n", tpa[0], tpa[1], tpa[2], tpa[3]);
        return; // Finished processing ARP/RARP
    }
    
    if (current_eth_type != ETHERTYPE_IP) {
        printf("Unknown EtherType! Valid types = IPv4:%04X, skipping unpacking further...\n", ETHERTYPE_IP);
        return;
    }
    
    // Layer 3: IPv4 Header (Adjusted offset for possible VLAN tag)
    struct ip *ip = (struct ip *)(packet + eth_header_len);
    uint ip_header_len = ip->ip_hl * header_scale;
    uint16_t ip_frag_off_field = ntohs(ip->ip_off); 
    uint ip_fragment_offset = (ip_frag_off_field & IP_OFFMASK) * fragment_scale;
    printf("[L3 IPv4]\n");
    printf("\t|-IP Version        : %d\n", (uint)ip->ip_v);

    printf("\t|-Header Length => Offset:%d * ScalingFactor:%d = %d Bytes\n",
            (uint)ip->ip_hl, header_scale, ip_header_len);

    printf("\t|-Type Of Service   : %d\n", (uint)ip->ip_tos);
    printf("\t|-Total Length      : %d Bytes\n", ntohs(ip->ip_len));
    printf("\t|-Identification    : %d\n", ntohs(ip->ip_id));

    printf("\t[Flags] => |Reserved-Bit:%d|Dont-Fragment:%d|More-Fragments:%d|\n", 
        (ip_frag_off_field& IP_RF) >> 15, (ip_frag_off_field & IP_DF) >> 14, (ip_frag_off_field & IP_MF) >> 13);

    printf("\t|-Fragment Offset  => Offset:%d * ScalingFactor:%d = %d\n",
            (uint)(ip_frag_off_field & IP_OFFMASK), fragment_scale, ip_fragment_offset);

    printf("\t|-TTL               : %d\n", (uint)ip->ip_ttl);
    printf("\t|-Protocol          : %d\n", (uint)ip->ip_p);
    printf("\t|-Header Checksum   : %d\n", ntohs(ip->ip_sum));
    printf("\t|-Source IP         : %s\n", inet_ntoa(ip->ip_src));
    printf("\t|-Destination IP    : %s\n", inet_ntoa(ip->ip_dst));

    // Layer 4: Transport Layer || Control Layer (Switch based on ip->ip_p)
    uint l4_header_len = 0;
    const u_char *l4_start = packet + eth_header_len + ip_header_len;

    switch (ip->ip_p) {
        case IPPROTO_TCP: {
            struct tcphdr *tcp = (struct tcphdr *)l4_start;
            l4_header_len = tcp->doff * header_scale;
            printf("[L4 TCP]\n");
            printf("\t|-Source Port       : %u\n", ntohs(tcp->source));
            printf("\t|-Destination Port  : %u\n", ntohs(tcp->dest));
            printf("\t|-Sequence No.      : %u\n", ntohl(tcp->seq));
            printf("\t|-Acknowledge No.   : %u\n", ntohl(tcp->ack_seq));

            printf("\t|-Header Length => Offset:%d * ScalingFactor:%d = %d Bytes\n",
                    (uint)tcp->doff, header_scale, l4_header_len);

            printf("\t|-Flags => |URG:%d|ACK:%d|PSH:%d|RST:%d|SYN:%d|FIN:%d|\n", 
                    (tcp->th_flags & TH_URG) ? 1 : 0, (tcp->th_flags & TH_ACK) ? 1 : 0, (tcp->th_flags & TH_PUSH) ? 1 : 0,
                    (tcp->th_flags & TH_RST) ? 1 : 0, (tcp->th_flags & TH_SYN) ? 1 : 0, (tcp->th_flags & TH_FIN) ? 1 : 0);
            
            printf("\t|-Window Size       : %d\n", ntohs(tcp->window));
            printf("\t|-Checksum          : %d\n", ntohs(tcp->check));
            printf("\t|-Urgent Pointer    : %d\n", tcp->urg_ptr);
            break;
        }

        case IPPROTO_ICMP: {
            struct icmphdr *icmp = (struct icmphdr *)l4_start;
            l4_header_len = 8; // ICMP header is 8 bytes
            printf("[L4 ICMP]\n");
            
            // 1. Basic ICMP Fields
            printf("\t|-Type     : %d ", icmp->type);
            
            // Decoding the Type
            switch(icmp->type) {
                case ICMP_ECHOREPLY:     printf("(Echo Reply)\n"); break;
                case ICMP_DEST_UNREACH:  printf("(Destination Unreachable)\n"); break;
                case ICMP_REDIRECT:      printf("(Redirect / Routing Error)\n"); break;
                case ICMP_ECHO:          printf("(Echo Request)\n"); break;
                case ICMP_TIME_EXCEEDED: printf("(Time Exceeded / TTL Expired)\n"); break;
                default:                 printf("(Other / Feedback)\n"); break;
            }

            printf("\t|-Code     : %d\n", (uint)icmp->code);
            printf("\t|-Checksum : %d\n", ntohs(icmp->checksum));

            // 2. Specialized Format Handling
            // Echo Request/Reply (Ping)
            if (icmp->type == ICMP_ECHO || icmp->type == ICMP_ECHOREPLY) {
                printf("\t|-Identifier : %d\n", ntohs(icmp->un.echo.id));
                printf("\t|-Sequence   : %d\n", ntohs(icmp->un.echo.sequence));
            } 
            // Gateway Redirect
            else if (icmp->type == ICMP_REDIRECT) {
                struct in_addr gw;
                gw.s_addr = icmp->un.gateway;
                printf("\t|-Gateway Addr: %s\n", inet_ntoa(gw));
            }
            // Error handling (Destination Unreachable / Time Exceeded)
            else if (icmp->type == ICMP_DEST_UNREACH || icmp->type == ICMP_TIME_EXCEEDED) {
                printf("\t|-Next-Hop MTU: %d (if applicable)\n", ntohs(icmp->un.frag.mtu));
                printf("\t[Note] This packet contains a copy of the original failed IP header.\n");
            }
            break;
        }

        case IPPROTO_IGMP: {
            struct igmp *igmp = (struct igmp *)l4_start;
            l4_header_len = 8;
            printf("[L4 IGMP]\n");
            
            printf("\t|-Type              : 0x%02X ", igmp->igmp_type);
            switch(igmp->igmp_type) {
                case IGMP_MEMBERSHIP_QUERY:     printf("(Membership Query)\n"); break;
                case IGMP_V1_MEMBERSHIP_REPORT: printf("(v1 Membership Report)\n"); break;
                case IGMP_V2_MEMBERSHIP_REPORT: printf("(v2 Membership Report)\n"); break;
                case IGMP_V2_LEAVE_GROUP:       printf("(Leave Group)\n"); break;
                case 0x22:                      printf("(v3 Membership Report)\n"); break;
                default:                        printf("(Unknown IGMP Type)\n"); break;
            }

            printf("\t|-Max Response Time : %d\n", igmp->igmp_code);
            printf("\t|-Checksum          : %d\n", ntohs(igmp->igmp_cksum));
            printf("\t|-Group Address     : %s\n", inet_ntoa(igmp->igmp_group));
            break;
        }

        case IPPROTO_UDP: {
            struct udphdr *udp = (struct udphdr *)l4_start;
            l4_header_len = 8; // UDP is always 8 bytes
            uint16_t src_port = ntohs(udp->source);
            uint16_t dst_port = ntohs(udp->dest);
            printf("[L4 UDP]\n");
            printf("\t|-Source Port       : %u\n", src_port);
            printf("\t|-Destination Port  : %u\n", dst_port);
            printf("\t|-UDP Length        : %u\n", ntohs(udp->len));
            printf("\t|-Checksum          : %d\n", ntohs(udp->check));
            
            // --- DNS Handling ---
            if (src_port == 53 || dst_port == 53) {
                const u_char *dns_start = l4_start + l4_header_len;
                printf("[L7 DNS Header]\n");
                printf("\t|-Transaction ID    : 0x%04X\n", ntohs(*(uint16_t*)(dns_start)));
                
                uint16_t flags = ntohs(*(uint16_t*)(dns_start + 2));
                printf("\t|-Flags             : 0x%04X ", flags);
                printf("(%s)\n", (flags & 0x8000) ? "Response" : "Query");

                printf("\t|-Questions         : %u\n", ntohs(*(uint16_t*)(dns_start + 4)));
                printf("\t|-Answer RRs        : %u\n", ntohs(*(uint16_t*)(dns_start + 6)));
                printf("\t|-Authority RRs     : %u\n", ntohs(*(uint16_t*)(dns_start + 8)));
                printf("\t|-Additional RRs    : %u\n", ntohs(*(uint16_t*)(dns_start + 10)));
            }
            break;
        }

        default:
            printf("Protocol Not Supported! Valid types = TCP:%d, UDP:%d, skipping unpacking further...\n", 
                IPPROTO_TCP, IPPROTO_UDP);
            return;
    }

    // L5&6 Final Payload
    uint total_headers = eth_header_len + ip_header_len + l4_header_len;
    const u_char *payload = packet + total_headers;
    uint payload_len = header->caplen - total_headers;

    if (payload_len > 0) {
        printf("[Payload (%d bytes)]\n  \"", payload_len);
        for(int i = 0; i < payload_len; i++) {
            if(payload[i] >= 32 && payload[i] <= 126) printf("%c", payload[i]);
            else printf("."); // Print non-printable chars as dots
        }
        printf("\"\n");
    } else {
        printf("[No Payload Found]\n");
    }
}

int main(int argc, char *argv[])  {
    if (argc != 2) {
        printf("Usage: %s <packet_file.pcap>\n", argv[0]);
        return 1;
    }
    char errbuf[PCAP_ERRBUF_SIZE];
    
    // 1. Open the offline pcap file
    pcap_t *handle = pcap_open_offline(argv[1], errbuf); 
    if (!handle) {
        fprintf(stderr, "Error opening pcap file: %s\n", errbuf);
        return 1;
    }

    printf("Starting packet processing...\n");

    // 2. Change '1' to '0' to process all packets until EOF
    // The callback 'process_packet' will be executed for every frame found
    if (pcap_loop(handle, 0, process_packet, NULL) < 0) {
        fprintf(stderr, "pcap_loop failed: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        return 1;
    }

    printf("\nProcessing complete. Total packets handled: %u\n", n);

    // 3. Close the handle
    pcap_close(handle);
    return 0;
}
