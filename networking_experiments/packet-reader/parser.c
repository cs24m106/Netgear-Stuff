// To read hex dump files without offset formatting
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <net/ethernet.h>

#define ETHERTYPE_QINQ 0x88a8 // not defined in std libs
#define header_scale 4
#define fragment_scale 8

static uint n = 0;

// Structs for internal organization
struct eth_info {
    uint8_t d_mac[6]; // destination mac addres
    uint8_t s_mac[6]; // source mac address
    uint16_t type; // ethertype
};

struct arp_info {
    uint16_t hrd, pro, op; // hardwaretype, protocoltype, operation
    uint8_t hln, pln; // hardwarelength, protocollength
    uint8_t sha[6], tha[6]; // sourcehardwareaddress, targethardwareaddress
    uint32_t spa, tpa; // sourceprotocoladdress, targetprotocoladdress
};

struct ip_info {
    uint8_t v, hl, tos, ttl, p; // version, headerlength, typeofservice, timetoleave, protocol
    uint16_t len, id, off, sum; // totallength, identificationno, fragmentoffset, checksum
    struct in_addr src, dst; // ipv4's source & destination address pair
};

// Helper to read a specific number of hex bytes from file and return as uint32 
uint read_hex(FILE *fp, int bytes) {
    if (bytes>4){
        fprintf(stderr, "Read Hex overflow request for %d Bytes!", bytes); // current max requirement check
        exit(1);
    }
    uint val = 0, tmp;
    for (int i = 0; i < bytes; i++) {
        if (fscanf(fp, "%2x", &tmp) != 1) return 0;
        val = (val << 8) | (tmp & 0xFF);
    }
    return val;
}


