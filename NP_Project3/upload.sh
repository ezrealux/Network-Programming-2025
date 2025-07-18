#!/bin/bash

# 密碼
PASSWORD="wilsonliao1226"
npservers=(
    "nplinux1.cs.nycu.edu.tw",
    "nplinux2.cs.nycu.edu.tw",
    "nplinux3.cs.nycu.edu.tw",
    "nplinux4.cs.nycu.edu.tw",
    "nplinux5.cs.nycu.edu.tw",
)
ports=(
    "1501"
    "1502"
    "1503"
    "1504"
    "1505"
)
work=$1

if [ -z "$work" ]; then
    echo "Warning: work is missing. Setting default value."
    work = ""
fi

if [ "$work" == "server" ]; then
    sshpass -p "$password" ssh -o StrictHostKeyChecking=no "c110705063@nplinux6.cs.nycu.edu.tw" "./np_single_golden 1453" &

    for i in ${!servers[@]}; do
        server=${servers[$i]}
        port=${ports[$i]}
        sshpass -p "$password" ssh -o StrictHostKeyChecking=no "c110705063@$server" "./np_single_golden $port" &
    done
elif [ "$work" == "upload" ]; then
    sshpass -p "$PASSWORD" scp makefile c110705063@nplinux10.cs.nycu.edu.tw:
    sshpass -p "$PASSWORD" scp http_server.cpp c110705063@nplinux10.cs.nycu.edu.tw:
    sshpass -p "$PASSWORD" scp console.cpp c110705063@nplinux10.cs.nycu.edu.tw:
elif [ "$work" == "compare" ]; then
    cmp=$2
    sshpass -p "$PASSWORD" scp -r -o StrictHostKeyChecking=no "c110705063@nplinux11.cs.nycu.edu.tw:project-3-demo-sample-ezrealux-main/client/multi_client_2/answer" .
    sshpass -p "$PASSWORD" scp -r -o StrictHostKeyChecking=no "c110705063@nplinux11.cs.nycu.edu.tw:project-3-demo-sample-ezrealux-main/client/multi_client_2/output" .
    if [ ! -f . ]; then
        sshpass -p "$PASSWORD" scp -r -o StrictHostKeyChecking=no "c110705063@nplinux11.cs.nycu.edu.tw:project-3-demo-sample-ezrealux-main/client/multi_client_2/compare.sh" .
    fi
    ./compare.sh $cmp
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