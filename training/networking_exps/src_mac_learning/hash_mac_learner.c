#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h> // randomize seed
#include <unistd.h> // For sleep()

#define MAX_PORTS 12
#define ETHERTYPE_VLAN 0x8100
#define ETHERTYPE_QINQ 0x88a8
#define ETHERTYPE_IPV4 0x0800

#define HASH_TABLE_SIZE 16  // Small for demonstration
#define BUCKET_SIZE 4       // 4-way set associative
#define MAX_TABLE_ENTRIES (HASH_TABLE_SIZE*BUCKET_SIZE) // cache size
#define MAC_AGE_OUT_TIME 5 // Timeout timer

typedef enum entry_type {EMPTY, STATIC, DYNAMIC} TYPE;

typedef struct mac_address_table_entry {
    uint vlan;
    uint8_t mac[6];
    TYPE type;
    uint port;
    time_t last_seen; // Timestamp (private)
} MATE;

// The Table: A 2D array (Buckets x Entries)
MATE l2_table[HASH_TABLE_SIZE][BUCKET_SIZE];
int entry_count = 0;
int frame_count = 0;

// Simple Hash Function: XOR MAC bytes and VLAN
uint32_t calculate_hash(uint8_t *mac, uint16_t vlan) {
    uint32_t h = vlan;
    for (int i = 0; i < 6; i++) h ^= mac[i];
    return h % HASH_TABLE_SIZE;
}
void init_table() {
    for (int i = 0; i < HASH_TABLE_SIZE; i++)
        for (int j = 0; j < BUCKET_SIZE; j++)
            l2_table[i][j].type = EMPTY;
}

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
    uint32_t index = calculate_hash(mac, vlan);
    printf("\tHash-Index: %u, ", index);
    int empty_slot = -1;
    time_t now = time(NULL);

    // 1. Search if MAC already exists in the bucket (O(1) search)
    for (int i = 0; i < BUCKET_SIZE; i++) {
        // --- AGING LOGIC ---
        // If slot is not empty, check if it has timed out
        if (l2_table[index][i].type == DYNAMIC) {
            if (difftime(now, l2_table[index][i].last_seen) > MAC_AGE_OUT_TIME) {
                printf("Bucket-Index: %u\t[AGE-OUT] (MAC: %02X:%02X:%02X:%02X:%02X:%02X, vlan: %d) timed out after %ds\n", i,
                       l2_table[index][i].mac[0], l2_table[index][i].mac[1], l2_table[index][i].mac[2],
                       l2_table[index][i].mac[3], l2_table[index][i].mac[4], l2_table[index][i].mac[5],
                       l2_table[index][i].vlan, MAC_AGE_OUT_TIME);
                l2_table[index][i].type = EMPTY;
            }
        }

        // --- LOOKUP & REFRESH ---        
        if (l2_table[index][i].type != EMPTY &&
            memcmp(l2_table[index][i].mac, mac, 6) == 0 &&
            l2_table[index][i].vlan == vlan) {
            
            l2_table[index][i].last_seen = now; // Reset timestamp on lookup/refresh

            // Handle MAC Move
            if (l2_table[index][i].port != port) {
                printf("Bucket-Index: %u\t[MOVE] (MAC: %02X:%02X:%02X:%02X:%02X:%02X, vlan: %d) shifted from Port 0x%X to 0x%X\n", i,
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vlan,
                    l2_table[index][i].port, port);
                l2_table[index][i].port = port; // Update/Refresh
            }
            // Handle MAC Refresh
            else{
                printf("Bucket-Index: %u\t[REFRESH] Timestamp updated for (MAC: %02X:%02X:%02X:%02X:%02X:%02X, vlan: %d)\n", i,
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vlan);
            }
            return;
        }
        if (l2_table[index][i].type == EMPTY && empty_slot == -1) {
            empty_slot = i;
        } 
    }
    // 2. If not found, insert into first empty slot in the bucket
    if (empty_slot != -1) {
        l2_table[index][empty_slot].vlan = vlan;
        l2_table[index][empty_slot].port = port;
        l2_table[index][empty_slot].type = type;
        l2_table[index][empty_slot].last_seen = now; // Set initial timestamp
        memcpy(l2_table[index][empty_slot].mac, mac, 6);
        printf("Bucket-Index: %u\t[NEW] Learned (MAC: %02X:%02X:%02X:%02X:%02X:%02X, vlan: %d) via Port 0x%X\n", empty_slot,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vlan, port);
    } else {
        // 3. COLLISION: Bucket is full!
        printf("!!! TABLE COLLISION !!! Bucket is full. Cannot learn given (MAC: %02X:%02X:%02X:%02X:%02X:%02X, vlan: %d) via Port 0x%X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], vlan, port);
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
    learn_mac(s_mac, vlan_id, DYNAMIC, port);

    // Skip remainder of packet to maintain sync
    // If IPv4, use Total Length field
    if (eth_type == ETHERTYPE_IPV4) {
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

// port flush is inherently O(N) because a port's MAC addresses could be scattered across any bucket in the table.
void disconnect_efp(uint32_t down_port) {
    int deleted_count = 0;

    printf("[PORT EVENT] Interface Port 0x%X Disconnected. Flushing Hash Table...\n", down_port);

    // We must scan the entire 2D structure
    for (int b = 0; b < HASH_TABLE_SIZE; b++) {
        for (int s = 0; s < BUCKET_SIZE; s++) {
            
            // Only remove entries that match the port AND are DYNAMIC
            if (l2_table[b][s].type == DYNAMIC && l2_table[b][s].port == down_port) {
                
                printf("\t|- Removing MAC: %02X:%02X:%02X:%02X:%02X:%02X (VLAN %u) from Bucket [%d] Slot [%d]\n",
                       l2_table[b][s].mac[0], l2_table[b][s].mac[1], l2_table[b][s].mac[2],
                       l2_table[b][s].mac[3], l2_table[b][s].mac[4], l2_table[b][s].mac[5],
                       l2_table[b][s].vlan, b, s);

                // In a hash table, we don't "shift" entries like a linear array.
                // We simply mark the slot as EMPTY so the hash search ignores it
                // and new entries can overwrite it later.
                l2_table[b][s].type = EMPTY;
                memset(l2_table[b][s].mac, 0, 6); // Optional: clear for security/debugging
                
                deleted_count++;
            }
        }
    }
    printf("[FLUSH COMPLETE] Removed %d entries for Port 0x%X from Hash Table.\n", deleted_count, down_port);
}

// reset all entries since we are reading from a packet file initialize MAT, it will expire before all are set
void display_hash_table() {
    time_t now = time(NULL);
    printf("\n---------- MAC TABLE (Size: %4dB, Timeout:%2ds) ----------\n", MAX_TABLE_ENTRIES, MAC_AGE_OUT_TIME);
    printf("%-6s | %-17s | %-8s | %-7s | %-5s\n", "VLAN", "MAC ADDRESS", "TYPE", "PORT", "AGE");
    printf("----------------------------------------------------------\n");
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        for (int j = 0; j < BUCKET_SIZE; j++) {
            if (l2_table[i][j].type != EMPTY) {
                MATE *e = &l2_table[i][j];
                double age = difftime(now, e->last_seen);
                if (e->type == DYNAMIC && age > MAC_AGE_OUT_TIME){
                    e->type = EMPTY;
                }
                printf(" %-5u | %02X:%02X:%02X:%02X:%02X:%02X | %-8s | 0x%-5X | %3.2fs\n",
                       e->vlan, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                       (e->type == EMPTY) ? "EXPIRED" : ((e->type == STATIC) ? "STATIC" : "DYNAMIC"), e->port, age);
            }
        }
    }
    printf("----------------------------------------------------------\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) return printf("Usage: %s <hex_text_file>\n", argv[0]), 1;
    FILE *fp = fopen(argv[1], "r");
    if (!fp) return perror("File error"), 1;
    
    srand(time(NULL));
    uint test_port = 10;
    printf(">>> Simulating MAC TABLE Learner using hash-table of size: %d Bytes...\n", MAX_TABLE_ENTRIES);

    // 1. simulate static mac addresses
    uint8_t mac[6];
    for (int i=0; i<6; i++) mac[i] = (uint8_t)0;
    printf("\nAssigning Static MAC (frame count:#%d) on Port 0x%X\n", ++frame_count, test_port);
    learn_mac(mac, 200, STATIC, test_port);
    for (int i=0; i<6; i++) mac[i] = (uint8_t)255;
    printf("\nAssigning Static MAC (frame count:#%d) on Port 0x%X\n", ++frame_count, test_port);
    learn_mac(mac, 200, STATIC, test_port);

    // 2. simulate dynamic mac addresses of all the packets from a file
    process_frame(fp, test_port); process_frame(fp, test_port);
    while(process_frame(fp, (rand() % MAX_PORTS) + 1)); // Random Port 1-12
    display_hash_table();

    sleep(1); // delay 1 sec

    // 3. simulate L2 collision handling
    uint8_t mac_a[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01}; // Hashes to 1 (if vlan 0)
    uint8_t mac_b[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x11}; // Also hashes to 1 (0x11 ^ 0x10 = 0x01)
    
    for(int i=0; i < 5; i++) { // // Fill the bucket
        mac_a[0] = mac_b[0] = i; // Change first byte to create "different" MACs
        test_port = 0x10 + i;
        printf("Collision Check: Adding MAC-entry (frame count:#%d) on Port 0x%X\n", ++frame_count, test_port);
        learn_mac(mac_a, 10, DYNAMIC, test_port);
        
        printf("Collision Check: Adding MAC-entry (frame count:#%d) on Port 0x%X\n", ++frame_count, test_port);
        learn_mac(mac_b, 10, DYNAMIC, test_port);    
        printf("\n");
        sleep(1); // Simulate real-time delay 1s
    }
    display_hash_table();
    
    // 4. simulate port disconnection 
    disconnect_efp(test_port);
    display_hash_table();

    fclose(fp);
    return 0;
}