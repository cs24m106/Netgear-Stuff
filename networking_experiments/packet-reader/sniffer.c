// gcc sniffer.c -lpcap
#define _DEFAULT_SOURCE  // Enables BSD-style struct definitions on Linux
#include <stdio.h>
#include <pcap.h>
#include <arpa/inet.h>
#include <netinet/in.h>      // Required for IP address structures
#include <netinet/ip.h>       // For L3 IPv4
#include <netinet/tcp.h>      // For L4 TCP
#include <netinet/udp.h>      // For L4 UDP
#include <net/ethernet.h>    // DEFINES: struct ether_header, ETHERTYPE_IP

#define header_scale 4 // header length field scale for ip and tcp
#define fragment_scale 8 // fragment offset field scale for ip

static uint n = 0;
void process_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet) {
    printf("\n=== PACKET FRAME %d ===\n", ++n);

    // 1. Layer 2: Ethernet Header
    struct ether_header *eth = (struct ether_header *) packet;
    printf("[L2 Ethernet]\n");
    printf("\t|-Source MAC      : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           eth->ether_shost[0], eth->ether_shost[1], eth->ether_shost[2], 
           eth->ether_shost[3], eth->ether_shost[4], eth->ether_shost[5]);
    printf("\t|-Destination MAC : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           eth->ether_dhost[0], eth->ether_dhost[1], eth->ether_dhost[2], 
           eth->ether_dhost[3], eth->ether_dhost[4], eth->ether_dhost[5]);
    printf("\t|-EtherType       : 0x%04X\n", ntohs(eth->ether_type));

    if (ntohs(eth->ether_type) != ETHERTYPE_IP) {
        printf("Unknown EtherType! Valid types = IPv4:%04X, skipping unpacking further...\n", ETHERTYPE_IP);
        return;
    }

    // 2. Layer 3: IPv4 Header
    struct ip *ip = (struct ip *)(packet + 14);
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

    printf("\t|-Flags => |-Reserved Bit:%d|-Don't Fragment:%d|-More Fragments:%d|\n", 
        (ip_frag_off_field& IP_RF) >> 15, (ip_frag_off_field & IP_DF) >> 14, (ip_frag_off_field & IP_MF) >> 13);

    printf("\t|-Fragment Offset  => Offset:%d * ScalingFactor:%d = %d\n",
            (uint)(ip_frag_off_field & IP_OFFMASK), fragment_scale, ip_fragment_offset);

    printf("\t|-TTL               : %d\n", (uint)ip->ip_ttl);
    printf("\t|-Protocol          : %d\n", (uint)ip->ip_p);
    printf("\t|-Header Checksum   : %d\n", ntohs(ip->ip_sum));
    printf("\t|-Source IP         : %s\n", inet_ntoa(ip->ip_src));
    printf("\t|-Destination IP    : %s\n", inet_ntoa(ip->ip_dst));

    // 3. Layer 4: Transport Layer (Switch based on ip->ip_p)
    uint l4_header_len = 0;
    const u_char *l4_start = packet + 14 + ip_header_len;

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

            printf("\t|-Flags => |-URG:%d|-ACK:%d|-PSH:%d|-RST:%d|-SYN:%d|-FIN:%d|\n", 
                    (tcp->th_flags & TH_URG) ? 1 : 0, (tcp->th_flags & TH_ACK) ? 1 : 0, (tcp->th_flags & TH_PUSH) ? 1 : 0,
                    (tcp->th_flags & TH_RST) ? 1 : 0, (tcp->th_flags & TH_SYN) ? 1 : 0, (tcp->th_flags & TH_FIN) ? 1 : 0);
            
            printf("\t|-Window Size       : %d\n", ntohs(tcp->window));
            printf("\t|-Checksum          : %d\n", ntohs(tcp->check));
            printf("\t|-Urgent Pointer    : %d\n", tcp->urg_ptr);
            break;
        }
        case IPPROTO_UDP: {
            struct udphdr *udp = (struct udphdr *)l4_start;
            l4_header_len = 8; // UDP is always 8 bytes
            printf("[L4 UDP]\n");
            printf("  |-Source Port       : %u\n", ntohs(udp->source));
            printf("  |-Destination Port  : %u\n", ntohs(udp->dest));
            printf("  |-UDP Length        : %u\n", ntohs(udp->len));
            printf("  |-Checksum          : %d\n", ntohs(udp->check));
            break;
        }
        default:
            printf("Protocol Not Supported! Valid types = TCP:%d, UDP:%d, skipping unpacking further...\n", 
                IPPROTO_TCP, IPPROTO_UDP);
            return;
    }

    // 4. Final Payload
    uint total_headers = 14 + ip_header_len + l4_header_len;
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

int main() {
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *handle = pcap_open_offline("packet.pcap", errbuf); // Open your file
    if (!handle) return 1;

    pcap_loop(handle, 1, process_packet, NULL); // Process one packet
    pcap_close(handle);
    return 0;
}
