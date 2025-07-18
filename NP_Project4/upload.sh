#!/bin/bash

# 密碼
PASSWORD="wilsonliao1226"

sshpass -p "$PASSWORD" scp makefile c110705063@nplinux10.cs.nycu.edu.tw:
sshpass -p "$PASSWORD" scp socks_server.cpp c110705063@nplinux10.cs.nycu.edu.tw:
sshpass -p "$PASSWORD" scp console.cpp c110705063@nplinux10.cs.nycu.edu.tw:
sshpass -p "$PASSWORD" scp makefile c110705063@nplinux10.cs.nycu.edu.tw:project-5-demo-sample-ezrealux-main/src/110705063
sshpass -p "$PASSWORD" scp socks_server.cpp c110705063@nplinux10.cs.nycu.edu.tw:project-5-demo-sample-ezrealux-main/src/110705063
sshpass -p "$PASSWORD" scp console.cpp c110705063@nplinux10.cs.nycu.edu.tw:project-5-demo-sample-ezrealux-main/src/110705063
sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "c110705063@nplinux10.cs.nycu.edu.tw" bash -c "make"
make