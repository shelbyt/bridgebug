## The use case:

An all-to-all udp ping between docker containers on the same docker bridge and  same host machine.

## What the program does:
The scipts will create  two debian docker images bbdemo0 and bbdemo1 and connects them
to a docker bridge. The all-to-all ping binary (allping) takes in an iplist of ips to ping, start time,
and number of containers to be pinged with a small udp ping/pong message in the payload.

The iplist is generated dynamically and then copied into the container. Both the iplist and the 
allping binary are copied into the bbdemo containers and the binary is launched at the specified time.
Each container logs the rtt (in seconds) of its own message  and of other containers. i.e. 2 containers would
produce 2 timestamps each resulting in 4 time stamps across two log files.  This is printed out at the end of the script. 

If a packet drop happens it will show up as negative when printed. It may take up to 4 runs before the drops start
to appear. See picture below for an example output run with missing packets:

![alt text](https://raw.githubusercontent.com/shelbyt/bridgebug/master/missing.png)

### Reproducing problem with the scripts:
Most of the time packets will be lost **(exp_1.sh)**. However, if we re-run the experiment in quick sucession the problem
usually does not appear again **(exp_2.sh)** and the packet is not dropped. 

If we inject a small amount of delay between pings i.e. add 100ms delay between pings then 
there are no drops 100% of the time **(exp_3.sh)**



## Observations
- If we replace our code with the PING command i.e. replace UDP with ICMP and do an all-to-all then
drops do not occur.
- This problem does NOT manifest itself if we start
several processes (instead of containers) and have them message each other through different ports. i.e. avoiding the use of docker.
This leads us to believe it is some issue with the bridge.
- This problem also manifests itself if we create the bridge manually i.e. use ip commands and do NOT use the docker bridge to
create the network. This leads us to beleive it may be some issue with the kernel bridge and its interaction with docker.

Measurements:
Looking at the pcap, the udp packet is sent out of container0 but never arrives in container1.

Findings:
We monitored kernel drops by enabling the **drop_monitor** module. We found that the drop occurs in the "nf_hook_slow"
function. This is the only clue that the packet was dropped somewhere. Monitoring from the host, IPTables, ethtool, and arp do not report any drops.

Unlikly Explanations:
IPTables - We ran an experiment where we made the firewall on the host accept all udp connections.
If we were getting firewalled, the bug would not dissappear on subsequent runs

ARP - We hardcoded arp entries with arp -S into the containers and the host machine
If we were getting some arp misconfiguration, the bug would not dissappear on subsequent runs

Tuning - Followed most best-practice kernel configurations including increasing buffers such as backlog.
	But this problem happens with only 2 connections. Unlikey a buffer is overflowing.


## Reproducibility Information
Reproduced on (Ubuntu):

Docker version 17.05.0-ce, build 89658be
kernel: 4.4.0-142-generic

Docker version 18.06.3-ce, build d7080c1
kernel: 4.4.0-142-generic

Docker version 18.09.4, build d14af54
kernel:4.19.5-041905-generic

Docker version 18.06.3-ce, build d7080c1
kernel:5.0.9-050009-generic


Testbeds Reproduced on:
Local machine
Local machine + vms
EC2

