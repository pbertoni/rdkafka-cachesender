#!/bin/bash

if [ "$#" -ne 1 ] ; then
	echo "	usage: cachesend_don_quixote.sh <partition>"
else
	echo "Assuming broker is on machine apu11 (10.20.12.11)"
	./rdkafka_cachesender -t "wc" -p $1 -b "10.20.12.11:9092" -f "quixote.txt"
fi