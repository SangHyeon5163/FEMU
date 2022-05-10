#echo $1 $2

DEVICE_SIZE=$1
if [ ${DEVICE_SIZE} -eq 64 ];then
        DEVSZ_MB=65536
        BLKS_PER_PLANE=1024    
    
elif [ ${DEVICE_SIZE} -eq 16 ];then
        DEVSZ_MB=16384
        BLKS_PER_PLANE=256
fi

POLICY=$2
BUFF_SIZE=$3
BUFF_THRESHOLD_RATIO=2 
BUFF_THRESHOLD=$((BUFF_SIZE/BUFF_THRESHOLD_RATIO))

echo $DEVSZ_MB $BLKS_PER_PLANE $POLICY $BUFF_SIZE $BUFF_THRESHOLD

cat run-blackbox-orig.sh | sed -e "s/devsz_mb=16384/devsz_mb=$DEVSZ_MB/" > run-blackbox.sh

#echo CHEKC
cat ../hw/block/femu/bbssd/ftl_orig.h | sed -e "s/P_BLKS_PER_PL 256/P_BLKS_PER_PL $BLKS_PER_PLANE/" \
        | sed -e "s/#define FIFO/#define $POLICY/" \
        | sed -e "s/BUFF_SIZE 65536/BUFF_SIZE $BUFF_SIZE/" \
        | sed -e "s/BUFF_THRESHOLD 32768/BUFF_THRESHOLD $BUFF_THRESHOLD/" > ../hw/block/femu/bbssd/ftl.h
    

./femu-compile.sh
./run-blackbox.sh

