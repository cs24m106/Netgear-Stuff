q1. FLOW CONTROL: reciever processing rate is slow, how to handle
q2. break down bigger packets into small and assemble, what is seq no.?
q3. how packet actually looks when it leaves the laptop and whats the flow?

ref: tasks here:
https://docs.google.com/document/d/15CoWay6-M56UoN6DcWov0bCpfXzcsfJhm1g2H_nA2HE/edit?tab=t.0

ref: my docs here:
https://docs.google.com/document/d/11GMobK0O21rQDGCR71WXoVzYhX9lAgUnxje1UQ0AJm4/edit?tab=t.fj58x88oneeb#heading=h.ksfrdmxzd9iz

test switch:
- user: admin
- password: Netgear@@123

cmds:
- `ls /dev/tty` - list of available ttys, usb connections to switch connection will appear with USB..
- `screen /dev/ttyUSB0 115200` - access console using screen, baudrate:115200, need to be mention for serial conn
- `minicom -D /dev/ttyUSB0 -b 115200` -- access via mini com similarly
- `show serviceport` - inside console, to check OOB connection ip

