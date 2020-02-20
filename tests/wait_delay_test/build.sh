#!/bin/bash

gcc test.c -l pthread -o wake_up_readers
gcc test.c -l pthread -D SEND_DELAY=0 -o wake_up_readers_instant
gcc test.c -l pthread -D SEND_DELAY=2000000 -o readers_timeout
gcc test.c -l pthread -D SEND_DELAY=2000000 -D REVOKE -o readers_timeout_revoke
gcc test.c -l pthread -D SEND_DELAY=2000000 -D DONT_WAIT -o readers_flush
