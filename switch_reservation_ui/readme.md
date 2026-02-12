ToDo: to make a web ui interface to view this context (that manages available switch accesses) from a database csv like file

column field names:
- device_name (primary key)
- model_name
- remote_link (url)
- mgmt_ip (ipv4)
- console_ip (ipv4)
- server_port (uint)
- tag (enum resv/free)
- current_user (who reserved it)
- resv_end_time (time stamp)

resv --> need to main a status bar that should be live and have information of resv start time and resv end time along with duration

additional: allow users to enter new resv entry with user name (to update current owner), resv duration in the resp row entry and that will trigger update into database and update resp entries and UI according.

Interface Representation:

| Id | Model | UI | Mgmt-Ip | Console | Health | Status | Reservation | Actions |
|---|---|---|---|---|---|---|---|--|
| device_name | model_name | remote_link | mgmt_ip | `console_ip + ' ' + server_port` | up/down | Free / Reserved / Static | ` f"User: {curent_user}, Duration: {resv_end_time}.format(hh::mm} . {time_left}.format(hh::mm} left \nStart: {start_time} End: {end_time}" `| Reserve/Release |
| swi-chn-p8 | m4250-gtx50-av | https//localhost/51160 | 192.168.1.125 | 10.2.160.102 10025 | (need to ping at regular intervals)| free | input_box{user} input_box{duration}.format(hh:mm).default(1:00) | Reserve |


Requirements:
- copy icon btn at column field entrys of `Mgmt-Ip` & `Console`
- Mgmt-Ip: `ssh admin@cell_value`
- Console: `telnet cell_value`
- these values should be copied to clipboard when clicked