void process_packet(FILE *fp) {
    printf("\n=== PACKET FRAME %d ===\n", ++n);

    // --- Layer 2: Ethernet Header ---
    struct eth_info eth;
    for(int i=0; i<6; i++) eth.d_mac[i] = (uint8_t)read_hex(fp, 1);
    for(int i=0; i<6; i++) eth.s_mac[i] = (uint8_t)read_hex(fp, 1);
    eth.type = (uint16_t)read_hex(fp, 2);

    printf("[L2 Ethernet]\n");
    printf("\t|-Source MAC      : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           eth.s_mac[0], eth.s_mac[1], eth.s_mac[2], eth.s_mac[3], eth.s_mac[4], eth.s_mac[5]);
    printf("\t|-Destination MAC : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           eth.d_mac[0], eth.d_mac[1], eth.d_mac[2], eth.d_mac[3], eth.d_mac[4], eth.d_mac[5]);

    uint16_t current_eth_type = eth.type;
    uint eth_header_len = 14; // SA(6)+DA(6)+Len(2) --> note CRC(4) is trailer after data payload, thus not included
    int vlan_count = 0;

    while (current_eth_type == ETHERTYPE_VLAN || current_eth_type == ETHERTYPE_QINQ) {
        uint16_t tci = (uint16_t)read_hex(fp, 2);
        uint16_t next_type = (uint16_t)read_hex(fp, 2);
        vlan_count++;

        printf("\t[VLAN Tag #%d] => ", vlan_count);
        printf("|Protocol(TPID):0x%04X", current_eth_type);
        printf("|Priority(PCP):%d", (tci >> 13) & 0x07);
        printf("|Drop-Eligible(DEI):%d", (tci >> 12) & 0x01);
        printf("|VID:%d\n", (tci & 0x0FFF));

        current_eth_type = next_type;
        eth_header_len += 4;
    }
    printf("\t|-EtherType  : 0x%04X\n", current_eth_type);

    // --- L3 ARP & RARP Handling ---
    if (current_eth_type == ETHERTYPE_ARP || current_eth_type == ETHERTYPE_REVARP) {
        struct arp_info arp;
        arp.hrd = (uint16_t)read_hex(fp, 2);
        arp.pro = (uint16_t)read_hex(fp, 2);
        arp.hln = (uint8_t)read_hex(fp, 1);
        arp.pln = (uint8_t)read_hex(fp, 1);
        arp.op  = (uint16_t)read_hex(fp, 2);

        printf("[%s Header]\n", (current_eth_type == ETHERTYPE_ARP) ? "ARP" : "RARP");
        printf("\t|-Hardware Type     : %d (Ethernet=1)\n", arp.hrd);
        printf("\t|-Protocol Type     : 0x%04X (IPv4=0800)\n", arp.pro);
        printf("\t|-Hardware Size     : %d\n", arp.hln);
        printf("\t|-Protocol Size     : %d\n", arp.pln);
        printf("\t|-Opcode            : %d ", arp.op);
        
        if(arp.op == 1) printf("(ARP Request)\n");
        else if(arp.op == 2) printf("(ARP Reply)\n");
        else if(arp.op == 3) printf("(RARP Request)\n");
        else if(arp.op == 4) printf("(RARP Reply)\n");

        for(int i=0; i<6; i++) arp.sha[i] = read_hex(fp, 1);
        arp.spa = read_hex(fp, 4);
        for(int i=0; i<6; i++) arp.tha[i] = read_hex(fp, 1);
        arp.tpa = read_hex(fp, 4);

        struct in_addr sa = {htonl(arp.spa)}, ta = {htonl(arp.tpa)};
        printf("\t|-Sender MAC        : %02X:%02X:%02X:%02X:%02X:%02X\n", arp.sha[0],arp.sha[1],arp.sha[2],arp.sha[3],arp.sha[4],arp.sha[5]);
        printf("\t|-Sender IP         : %s\n", inet_ntoa(sa));
        printf("\t|-Target MAC        : %02X:%02X:%02X:%02X:%02X:%02X\n", arp.tha[0],arp.tha[1],arp.tha[2],arp.tha[3],arp.tha[4],arp.tha[5]);
        printf("\t|-Target IP         : %s\n", inet_ntoa(ta));
        return;
    }

    if (current_eth_type != ETHERTYPE_IP) {
        printf("Unknown EtherType! Valid types = IPv4:%04X, skipping unpacking further...\n", ETHERTYPE_IP);
        return;
    }

    // --- Layer 3: IPv4 Header ---
    struct ip_info ip;
    uint8_t ver_ihl = (uint8_t)read_hex(fp, 1);
    ip.v = (ver_ihl >> 4);
    ip.hl = (ver_ihl & 0x0F);
    ip.tos = (uint8_t)read_hex(fp, 1);
    ip.len = (uint16_t)read_hex(fp, 2);
    ip.id  = (uint16_t)read_hex(fp, 2);
    ip.off = (uint16_t)read_hex(fp, 2);
    ip.ttl = (uint8_t)read_hex(fp, 1);
    ip.p   = (uint8_t)read_hex(fp, 1);
    ip.sum = (uint16_t)read_hex(fp, 2);
    ip.src.s_addr = htonl(read_hex(fp, 4));
    ip.dst.s_addr = htonl(read_hex(fp, 4));

    uint ip_hdr_bytes = ip.hl * header_scale;
    uint ip_frag_offset = (ip.off & 0x1FFF) * fragment_scale;

    printf("[L3 IPv4]\n");
    printf("\t|-IP Version        : %d\n", ip.v);
    printf("\t|-Header Length => Offset:%d * ScalingFactor:%d = %d Bytes\n", ip.hl, header_scale, ip_hdr_bytes);
    printf("\t|-Type Of Service   : %d\n", ip.tos);
    printf("\t|-Total Length      : %d Bytes\n", ip.len);
    printf("\t|-Identification    : %d\n", ip.id);
    printf("\t[Flags] => |Reserved-Bit:%d |Dont-Fragment:%d|More-Fragments:%d|\n", 
        (ip.off >> 15), (ip.off & 0x4000) >> 14, (ip.off & 0x2000) >> 13);
    printf("\t|-Fragment Offset  => Offset:%d * ScalingFactor:%d = %d\n", (ip.off & 0x1FFF), fragment_scale, ip_frag_offset);
    printf("\t|-TTL               : %d\n", ip.ttl);
    printf("\t|-Protocol          : %d\n", ip.p);
    printf("\t|-Header Checksum   : %d\n", ip.sum);
    printf("\t|-Source IP         : %s\n", inet_ntoa(ip.src));
    printf("\t|-Destination IP    : %s\n", inet_ntoa(ip.dst));

    for(int i = 0; i < (int)(ip_hdr_bytes - 20); i++) read_hex(fp, 1); 

    // --- Layer 4: Transport/Control Layer ---
    uint l4_header_len = 0;
    uint app_header_len = 0;

    if (ip.p == IPPROTO_TCP) {
        uint16_t src_port = read_hex(fp, 2);
        uint16_t dst_port = read_hex(fp, 2);
        uint32_t seq = read_hex(fp, 4);
        uint32_t ack = read_hex(fp, 4);
        uint16_t off_flags = read_hex(fp, 2);
        uint8_t th_off = (off_flags >> 12);
        uint8_t th_flags = (off_flags & 0xFF);
        uint16_t win = read_hex(fp, 2), sum = read_hex(fp, 2), urp = read_hex(fp, 2);

        l4_header_len = th_off * header_scale;
        printf("[L4 TCP]\n");
        printf("\t|-Source Port       : %u\n", src_port);
        printf("\t|-Destination Port  : %u\n", dst_port);
        printf("\t|-Sequence No.      : %u\n", seq);
        printf("\t|-Acknowledge No.   : %u\n", ack);
        printf("\t|-Header Length => Offset:%d * ScalingFactor:%d = %d Bytes\n", th_off, header_scale, l4_header_len);
        printf("\t|-Flags => |URG:%d|ACK:%d|PSH:%d|RST:%d|SYN:%d|FIN:%d|\n", 
               (th_flags&0x20)>>5, (th_flags&0x10)>>4, (th_flags&0x08)>>3, (th_flags&0x04)>>2, (th_flags&0x02)>>1, (th_flags&0x01));
        printf("\t|-Window Size       : %d\n", win);
        printf("\t|-Checksum          : %d\n", sum);
        printf("\t|-Urgent Pointer    : %d\n", urp);
        for(int i = 0; i < (int)(l4_header_len - 20); i++) read_hex(fp, 1);
    } 
    else if (ip.p == IPPROTO_UDP) {
        uint16_t src_port = read_hex(fp, 2);
        uint16_t dst_port = read_hex(fp, 2);
        uint16_t len = read_hex(fp, 2);
        uint16_t sum = read_hex(fp, 2);
        l4_header_len = 8;
        printf("[L4 UDP]\n");
        printf("\t|-Source Port       : %u\n", src_port);
        printf("\t|-Destination Port  : %u\n", dst_port);
        printf("\t|-UDP Length        : %u\n", len);
        printf("\t|-Checksum          : %d\n", sum);

        // DNS Handling (Application Layer inside UDP)
        if (src_port == 53 || dst_port == 53) {
            printf("[L7 DNS Header]\n");
            printf("\t|-Transaction ID    : 0x%04X\n", (uint16_t)read_hex(fp, 2));
            uint16_t dns_flags = (uint16_t)read_hex(fp, 2);
            printf("\t|-Flags             : 0x%04X (%s)\n", dns_flags, (dns_flags & 0x8000) ? "Response" : "Query");
            printf("\t|-Questions         : %u\n", (uint16_t)read_hex(fp, 2));
            printf("\t|-Answer RRs        : %u\n", (uint16_t)read_hex(fp, 2));
            printf("\t|-Authority RRs     : %u\n", (uint16_t)read_hex(fp, 2));
            printf("\t|-Additional RRs    : %u\n", (uint16_t)read_hex(fp, 2));
            app_header_len = 12; // DNS header is 12 bytes
        }
    }
    else if (ip.p == 1) { // ICMP
        uint8_t type = read_hex(fp, 1);
        uint8_t code = read_hex(fp, 1);
        uint16_t cksum = read_hex(fp, 2);
        uint32_t rest = read_hex(fp, 4);
        l4_header_len = 8;
        printf("[L4 ICMP]\n");
        printf("\t|-Type              : %d ", type);
        if(type==8) printf("(Echo Request)\n"); else if(type==0) printf("(Echo Reply)\n");
        else if(type==3) printf("(Dest Unreachable)\n"); else if(type==11) printf("(Time Exceeded)\n");
        else printf("(Other)\n");
        printf("\t|-Code              : %d\n", code);
        printf("\t|-Checksum          : %d\n", cksum);
        if(type==0 || type==8) {
            printf("\t|-Identifier        : %d\n", rest >> 16);
            printf("\t|-Sequence Number   : %d\n", rest & 0xFFFF);
        }
    }
    else if (ip.p == 2) { // IGMP
        uint8_t type = read_hex(fp, 1);
        uint8_t mrtc = read_hex(fp, 1);
        uint16_t cksum = read_hex(fp, 2);
        uint32_t group = read_hex(fp, 4);
        struct in_addr gaddr = {htonl(group)};
        l4_header_len = 8;
        printf("[L4 IGMP]\n");
        printf("\t|-Type              : 0x%02X ", type);
        if(type==0x11) printf("(Membership Query)\n"); else if(type==0x16) printf("(v2 Report)\n");
        else if(type==0x17) printf("(Leave Group)\n"); else if(type==0x22) printf("(v3 Report)\n");
        printf("\t|-Max Response Time : %d\n", mrtc);
        printf("\t|-Checksum          : %d\n", cksum);
        printf("\t|-Group Address     : %s\n", inet_ntoa(gaddr));
    }

    // --- 4. Final Payload ---
    int payload_len = ip.len - ip_hdr_bytes - l4_header_len - app_header_len;
    if (payload_len > 0) {
        printf("[Payload (%d bytes)]\n  \"", payload_len);
        for(int i = 0; i < payload_len; i++) {
            u_char c = (u_char)read_hex(fp, 1);
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        printf("\"\n");
    } else {
        printf("[No Extra Payload Found]\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) return printf("Usage: %s <hex_text_file>\n", argv[0]), 1;
    FILE *fp = fopen(argv[1], "r");
    if (!fp) return perror("File error"), 1;

    uint peek;
    while (fscanf(fp, "%2x", &peek) == 1) {
        fseek(fp, -2, SEEK_CUR); // Back up so process_packet can read the full byte
        process_packet(fp);
    }

    fclose(fp);
    return 0;
}
