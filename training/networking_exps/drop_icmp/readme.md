## Compile & Load
```sh
# Compile
make

# Load module
sudo insmod icmp_block.ko

# Verify loaded
lsmod | grep icmp_block
```

## Check ICMP via Ping
```sh

# Test ICMP blocking (should FAIL)
ping -c 2 8.8.8.8
# Output: "Request timeout" or 100% packet loss

# Test TCP allowed (should SUCCEED)
curl -I http://example.com

# Monitor blocked packets
sudo dmesg -w | grep FIREWALL
# Expected: "FIREWALL: BLOCKED ICMP type=8 code=0 from 192.168.x.x"
```

## Unload Module
```sh
# Unload when done
sudo rmmod icmp_block
dmesg | tail -5  # Verify unload message
```