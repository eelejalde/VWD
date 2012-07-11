#
#	============================================================================
#	Name        : build.sh
#	Author      : Azu Parallel Algorithms and Systems Lab
#	Version     : 1.0
#	Copyright   : 
#	Description : build executable files
#	============================================================================
#


gcc -DCOUNT_EVENTS=1 -O0 -g -c -o wool.o -lpthread -lm -lrt wool.c

gcc -DCOUNT_EVENTS=1 -g -O0 -Wall -o originalVWD originalVWD.c  ./wool.o -lm -lpthread

gcc -DCOUNT_EVENTS=1 -g -O0 -std=gnu99 -Wall -o depthfirstVWD depthfirstVWD.c  ./wool.o -lm -lpthread

echo "Done!"
