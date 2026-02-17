## [MAC Address Table](https://www.geeksforgeeks.org/computer-networks/what-is-mac-address-table/)
The MAC address table is a way to map each and every port to a MAC address. The MAC address table consists of two types of entries:
- **Static Entries:** 
    - have high priority,
    - manually changed or removed by the switch admin
- **Dynamic Entries:**
    - entries are added to the table automatically via MAC learning and deleted automatically as well
    - fetches the source MAC address of each Ethernet frame received on the port

## [MAC Learning](https://www.geeksforgeeks.org/computer-networks/mac-learning-in-ccna/)
MAC/CAM (Content Addressable Memory) Table Stores:
- MAC address
- The interface
- VLAN MAC address belongs to
- How the MAC address is learned is statically or dynamically.

Whenever a frame hits the interface of the switch it first checks the source MAC address and tries to find an entry for it in its CAM table if the entry doesn't exist an entry is created, if it already exists then the aging timer for that entry is refreshed.

The aging timer is used to identify how long a MAC address of a non-communicating device should be stored in the CAM table. *The default MAC address flushing time of all VLANs is 300s or 5 minutes.* [Age Timer Types](https://www.geeksforgeeks.org/computer-networks/mac-learning-and-aging/):
- Global aging timer: Amount after which learned MAC addresses are flushed for all VLANs.
- Per VLAN aging timer: Time after which MAC address is flushed belonging to a specific VLAN.

## TO Optimize:
- efficient search table --> fast loopup table needed.
- if it were only from software prespective, hash-table would suffice as answer to the problem, 
- but since practical cases in real life, switches are designed with a specialized memory hardware.
- thus for implemention of the similar working concept, we will be using cache like working data structure
- ref: https://stackoverflow.com/questions/39797288/how-would-you-design-a-mac-address-table-essentially-a-fast-look-up-table
- The capacity of a MAC address table is limited.
- then how to handle L2 MAC-Addr table collision?
- When the MAC address table is full, the device cannot learn source MAC addresses of valid packets.
- A device limits the number of learned MAC addresses in one of the following modes:
    - Disabling MAC address learning on an interface or a VLAN
    - Limiting the number of MAC addresses on an interface or a VLAN
- ref: https://support.huawei.com/enterprise/en/doc/EDOC1100064365/406107ae/understanding-the-mac-address-table
