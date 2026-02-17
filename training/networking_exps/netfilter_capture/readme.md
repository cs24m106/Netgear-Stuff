## Compile & Load Module
```sh
# Compile module
make

# Load module (requires root)
sudo insmod eth_capture.ko

# Verify loaded
lsmod | grep eth_capture

# Monitor kernel logs in real-time (in separate terminal)
sudo dmesg -w
```

## Generate Traffic & Verify Output
```sh
# In NEW terminal, generate traffic
ping -c 3 8.8.8.8

# Expected dmesg output:
[ 1234.567890] ETHER CAPTURE: SRC MAC: aa:bb:cc:dd:ee:ff DST MAC: 11:22:33:44:55:66 EtherType: 0x0800
[ 1234.567891] ETHER CAPTURE: SRC MAC: 11:22:33:44:55:66 DST MAC: aa:bb:cc:dd:ee:ff EtherType: 0x0800
```

## Unload Module
```sh
sudo rmmod eth_capture
dmesg | tail -5  # Verify unload message
```