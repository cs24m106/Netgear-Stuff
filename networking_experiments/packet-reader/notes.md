## **IEEE 802.1Q VLAN tagging**

- [802.1Q](https://en.wikipedia.org/wiki/IEEE_802.1Q#:~:text=IEEE%20802.1Q%2C%20often%20referred,IEEE%20802.1D%2D2004%20standard.) [VLAN tagging](https://www.cbtnuggets.com/blog/technology/networking/what-is-802-1q-port-tagging) is the IEEE standard for identifying Ethernet frames belonging to different Virtual LANs (VLANs) by inserting a special 4-byte tag into the Ethernet header, allowing switches to forward traffic across trunk links while keeping VLANs logically separate and managing Quality of Service (QoS) with priority bits. This tagging process adds a Tag Protocol Identifier (TPID) and Tag Control Information (TCI) containing the VLAN ID (VID) and priority, enabling a single physical link to carry traffic for multiple VLANs.  

![Frame Format](https://upload.wikimedia.org/wikipedia/commons/thumb/0/0e/Ethernet_802.1Q_Insert.svg/1992px-Ethernet_802.1Q_Insert.svg.png)

### How it Works
- **Tag Insertion:** When a frame from an access port (connected to an end device) needs to travel across a trunk link (between switches), the switch inserts the 802.1Q tag. 
- **Tag Structure:** The 4-byte tag contains:
    - **TPID (2 bytes):** Identifies the frame as an 802.1Q tagged frame (e.g., 0x8100). 
    - **TCI (2 bytes):** Contains:
        - Priority Code Point (PCP): 3 bits for QoS (0-7). 
        - Drop Eligible Indicator (DEI): A flag. 
        - VLAN Identifier (VID): A 12-bit field (0-4094) identifying the VLAN. 
- **Trunking:** On trunk ports, frames carry these tags to maintain VLAN context. 
- **Native VLAN:** Frames on the native VLAN (typically VLAN 1) are sent untagged. 
- **Tag Removal:** When a tagged frame leaves the trunk and enters an access port for a specific device, the switch removes the tag before forwarding. 

![What is 802.1Q Port Tagging?](https://images.ctfassets.net/aoyx73g9h2pg/3Bv0UJzi0ZOIpeIDn4SvEM/7f63324da001b246a7e263860cb9d89a/What-is-802-1Q-Port-Tagging-Diagram.jpg)

### Key Benefits
- Logical Separation: Creates multiple broadcast domains on shared physical hardware.
- Scalability: Efficiently manages traffic for many VLANs over fewer cables.
- QoS: Allows prioritization of different traffic types (voice, video). 

*Use Case Example*<br>
When connecting two switches, the link between them is usually a trunk port configured for 802.1Q. It carries traffic for multiple VLANs (e.g., VLAN 10, VLAN 20). When a frame from a device on VLAN 10 hits the switch, the switch adds the VLAN 10 tag. The receiving switch reads the tag, knows it's for VLAN 10, and forwards it appropriately, keeping it separate from VLAN 20 traffic.

## TCP Header Theory vs Implementation Convienience (Only for Storing field-wise):
`struct tcphdr`: ? as 1B is least allocatable address (Byte addressable machines)
- header length (4bits) --> data_offset (8bits) 
- reserved field (6bits) --> unused (8bits)
- tcp flags (6bits) --> flags (8bits)