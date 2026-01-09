#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <net/ethernet.h>

#define header_scale 4
#define fragment_scale 8

static uint n = 0;

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

    // 1. Layer 2: Ethernet Header
    printf("[L2 Ethernet]\n");
    
    // read MACs byte by byte to format
    uint d_mac[6], s_mac[6];
    for(int i=0; i<6; i++) d_mac[i] = read_hex(fp, 1);
    for(int i=0; i<6; i++) s_mac[i] = read_hex(fp, 1);

    printf("\t|-Source MAC      : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    printf("\t|-Destination MAC : %02X:%02X:%02X:%02X:%02X:%02X\n", 
           d_mac[0], d_mac[1], d_mac[2], d_mac[3], d_mac[4], d_mac[5]);

    uint16_t current_eth_type = (uint16_t)read_hex(fp, 2);
    printf("\t|-Next EtherType  : 0x%04X\n", current_eth_type);
    uint eth_header_len = 14;

    if (current_eth_type == ETHERTYPE_VLAN) {
        uint16_t tci = (uint16_t)read_hex(fp, 2);
        uint16_t next_type = (uint16_t)read_hex(fp, 2);

        printf("\t[802.1Q Header] => ");
        printf("|-Tag Protocol (TPID): 0x%04X", current_eth_type);
        printf("|-Priority (PCP): %d", (tci >> 13) & 0x07);
        printf("|-Drop Eligible (DEI): %d", (tci >> 12) & 0x01);
        printf("|-VLAN ID (VID): %d\n", (tci & 0x0FFF));

        current_eth_type = next_type;
        eth_header_len += 4;
    }
    printf("\t|-EtherType  : 0x%04X\n", current_eth_type);

    if (current_eth_type != ETHERTYPE_IP) {
        printf("Unknown EtherType! Valid types = IPv4:%04X, skipping unpacking further...\n", ETHERTYPE_IP);
        return;
    }

    // 2. Layer 3: IPv4 Header
    u_char ver_ihl = (u_char)read_hex(fp, 1);
    u_char ip_v = (ver_ihl >> 4);
    u_char ip_hl = (ver_ihl & 0x0F);
    
    u_char ip_tos = (u_char)read_hex(fp, 1);
    uint16_t ip_len = (uint16_t)read_hex(fp, 2);
    uint16_t ip_id = (uint16_t)read_hex(fp, 2);
    uint16_t ip_off = (uint16_t)read_hex(fp, 2);
    u_char ip_ttl = (u_char)read_hex(fp, 1);
    u_char ip_p = (u_char)read_hex(fp, 1);
    uint16_t ip_sum = (uint16_t)read_hex(fp, 2);
    
    uint32_t s_ip = (uint32_t)read_hex(fp, 4);
    uint32_t d_ip = (uint32_t)read_hex(fp, 4);

    struct in_addr src_addr = { htonl(s_ip) };
    struct in_addr dst_addr = { htonl(d_ip) };

    uint ip_header_len = ip_hl * header_scale;
    uint ip_fragment_offset = (ip_off & 0x1FFF) * fragment_scale;

    printf("[L3 IPv4]\n");
    printf("\t|-IP Version        : %d\n", ip_v);
    printf("\t|-Header Length => Offset:%d * ScalingFactor:%d = %d Bytes\n", ip_hl, header_scale, ip_header_len);
    printf("\t|-Type Of Service   : %d\n", ip_tos);
    printf("\t|-Total Length      : %d Bytes\n", ip_len);
    printf("\t|-Identification    : %d\n", ip_id);
    printf("\t[Flags] => |-Reserved Bit:%d|-Don't Fragment:%d|-More Fragments:%d|\n", 
           (ip_off >> 15), (ip_off & 0x4000) >> 14, (ip_off & 0x2000) >> 13);
    printf("\t|-Fragment Offset  => Offset:%d * ScalingFactor:%d = %d\n", (ip_off & 0x1FFF), fragment_scale, ip_fragment_offset);
    printf("\t|-TTL               : %d\n", ip_ttl);
    printf("\t|-Protocol          : %d\n", ip_p);
    printf("\t|-Header Checksum   : %d\n", ip_sum);
    printf("\t|-Source IP         : %s\n", inet_ntoa(src_addr));
    printf("\t|-Destination IP    : %s\n", inet_ntoa(dst_addr));

    // Skip IP options if any
    for(int i = 0; i < (int)(ip_header_len - 20); i++) read_hex(fp, 1);

    // 3. Layer 4: Transport Layer
    uint l4_header_len = 0;

    if (ip_p == IPPROTO_TCP) {
        uint16_t src_port = (uint16_t)read_hex(fp, 2);
        uint16_t dst_port = (uint16_t)read_hex(fp, 2);
        uint32_t seq = (uint32_t)read_hex(fp, 4);
        uint32_t ack = (uint32_t)read_hex(fp, 4);
        uint16_t off_flags = (uint16_t)read_hex(fp, 2);
        uint8_t th_off = (off_flags >> 12);
        uint8_t th_flags = (off_flags & 0xFF);
        uint16_t win = (uint16_t)read_hex(fp, 2);
        uint16_t sum = (uint16_t)read_hex(fp, 2);
        uint16_t urp = (uint16_t)read_hex(fp, 2);

        l4_header_len = th_off * header_scale;
        printf("[L4 TCP]\n");
        printf("\t|-Source Port       : %u\n", src_port);
        printf("\t|-Destination Port  : %u\n", dst_port);
        printf("\t|-Sequence No.      : %u\n", seq);
        printf("\t|-Acknowledge No.   : %u\n", ack);
        printf("\t|-Header Length => Offset:%d * ScalingFactor:%d = %d Bytes\n", th_off, header_scale, l4_header_len);
        printf("\t|-Flags => |-URG:%d|-ACK:%d|-PSH:%d|-RST:%d|-SYN:%d|-FIN:%d|\n", 
               (th_flags & 0x20)>>5, (th_flags & 0x10)>>4, (th_flags & 0x08)>>3,
               (th_flags & 0x04)>>2, (th_flags & 0x02)>>1, (th_flags & 0x01));
        printf("\t|-Window Size       : %d\n", win);
        printf("\t|-Checksum          : %d\n", sum);
        printf("\t|-Urgent Pointer    : %d\n", urp);

        for(int i = 0; i < (int)(l4_header_len - 20); i++) read_hex(fp, 1);
    } 
    else if (ip_p == IPPROTO_UDP) {
        uint16_t src_port = (uint16_t)read_hex(fp, 2);
        uint16_t dst_port = (uint16_t)read_hex(fp, 2);
        uint16_t len = (uint16_t)read_hex(fp, 2);
        uint16_t sum = (uint16_t)read_hex(fp, 2);
        l4_header_len = 8;
        printf("[L4 UDP]\n");
        printf("\t|-Source Port       : %u\n", src_port);
        printf("\t|-Destination Port  : %u\n", dst_port);
        printf("\t|-UDP Length        : %u\n", len);
        printf("\t|-Checksum          : %d\n", sum);
    }

    // 4. Final Payload
    int payload_len = ip_len - ip_header_len - l4_header_len;
    if (payload_len > 0) {
        printf("[Payload (%d bytes)]\n  \"", payload_len);
        for(int i = 0; i < payload_len; i++) {
            u_char c = (u_char)read_hex(fp, 1);
            if(c >= 32 && c <= 126) printf("%c", c);
            else printf(".");
        }
        printf("\"\n");
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
