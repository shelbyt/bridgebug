#!/usr/bin/env bash

# Initialize our own variables:

containers=2

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


docker network create --driver=bridge --subnet=172.28.0.0/16 --ip-range=172.28.5.0/24 pingbridge

for ((i=0; i<$containers; i++))
do
docker run -d --net=pingbridge --ip 172.28.5.$((i+1)) --name=bbdemo$i debian sleep infinity
docker cp iplist bbdemo$i:/iplist
docker cp allping bbdemo$i:/allping
done

# Start all after delay because making containers takes time
delay=$(($containers*2))
time=$(($(date +%s)+$delay))

for ((i=0; i<$containers; i++))
do
docker exec -d bbdemo$i sh -c "/allping $time $i $containers < /iplist > /log_bbdemo$i.dat 2>error$i.dat"
done

# REQURIED: Allping packet timeout
sleep 10

for ((i=0;i<$containers;i++))
do
docker cp bbdemo$i:/log_bbdemo$i.dat .
#docker cp bbdemo$i:/error$i.dat .
done

paste -d , log_*.dat

