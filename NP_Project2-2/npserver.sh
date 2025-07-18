#!/bin/bash

# 密碼
PASSWORD="wilsonliao1226"

work=$1

if [ -z "$work" ]; then
    echo "Warning: work is missing. Setting default value."
    work = ""
fi

echo "Work: $work"

if [ "$work" == "kill" ]; then
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "c110705063@nplinux2.cs.nycu.edu.tw" bash -c "
        pkill -u c110705063 -f np_multi_proc
        pkill -u c110705063 -f \"tmux: server\"
    "
elif [ "$work" == "upload" ]; then
    sshpass -p "$PASSWORD" scp np_multi_proc.cpp c110705063@nplinux2.cs.nycu.edu.tw:
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "c110705063@nplinux2.cs.nycu.edu.tw" bash -c "make"
elif [ "$work" == "compare" ]; then
    cmp=$2
    sshpass -p "$PASSWORD" scp -r -o StrictHostKeyChecking=no "c110705063@nplinux3.cs.nycu.edu.tw:project-3-demo-sample-ezrealux-main/client/multi_client_2/answer" .
    sshpass -p "$PASSWORD" scp -r -o StrictHostKeyChecking=no "c110705063@nplinux3.cs.nycu.edu.tw:project-3-demo-sample-ezrealux-main/client/multi_client_2/output" .
    if [ ! -f . ]; then
        sshpass -p "$PASSWORD" scp -r -o StrictHostKeyChecking=no "c110705063@nplinux3.cs.nycu.edu.tw:project-3-demo-sample-ezrealux-main/client/multi_client_2/compare.sh" .
    fi
    ./compare.sh $cmp
elif [ "$work" == "server" ]; then
    port=$2
    if [ -z "$port" ]; then
        echo "Warning: Port is missing."
        exit 1
    fi
    
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "c110705063@nplinux4.cs.nycu.edu.tw" bash -c "
        ./np_multi_proc $port || exit 1
    "
elif [ "$work" == "client" ]; then
    port=$2
    clientnum=$3
    if [ -z "$port" ]; then
        echo "Warning: Port is missing."
        exit 1
    fi
    if [ -z "$clientnum" ] || [ "$clientnum" -lt 5 ] || [ "$clientnum" -gt 11 ]; then
        echo "Error: Client number must be between 5 and 11."
        exit 1
    fi

    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "c110705063@nplinux$clientnum.cs.nycu.edu.tw" bash -c "
        telnet nplinux4.cs.nycu.edu.tw $port || exit 1
    "
else
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "c110705063@nplinux12.cs.nycu.edu.tw"
fi