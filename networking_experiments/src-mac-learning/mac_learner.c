#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <arpa/inet.h>

#define MAX_TABLE_ENTRIES 100
#define MAX_PORTS 12
#define ETHERTYPE_VLAN 0x8100
#define ETHERTYPE_QINQ 0x88a8

typedef enum entry_type {stat, dynm} TYPE;

typedef struct mac_address_table_entry {
    uint vlan;
    uint8_t mac[6];
    TYPE type;
    uint port;
} MATE;

MATE mac_table[MAX_TABLE_ENTRIES];
int entry_count = 0;
int frame_count = 0;

// --- Helper Functions ---

// Consumes hex and returns value. Returns -1 on EOF/Error.
int32_t read_hex(FILE *fp, int bytes) {
    uint32_t val = 0;
    char hex[3];
    for (int i = 0; i < bytes; i++) {
        if (fscanf(fp, "%2s", hex) != 1) return -1;
        val = (val << 8) | (uint32_t)strtol(hex, NULL, 16);
    }
    return (int32_t)val;
}

// Skips non-hex junk between packets
void sync_to_next_packet(FILE *fp) {
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (isxdigit(c)) {
            ungetc(c, fp);
            break;
        }
    }
}

// --- MAC Table Management ---

void learn_mac(uint8_t *mac, uint vlan, TYPE type, uint port) {
    int found_index = -1;
    for (int i = 0; i < entry_count; i++) {
        if (memcmp(mac_table[i].mac, mac, 6) == 0 && mac_table[i].vlan == vlan) {
            found_index = i;
            break;
        }
    }

    if (found_index != -1) {
        // Handle MAC Move
        if (mac_table[found_index].port != port) {
            printf("\t[MOVE] MAC %02X:%02X:%02X:%02X:%02X:%02X shifted from Port 0x%X -> %u\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], 
                   mac_table[found_index].port, port);
            mac_table[found_index].port = port;
        } else {
            printf("\t[REFRESH] MAC already known on Port 0x%X\n", port);
        }
    } else {
        // Add new Dynamic entry
        if (entry_count < MAX_TABLE_ENTRIES) {
            mac_table[entry_count].vlan = vlan;
            memcpy(mac_table[entry_count].mac, mac, 6);
            mac_table[entry_count].type = type;
            mac_table[entry_count].port = port;
            printf("\t[NEW] Learned MAC %02X:%02X:%02X:%02X:%02X:%02X on Port 0x%X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], port);
            entry_count++;
        }
    }
}

// --- Main Processing Logic ---

int process_frame(FILE *fp, int port) {
    sync_to_next_packet(fp);
    if (feof(fp)) return 0;
    printf("\nFrame #%d arriving on Port 0x%X, ", ++frame_count, port);

    uint8_t d_mac[6], s_mac[6];
    
    // Read Destination (and check for EOF)
    for(int i=0; i<6; i++) {
        int val = read_hex(fp, 1);
        if (val == -1) return 0;
        d_mac[i] = (uint8_t)val;
    }
    // Read Source
    for(int i=0; i<6; i++) s_mac[i] = (uint8_t)read_hex(fp, 1);
    
    uint16_t eth_type = (uint16_t)read_hex(fp, 2);
    uint vlan_id = 1; // Default
    uint bytes_read = 14;

    // VLAN check (innermost tag)
    while (eth_type == ETHERTYPE_VLAN || eth_type == ETHERTYPE_QINQ) {
        uint16_t tci = (uint16_t)read_hex(fp, 2); // // Tag Control Information
        vlan_id = tci & 0x0FFF;
        eth_type = (uint16_t)read_hex(fp, 2);
        bytes_read += 4;
    }
    // randomizer for vlan
    if (port > MAX_PORTS/2 && vlan_id == 1){

        vlan_id = (rand() % 10) + 1;
        printf("vlan (randomized) %d\n", vlan_id);
    }
    else{
        if (vlan_id == 1){
            printf("vlan (default) %d\n", vlan_id);
        }
        else{
            printf("vlan (actual) %d\n", vlan_id);
        }
    }


    // LEARN
    learn_mac(s_mac, vlan_id, dynm, port);

    // Skip remainder of packet to maintain sync
    // If IPv4, use Total Length field
    if (eth_type == 0x0800) {
        read_hex(fp, 1); // Version/IHL
        read_hex(fp, 1); // TOS
        uint16_t ip_len = (uint16_t)read_hex(fp, 2);        
        // Ensure we consume up the full IP length minus 4 as we have already read those fields
        for (int i = 0; i < ip_len - 4; i++) read_hex(fp, 1);
    } else {
        // For non-IP, just skip to 60-byte minimum (min eth-frame = 64 Bytes = 60 + 4B CRC that is not inclusive)
        if (bytes_read < 60) {
            for (int i = 0; i < (60 - bytes_read); i++) read_hex(fp, 1);
        }
    }

    return 1;
}

void disconnect_efp(uint down_port) {
    int deleted_count = 0;

    printf("\n[PORT EVENT] Interface Port 0x%X Disconnected. Flushing MAC Table...\n", down_port);

    for (int i = 0; i < entry_count; ) {
        // Only flush DYNAMIC entries; STATIC entries usually persist 
        // unless the configuration is explicitly removed.
        if (mac_table[i].port == down_port && mac_table[i].type == dynm) {
            
            printf("\t|- Removing MAC: %02X:%02X:%02X:%02X:%02X:%02X (VLAN %u)\n",
                   mac_table[i].mac[0], mac_table[i].mac[1], mac_table[i].mac[2],
                   mac_table[i].mac[3], mac_table[i].mac[4], mac_table[i].mac[5],
                   mac_table[i].vlan);

            // Shift subsequent entries left to fill the gap
            for (int j = i; j < entry_count - 1; j++) {
                mac_table[j] = mac_table[j + 1];
            }

            entry_count--;
            deleted_count++;
            // Do NOT increment 'i' here, because the next element 
            // has shifted into the current index 'i'.
        } else {
            i++;
        }
    }
    printf("[FLUSH COMPLETE] Removed %d entries for Port 0x%X.\n", deleted_count, down_port);
}

void display_table() {
    printf("\n|               MAC ADDRESS TABLE              |\n");
    printf("------------------------------------------------\n");
    printf("%-8s | %-17s | %-8s | %-5s\n", "VLAN", "MAC ADDRESS", "TYPE", "PORT");
    printf("------------------------------------------------\n");
    for (int i = 0; i < entry_count; i++) {
        printf("%-8u | %02X:%02X:%02X:%02X:%02X:%02X | %-8s | 0x%-5X\n",
               mac_table[i].vlan,
               mac_table[i].mac[0], mac_table[i].mac[1], mac_table[i].mac[2],
               mac_table[i].mac[3], mac_table[i].mac[4], mac_table[i].mac[5],
               (mac_table[i].type == stat) ? "STATIC" : "DYNAMIC",
               mac_table[i].port);
    }
    printf("------------------------------------------------\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) return printf("Usage: %s <hex_text_file>\n", argv[0]), 1;
    FILE *fp = fopen(argv[1], "r");
    if (!fp) return perror("File error"), 1;
    
    srand(time(NULL));
    uint test_port = 10;
    
    // 1. simulate static mac addresses
    uint8_t mac[6];
    for (int i=0; i<6; i++) mac[i] = (uint8_t)0;
    printf("\nAssigning Static MAC (frame count:#%d) on Port 0x%X\n", ++frame_count, test_port);
    learn_mac(mac, 200, stat, test_port);
    for (int i=0; i<6; i++) mac[i] = (uint8_t)255;
    printf("\nAssigning Static MAC (frame count:#%d) on Port 0x%X\n", ++frame_count, test_port);
    learn_mac(mac, 200, stat, test_port);

    // 2. simulate dynamic mac addresses of all the packets from a file
    process_frame(fp, test_port); process_frame(fp, test_port);
    while(process_frame(fp, (rand() % MAX_PORTS) + 1)); // Random Port 1-12
    display_table();
    
    // 3. simulate port disconnection 
    disconnect_efp(test_port);
    display_table();

    fclose(fp);
    return 0;
}