#! /bin/sh -e

# eg 00000000d1dab9aad25682481f5bfb272795b76f5bfe5af19a07230cb7c53a0f
BLOCK=$1
NUM_TXS=$2

count=0
while [ $count -lt $NUM_TXS ]; do
    # Get TXIDS, excluding coinbase.
    TXS=`bitcoind getblock $BLOCK | sed -n 's/^        "\(.*\)",\?/\1/p' | tail -n +2`
    for tx in $TXS; do
	echo "$tx":`bitcoind getrawtransaction $tx`
	count=$(($count + 1))
    done
    BLOCK=`bitcoind getblock $BLOCK | sed -n 's/    "previousblockhash" : "\(.*\)",\?/\1/p'`
done
	
