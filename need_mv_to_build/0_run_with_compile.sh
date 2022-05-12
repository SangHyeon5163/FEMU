#echo $1 $2

if [[ $# -lt 4 ]]; then
    echo “Usage: ./0_run.sh [DEVSZ_GB] [POLICY] [BUFFSZ_GB] [PROTECTED_RATIO] [WT_RES]”
    echo “Usage: ./0_run.sh [16 64] [FIFO DAWID] [1 2 4 8] [0.01 0.05] [1 0]”
	#echo "If you want print Write Traffic"
	#echo "-- Usage: ./0_run.sh [16] [FIFO] [1] [0.01] [1] --"
    exit
fi

echo "DEVICE_SIZE $1" > femu.conf
echo "POLICY $2" >> femu.conf
echo "BUFFSZ_GB $3" >> femu.conf
echo "PROTECTED_RATIO $4" >> femu.conf

DEVICE_SIZE=$1
if [ ${DEVICE_SIZE} -eq 64 ];then
        DEVSZ_MB=65536
        BLKS_PER_PLANE=1024    
    
elif [ ${DEVICE_SIZE} -eq 16 ];then
        DEVSZ_MB=16384
        BLKS_PER_PLANE=256
fi


POLICY=$2

BUFFSZ_GB=$3
BUFF_SIZE=1048576
if [[ $BUFFSZ_GB == 1 ]]; then
	BUFF_SIZE=$(expr $BUFF_SIZE / 4)
elif [[ $BUFFSZ_GB == 2 ]]; then
	BUFF_SIZE=$(expr $BUFF_SIZE / 2)
elif [[ $BUFFSZ_GB == 8 ]]; then
	BUFF_SIZE=$(expr $BUFF_SIZE * 2)
fi

echo $BUFF_SIZE

PROTECTED_RATIO=$4
WT_RES=$5

BUFF_THRESHOLD_RATIO=1
BUFF_THRESHOLD=$((BUFF_SIZE/BUFF_THRESHOLD_RATIO))

echo $DEVSZ_MB $BLKS_PER_PLANE $POLICY $BUFF_SIZE $BUFF_THRESHOLD $PROTECTED_RATIO

cat run-blackbox-orig.sh | sed -e "s/devsz_mb=16384/devsz_mb=$DEVSZ_MB/" > run-blackbox.sh

#echo CHEKC
if [ ${WT_RES} -eq 1 ];then
	cat ../hw/block/femu/bbssd/ftl_orig.h | sed -e "s/P_BLKS_PER_PL 256/P_BLKS_PER_PL $BLKS_PER_PLANE/" \
			| sed -e "s/PROTECTED_RATIO 0.5/PROTECTED_RATIO $PROTECTED_RATIO/" \
	        | sed -e "s/#define FIFO/#define $POLICY/" \
			| sed -e "s_//#define RES_#define RES_" \
	        | sed -e "s/BUFF_SIZE 65536/BUFF_SIZE $BUFF_SIZE/" \
	        | sed -e "s/BUFF_THRESHOLD 32768/BUFF_THRESHOLD $BUFF_THRESHOLD/" > ../hw/block/femu/bbssd/ftl.h
elif [ ${WT_RES} -eq 0 ];then
	cat ../hw/block/femu/bbssd/ftl_orig.h | sed -e "s/P_BLKS_PER_PL 256/P_BLKS_PER_PL $BLKS_PER_PLANE/" \
			| sed -e "s/PROTECTED_RATIO 0.5/PROTECTED_RATIO $PROTECTED_RATIO/" \
	        | sed -e "s/#define FIFO/#define $POLICY/" \
	        | sed -e "s/BUFF_SIZE 65536/BUFF_SIZE $BUFF_SIZE/" \
	        | sed -e "s/BUFF_THRESHOLD 32768/BUFF_THRESHOLD $BUFF_THRESHOLD/" > ../hw/block/femu/bbssd/ftl.h
fi    

./femu-compile.sh
./run-blackbox.sh

