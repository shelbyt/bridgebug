#!/usr/bin/env bash

# Initialize our own variables:

containers=2

BINFILE=$PWD/allping
if ! test -f "$BINFILE"; then
    echo "$BINFILE Binary does not exist. Run make first. Exiting.."
    echo
    exit 1
fi


if test -z "$1" 
then
    runs=1
else
    runs=$1
fi

docker kill bbdemo0
docker kill bbdemo1

docker rm bbdemo0
docker rm bbdemo1


docker network rm pingbridge

truncate -s 0 iplist
for ((i=0; i<$containers; i++))
do
  echo "172.28.5.$(($i+1))" >> iplist
done

echo
echo "Creating Docker Bridge"
docker network create --driver=bridge --subnet=172.28.0.0/16 --ip-range=172.28.5.0/24 pingbridge

echo
echo "Copying binary and ip list into container"
for ((i=0; i<$containers; i++))
do
docker run -d --net=pingbridge --ip 172.28.5.$((i+1)) --name=bbdemo$i debian sleep infinity
docker cp iplist bbdemo$i:/iplist
docker cp allping bbdemo$i:/allping
done

# Start all after delay because making containers takes time

for ((j=0; j<$runs; j++))
do
delay=$(($containers*3))
time=$(($(date +%s)+$delay))
for ((i=0; i<$containers; i++))
do
docker exec -d bbdemo$i sh -c "/allping $time $i $containers < /iplist > /log_bbdemo$i.dat-$j 2>error$i.dat-$j"
done
# REQURIED: Allping packet timeout
echo
echo "Sleeping 15s so logs can be created and any remaining packets can timeout"
sleep 15
done
ctime=$(date +%s)
RESULTS_DIR=$PWD/$ctime
mkdir $RESULTS_DIR


for ((j=0;j<$runs;j++))
do
for ((i=0;i<$containers;i++))
do
docker cp bbdemo$i:/log_bbdemo$i.dat-$j $RESULTS_DIR
#docker cp bbdemo$i:/error$i.dat .
done
done

for ((i=0; i < $runs;i++))
do
echo "Run $i"
paste -d , $RESULTS_DIR/log_*.dat-$i
echo 
done

