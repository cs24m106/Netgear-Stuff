## **IEEE 802.1Q VLAN tagging**

- [802.1Q](https://en.wikipedia.org/wiki/IEEE_802.1Q#:~:text=IEEE%20802.1Q%2C%20often%20referred,IEEE%20802.1D%2D2004%20standard.) [VLAN tagging](https://www.cbtnuggets.com/blog/technology/networking/what-is-802-1q-port-tagging) is the IEEE standard for identifying Ethernet frames belonging to different Virtual LANs (VLANs) by inserting a special 4-byte tag into the Ethernet header, allowing switches to forward traffic across trunk links while keeping VLANs logically separate and managing Quality of Service (QoS) with priority bits. This tagging process adds a Tag Protocol Identifier (TPID) and Tag Control Information (TCI) containing the VLAN ID (VID) and priority, enabling a single physical link to carry traffic for multiple VLANs.  

![Frame Format](https://upload.wikimedia.org/wikipedia/commons/thumb/5/52/TCPIP_802.1ad_DoubleTag.svg/1280px-TCPIP_802.1ad_DoubleTag.svg.png)

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

## Double Tagging
Double tagging, also known as Q-in-Q, is a networking technique that uses two VLAN (Virtual Local Area Network) tags on a single Ethernet frame.

Double tagging involves embedding one VLAN tag (Inner Tag) within another (Outer Tag). The Inner Tag is used to manage traffic within a customer’s network, while the Outer Tag is added by the service provider to route the customer’s data through their infrastructure. This method helps keep traffic from different customers or departments separate while preserving the original VLAN structure.

- **How it works:** A service provider adds an outer tag to customer traffic that already has an inner tag (the customer's VLAN).
- **Purpose:** Allows a single trunk link to carry traffic for multiple customers, each with their own VLANs, without conflicts, extending VLAN scalability.
- **Analogy:** Like putting a customer's letter (inner tag) inside a service provider's envelope (outer tag). 

## ARP Format:
ARP's placement within the Internet protocol suite and the OSI model. [Example](https://en.wikipedia.org/wiki/Address_Resolution_Protocol#Example):<br>

>Lookup Cache (refer aboce explample): *"Typically, a network node maintains a lookup cache that associates IP and MAC addressees. In this example, if A had the lookup cached, then it would not need to broadcast the ARP request. Also, when B received the request, it could cache the lookup to A so that if B needs to send a packet to A later, it does not need to use ARP to lookup its MAC address. Finally, when A receives the ARP response, it can cache the lookup for future messages addressed to the same IP address."*

![Frame Format](https://media.geeksforgeeks.org/wp-content/uploads/20230210180525/ARP-Green.png)

[IPv4 Packet Structure:](https://en.wikipedia.org/wiki/Address_Resolution_Protocol#Packet_structure)
- **Hardware Type (HTYPE): 16 bits**
    >This field specifies the network link protocol type. Value of 1 indicates Ethernet.
- **Protocol Type (PTYPE): 16 bits**
    >This field specifies the internetwork protocol for which the ARP request is intended. For IPv4, this has the value 0x0800. The permitted PTYPE values share a numbering space with those for EtherType.
- **Hardware Length (HLEN): 8 bits**
    >Length (in octets) of a hardware address. For Ethernet, the address length is 6.
- **Protocol Length (PLEN): 8 bits**
    >Length (in octets) of internetwork addresses. The internetwork protocol is specified in PTYPE. In this example: IPv4 address length is 4.
- **Operation (OPER): 16 bits**
    > Specifies the operation that the sender is performing: 1 for request, 2 for reply.



    >Media address of the sender. In an ARP request this field is used to indicate the address of the host sending the request. In an ARP reply this field is used to indicate the address of the host that the request was looking for.
- **Sender protocol address (SPA): 32 bits**
    >Internetwork address of the sender.
- **Target hardware address (THA): 48 bits**
    >Media address of the intended receiver. In an ARP request this field is ignored. In an ARP reply this field is used to indicate the address of the host that originated the ARP request.
- **Target protocol address (TPA): 32 bits**
    >Internetwork address of the intended receiver.

|Field|Request (Who has this IP?)|Reply (I am that IP!)|
|---|---|---|
|Sender Hardware Address|MAC address of the source host|MAC address of the responding host|
|Sender Protocol Address|IP address of the source host|IP address of the responding host|
|Target Hardware Address|00:00:00:00:00:00 (Unknown/Ignored)|MAC address of the original requester|
|Target Protocol Address|IP address of the destination you are looking for|IP address of the original requester|

The EtherType for ARP is 0x0806. This appears in the Ethernet frame header when the payload is an ARP packet and is not to be confused with PTYPE, which appears within this encapsulated ARP packet.

## RARP Format:
![Frame Format](https://media.geeksforgeeks.org/wp-content/uploads/20231016193121/RARP-address-protocol-format-660.jpg)

|Field|Request|Reply|
|---|---|---|
|Sender Hardware Address|MAC address of the **source host**|MAC address of the **RARP server**|
|Sender Protocol Address|*undefined* (who am i?)|IP address of the **RARP server**|
|Target Hardware Address|MAC address of the **source host**|MAC address of the **host** that sent the RARP request|
|Target Protocol Address|*undefined* (i dont know rarp server--> typically broadcasted)|IP address of the **host** that sent the RARP request|

Points to Note:
- **Protocol Unification:** 
    >Both ARP and RARP use the exact same packet format, only differing in the operation code (and the use of the address fields). RARP requests use 3 and RARP replies use 4, naturally extending from ARP's 1 and 2. This unified design, made possible by having a sufficiently large operation field, promotes interoperability and simpler implementation across different network layers and hardware types (like Ethernet, Token Ring, FDDI, etc.).
- **Standardization by IANA:**
    > The Internet Assigned Numbers Authority (IANA) is responsible for assigning and maintaining a registry of these operation codes. The 16-bit field provides a large numbering space for various experimental and non-standard functions, even if they aren't widely known or implemented in typical consumer networks. 


In a standard RARP (Reverse Address Resolution Protocol) request, the Sender Hardware Address (SHA) and Target Hardware Address (THA) are usually identical because a diskless workstation is typically asking for its own IP address.

If they are not the same, the packet is technically valid according to the protocol structure, but it changes the logic of the operation. Here is exactly what happens:

1. The RARP Server's Perspective
    >The RARP server (the machine with the database of MAC-to-IP mappings) ignores the Sender MAC field when performing its lookup. It specifically looks at the Target Hardware Address (THA) field to find a matching IP address in its table.
    >
    >Lookup: The server searches its database for the Target MAC, not the Sender MAC.
    >
    >Result: If a mapping for the Target MAC exists, the server prepares a RARP Reply.

2. The Destination of the Reply
    >Even though the server looked up the Target MAC, the RARP Reply (Opcode 4) is a unicast frame. The server will send this reply to the Sender Hardware Address (SHA)—the MAC address of the machine that actually physically sent the request.
    >
    >The Result: The machine that sent the request (Sender) will receive the IP address belonging to the Target machine.

|Feature|Standard Request|"Mismatched" Request|
|---|---|---|
|Who is asking?|The host that needs an IP.|A host asking on behalf of another.|
|Server Lookup Key|Target Hardware Address|Target Hardware Address|
|Where is Reply sent?|To the Sender (the host itself).|To the Sender (the proxy/requesting host).|
|Primary Goal|Self-Configuration.|Proxying or Information Gathering.|