## Setup Router Simulation Environment
```sh
# Create network namespaces to simulate multi-hop routing
sudo ip netns add host1
sudo ip netns add router
sudo ip netns add host2

# Create veth pairs
sudo ip link add veth1 type veth peer name veth1r
sudo ip link add veth2 type veth peer name veth2r

# Assign interfaces to namespaces
sudo ip link set veth1 netns host1
sudo ip link set veth1r netns router
sudo ip link set veth2 netns host2
sudo ip link set veth2r netns router

# Configure host1
sudo ip netns exec host1 ip addr add 10.0.1.2/24 dev veth1
sudo ip netns exec host1 ip link set veth1 up
sudo ip netns exec host1 ip route add default via 10.0.1.1

# Configure router interfaces
sudo ip netns exec router ip addr add 10.0.1.1/24 dev veth1r
sudo ip netns exec router ip addr add 10.0.2.1/24 dev veth2r
sudo ip netns exec router ip link set veth1r up
sudo ip netns exec router ip link set veth2r up
sudo ip netns exec router sysctl -w net.ipv4.ip_forward=1

# Configure host2
sudo ip netns exec host2 ip addr add 10.0.2.2/24 dev veth2
sudo ip netns exec host2 ip link set veth2 up
sudo ip netns exec host2 ip route add default via 10.0.2.1

# Load TTL module INSIDE router namespace
sudo ip netns exec router insmod ttl_router.ko
```

## Test TTL Behavior
```sh
# In NEW terminal, monitor router logs
sudo ip netns exec router dmesg -w

# From host1, send ping with TTL=2 to host2
sudo ip netns exec host1 ping -t 2 -c 3 10.0.2.2

# Expected output in router dmesg:
[ 1234.567890] ROUTER: Forwarded packet from 10.0.1.2 TTL=1
[ 1234.567891] ROUTER: TTL EXPIRED (was 1) from 10.0.1.2 - Dropping packet

# Test with TTL=3 (should succeed)
sudo ip netns exec host1 ping -t 3 -c 3 10.0.2.2
# Expected: "ROUTER: Forwarded packet from 10.0.1.2 TTL=2" (twice - one hop each direction)
```

## Cleanup
```sh
sudo ip netns exec router rmmod ttl_router
sudo ip -all netns delete
```