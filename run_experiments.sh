#!/bin/bash

#
#	============================================================================
#	Name        : run_experiments.sh
#	Author      : Azu Parallel Algorithms and Systems Lab
#	Version     : 1.0
#	Copyright   : 
#	Description : Run experiments to test search algorithms, for example "./run_experiments test_dna 40 originalVWD depthfirstVWD"
#	============================================================================
#

if [ $# -le 2 ]
then
        echo "./run_experiments <test_file> <max #threads> <executable>*"
        exit
fi


folder="$(date +%Y-%m-%d_%H.%M.%S)"
mkdir $folder
for ((i=1; i<=$2; i++))
do
        output[$i]=$folder"/output_"$i"t"
        touch ${output[$i]}
done

success=0

for (( k=3; k<=$#; k++ ))
do
        eval arg=\$$k
        echo "### "$arg" ###"

        for ((j=1; j<=$2; j++))
        do
                echo $j" Thread(s)"

                echo "###"$arg"###" >> ${output[$j]}
                echo >> ${output[$j]}

                command="./"$arg" -p "$j" "$1" >> "${output[$j]}" 2>> "${output[$j]}
                eval $command

                echo "" >> ${output[$j]}
        done
done
echo "Done."

