# Task
To make a web ui interface to view & edit the context (that manages available switch accesses) 
from a database.csv with column field names:
- device_id (primary key)
- model_name
- hw_id (hardware id)
- mgmt_ip (ipv4)
- port_id (uint8)
- tag (enum resv/free/static, if empty, write as free)
- current_user (who reserved it)
- duration (uint in minutes)
- resv_end_time (time stamp)


## Global Config:
- pi_ip = "10.25.4.200"
- user = "host1"
- password = sheldon123
- console_ip = "192.168.1.102"
- port_offset = 10000
- av_ui.port = 4443, AV_UI.offset = 60000
- main_ui.port = 49152, main_ui.offset = 51000
- main_ui.old_port = 49151, main_ui.old_offset = 50000
- up_health_timer = 10s
- down_health_timer = 2s
- max_health_check_retries = 3


## Interface Representation:
| Id | Model | Hardware | UI | Mgmt-Ip | Console-Port | Health | Status | Reservation | Actions |
|---|---|---|---|---|---|---|---|---|---|
| device_id | model_name | hw_id | (AV, old_main, new_main) --> three hyper links. All three links should be like “http://localhost:calc_port/“ where <ul> <li> let last octect of mgmt_ip be `switch_id` </li> <li> for AV link: calc port = `AV_UI.offset + switch_id` </li> <li> for old_main link: calc port = `main_ui.old_offset + switch_id` </li> <li> for new_main link: calc port = `main_ui.offset + switch_id` </li> </ul>| mgmt_ip & copy_btn.value{`ssh admin@cell_value`} | port_id & copy_btn.value{`telnet console_ip ${port_id + port_offset}`} | up/down/unk & refresh_btn.onClick{resets health check retry counter} | tag.map(Free/Reserved/Static) | resv info refer below | Reserve/Release |
|ng-377 | M4250-10G2F-PoE+ | GSM4212P V1 | AV, Main(old), Main(new) --> placeholders for the hyper link to which each should be mapped| 192.168.1.125 | 7 | (need to ping at regular intervals)| free | input_box{user} input_box{duration}.format(box{hh}:box{mm}).default(1:00) | Reserve |


### Additional Requirements:
- copy.btn --> these values should be copied to clipboard when clicked

- Reservation Column:
    - `if tag == resv:` 
    need to main a status bar that should be live in below format:
    ```py
    f"User: {curent_user}, Duration: {duration}.format{(hh)hrs,(mm)mins}, Time Left: {resv_end_time - current_time}.format(hh:mm:ss} \n\
    Start: {resv_end_time - duration}.format(dd-mm-yy hh:mm) End: {end_time_stamp}.format(dd-mm-yy hh:mm)" 
    ```
    - `if tag == static:` 
    display current user name as owner, no need for any other info
    - `if tag == free:` allow users to enter new resv entry:
        - with user name (to update current owner), 
        - resv duration --> in two input boxes format, i.e. "[HH]hrs [MM]mins" with default values 1:00
        - when entered, it should trigger update into database and update resp entries and UI according.
- similarly Actions column should contain btns based on status tag, if free, display [RESERVE] btn else display [RELEASE] btn.

- keep the copy btns minimalistic, use this logo like `⧉` rather than texts in the btns, and relatively smaller than font_size used in that cell
- each resp btns should be within thier resp column fields perfectly aligned right wards
- handle missing entries:
    - if mgmt_ip is missing/not present/valid:
        - dont hyperlink any the UI placeholders
        - health = unk -> unknown only in this case, (any failure/exceptions should be marked down)
    - colorize and highlight different values of `Status`, `Health`, `Actions` btns


Checking & Updating Health status:
- if current health status is not known or down: 
    - recheck response every `down_health_timer` for `max_health_check_retries`
    - if refresh btn is clicked, the retry counter is resetted and app should start pinging device through below cmd till max retries again.
- else recheck response every `up_health_timer`
- use the below cmds to check ping response to the device
```
>>> Part-1:
    $ ssh {user}@{pi_ip}
>> continue connecting (yes/no) --> input yes if asked
>> password: "sheldon123"
>> if connection successfull, use this cmd to ping
>>> Part-2:
    $ ping {mgmt_ip}
>> exit all connections, any exceptions caught till now, means response check failed
```
- NOTE: Part-1 is common for health checkup for all the devices as they are accessable only via console, thus instead of repeating part-1 multiple times, u have to do part-1 once and sequentially do part-2 for all devices sequentially for all devices that are doing at regular intervals, like devices with health status `up` have to synchronously doing health checks at the given intervals.


## Manual Acesss Info:
- cmd: ssh host1@10.25.4.200
- password: sheldon123
- view devices: cat /var/lib/misc/dnsmasq.leases
- check connection: telnet 192.168.1.102 10001 
- (console ip - fixed = 192.168.1.102, usb no. = 1 + serve port offset = 10000)