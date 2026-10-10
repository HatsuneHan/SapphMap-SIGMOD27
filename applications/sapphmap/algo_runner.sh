#!/bin/bash
source /startsgxenv.sh

# Use mode HW on machine with SGX hardware
# Use mode SIM on machine without SGX hardware
SGX_MODE=HW

# Algorithms:
# Set to one (or more) of the following algorithms

# Sorting Algorithms:
# KWAYBUTTERFLYOSORT - the flex-way osort 
    # BITONICSORT - recursive bitonic sort 
    # UNOPTBITONICSORT - bitonic sort without recursion
    # CABUCKETSORT - the multi-way bucket sort
    # EXTMERGESORT - the external merge sort (not-oblivious)

# Shuffling Algorithms:
    # KWAYBUTTERFLYOSHUFFLE - the flex-way oshuffle 
    # ORSHUFFLE - OrShuffle
    # BITONICSHUFFLE - shuffle using bitonic sort
    # CABUCKETSHUFFLE - multi-way bucket shuffle

# Applications:
    # HISTOGRAM
    # DBJOIN
    # ORAMINIT
ALGOs=(KWAYBUTTERFLYOSORT BITONICSORT CABUCKETSORT EXTMERGESORT)
MIN_ELEMENT_SIZE=128
MAX_ELEMENT_SIZE=128
MIN_SIZE=524288
MAX_SIZE=371616317

# the size of the enclave heap in MB
# the script will run the program with enclave size from MIN_ENCLAVE_SIZE to MAX_ENCLAVE_SIZE with a step of 2x
MIN_ENCLAVE_SIZE=128
MAX_ENCLAVE_SIZE=128

# the core id of the cpu to run the program
CORE_ID=5

# whether swap data to disk when data size exceeds the enclave heap size
# 0: swap to insecure memory, 1: swap to disk
DISK_IO=0

export CC=/usr/bin/gcc
export CXX=/usr/bin/g++

if [ $DISK_IO = 1 ]
then
    DISK_TAG=_DISK
fi

if [ $MAX_ENCLAVE_SIZE != 128 ]
then
    ENCLAVE_SIZE_TAG=_${MIN_ENCLAVE_SIZE}_${MAX_ENCLAVE_SIZE}
fi

for ALGO in ${ALGOs[@]}; do
FILENAME=${ALGO}_${MIN_ELEMENT_SIZE}_${MAX_ELEMENT_SIZE}_${MIN_SIZE}_${MAX_SIZE}${IO_TAG}${DISK_TAG}${ENCLAVE_SIZE_TAG}.out
if [ -z "$1" ]; then
rm -f $FILENAME
echo "output to "${FILENAME}
fi
for (( encsize=$MIN_ENCLAVE_SIZE; encsize<=$MAX_ENCLAVE_SIZE; encsize*=2 ))
do
heapsizeB=$(( encsize * 1000000 ))
hex_encsize=$(printf '%x\n' $heapsizeB)

sed -i "/.*<Heap.*/c\  <HeapMaxSize>0x"${hex_encsize}"</HeapMaxSize>" ./Enclave/Enclave.config.xml
for (( s=$MIN_ELEMENT_SIZE; s<=$MAX_ELEMENT_SIZE; s=s*3/2 ))
do
    make clean
    make SGX_MODE=$SGX_MODE SGX_PRERELEASE=1 ELEMENT_SIZE=$s ALGO=$ALGO MIN_SIZE=$MIN_SIZE MAX_SIZE=$MAX_SIZE IO_ROUND=1 DISK_IO=$DISK_IO ENCLAVE_SIZE=$encsize
    if [[ $1 = 1 ]]; then
        taskset -c ${CORE_ID} ./sort.elf
        sleep 1
    else
        taskset -c ${CORE_ID} stdbuf -oL nohup ./sort.elf &>> $FILENAME < /dev/null
        sleep 1
        last_line=$(tail -n 1 $FILENAME)

        last_token=$(echo -e "$last_line" | cut -f 3)

        # Check if duration is greater than 4000
        if (( $(echo "$last_token > 4000" | bc -l) )); then
            break
        fi
    fi
done
done
done
# Reset the Enclave.config.xml to default heap size
sed -i '/.*<Heap.*/c\  <HeapMaxSize>0x7A00000</HeapMaxSize>' ./Enclave/Enclave.config.xml