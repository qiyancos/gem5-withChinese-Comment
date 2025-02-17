#! /bin/bash
localDir=$PWD
root=`dirname $0`
cd $root/..
root=$PWD

cpuNum=`lscpu | awk '/^CPU\(s\):/{print $2}'`
#testList="401.bzip2"
#testList="401.bzip2 437.leslie3d 458.sjeng 470.lbm 473.astar"
#testList="482.sphinx3 464.h264ref 445.gobmk 447.dealII 437.leslie3d 401.bzip2 470.lbm 473.astar 458.sjeng"
testList="400.perlbench 401.bzip2 403.gcc 435.gromacs 437.leslie3d 444.namd 445.gobmk 447.dealII 450.soplex 453.povray 456.hmmer 458.sjeng 459.GemsFDTD 462.libquantum 464.h264ref 470.lbm 471.omnetpp 473.astar 481.wrf 482.sphinx3 483.xalancbmk"
#eval "$(grep "^testList=" $root/script/auto_run_single_mix.sh)"
#eval "$(grep "^testTarget=" $root/script/auto_run_single_mix.sh)"
testTarget="l2_ppf_prefetcher_type_test"
eval "$(grep "^testFolder=" $root/script/auto_run_single_mix.sh)"
statsTitle="TestName, Task Number, Test Subset"
statsList=("IPC"
        #"Total_Inst"
        #"Total_Read_Inst"
        #"Total_Write_Inst"
        #"Total_Float_Read_Inst"
        #"Total_Float_Write_Inst"
        "L1D_Block_NoMSHR"
        "L1D_Block_NoTarget"
        #"L1D_Miss_Rate"
        "L1D_Demand_Miss_Rate"
        #"L1D_Pref_Issued"
        #"L1D_Pref_Identidied"
        #"L1D_Pref_BufferHit"
        #"L1D_Pref_CacheHit"
        #"L1D_Pref_RemoveFull"
        #"L1D_Pref_RemoveCrossPage"
        #"L2_Miss_Rate"
        "L2_Demand_Miss_Rate"
        #"L2_Pref_Issued"
        #"L2_Pref_Identidied"
        #"L2_Pref_BufferHit"
        #"L2_Pref_CacheHit"
        "L2_Pref_RemoveFull"
        "L2_Pref_RemoveCrossPage"
        #"PPF_L1D_Demand_Hit"
        #"PPF_L1I_Demand_Hit"
        #"PPF_L2_Demand_Hit"
        #"PPF_L3_Demand_Hit"
        "PPF_L1D_Demand_Miss"
        "PPF_L1I_Demand_Miss"
        "PPF_L2_Demand_Miss"
        "PPF_L3_Demand_Miss"
        #"PPF_L2_Accepted_Pref"
        #"PPF_L2_Rejectted_Pref"
        #"PPF_L2_Threshing_Pref"
        "PPF_L2_Untrained_Pref"
        #"PPF_L2_Untrained_Dmd"
        "PPF_L2_Train_DismissedPref"
        "PPF_L2_Train_GoodPref"
        "PPF_L2_Train_BadPref"
        "PPF_L2_Train_UselessPref"
        #"PPF_L2_Train_DemandMiss"
        "PPF_L2_To_L1DCache_Pref"
        "PPF_L2_To_L2Cache_Pref"
        "PPF_L2_To_L3Cache_Pref"
        "PPF_L2_To_DRAM_Pref"
        "PPF_L2_Process_Time_L1D"
        "PPF_L2_Process_Time_L2"
        "PPF_L2_Process_Time_L3"
        "PPF_L2_Process_Time_DRAM"
        "PPF_L2_Waiting_Time"
        #"PPF_L2_Dismissed_LevelDown"
        #"PPF_L2_Dismissed_LevelUp_Late"
        #"PPF_L2_Dismissed_LevelUp_NoWB"
        #"PPF_L2_Pref_Hit_L3"
        #"PPF_L2_Pref_Processed_L1D"
        #"PPF_L2_Pref_Processed_L2"
        #"PPF_L2_Pref_Processed_L3"
        #"PPF_L2_Pref_Processed_DRAM"
        "PPF_L2_Shadowed_Pref"
        "PPF_L2_Squeezed_Pref"
        "PPF_L1_Stats_Count"
        "PPF_L2_Stats_Count"
        #"PPF_L2_Total_CrossCore_Useful_Value"
        #"PPF_L2_Total_SingleCore_Useful_Value"
        "PPF_L2_Type_CrossCore_Harmful"
        "PPF_L2_Type_CrossCore_Useful"
        "PPF_L2_Type_Selfish"
        "PPF_L2_Type_Selfless"
        "PPF_L2_Type_SingleCore_Harmful"
        "PPF_L2_Type_SingleCore_Useful"
        "PPF_L2_Type_Useless"
        )

#################################################################

singleStatsList=("L3_Miss_Rate"
        "L3_Demand_Miss_Rate")

singleStatsString=("l3.overall_miss_rate::total"
        "l3.demand_miss_rate::total")

multiStatsList=("IPC"
        "Total_Inst"
        "Total_Read_Inst"
        "Total_Write_Inst"
        "Total_Float_Read_Inst"
        "Total_Float_Write_Inst"
        "L1D_Block_NoMSHR"
        "L1D_Block_NoTarget"
        "L1D_Miss_Rate"
        "L1D_Demand_Miss_Rate"
        "L1D_Pref_Issued"
        "L1D_Pref_Identidied"
        "L1D_Pref_BufferHit"
        "L1D_Pref_CacheHit"
        "L1D_Pref_RemoveFull"
        "L1D_Pref_RemoveCrossPage"
        "L2_Miss_Rate"
        "L2_Demand_Miss_Rate"
        "L2_Pref_Issued"
        "L2_Pref_Identidied"
        "L2_Pref_BufferHit"
        "L2_Pref_CacheHit"
        "L2_Pref_RemoveFull"
        "L2_Pref_RemoveCrossPage"
        "PPF_L1D_Demand_Hit"
        "PPF_L1I_Demand_Hit"
        "PPF_L2_Demand_Hit"
        "PPF_L3_Demand_Hit"
        "PPF_L1D_Demand_Miss"
        "PPF_L1I_Demand_Miss"
        "PPF_L2_Demand_Miss"
        "PPF_L3_Demand_Miss"
        "PPF_L2_Accepted_Pref"
        "PPF_L2_Rejectted_Pref"
        "PPF_L2_Threshing_Pref"
        "PPF_L2_Untrained_Pref"
        "PPF_L2_Untrained_Dmd"
        "PPF_L2_To_L1DCache_Pref"
        "PPF_L2_To_L2Cache_Pref"
        "PPF_L2_To_L3Cache_Pref"
        "PPF_L2_To_DRAM_Pref"
        "PPF_L2_Train_DismissedPref"
        "PPF_L2_Train_GoodPref"
        "PPF_L2_Train_BadPref"
        "PPF_L2_Train_UselessPref"
        "PPF_L2_Train_DemandMiss"
        "PPF_L2_Process_Time_L1D"
        "PPF_L2_Process_Time_L2"
        "PPF_L2_Process_Time_L3"
        "PPF_L2_Process_Time_DRAM"
        "PPF_L2_Waiting_Time"
        "PPF_L2_Dismissed_LevelDown"
        "PPF_L2_Dismissed_LevelUp_Late"
        "PPF_L2_Dismissed_LevelUp_NoWB"
        "PPF_L2_Pref_Hit_L3"
        "PPF_L2_Pref_Processed_L1D"
        "PPF_L2_Pref_Processed_L2"
        "PPF_L2_Pref_Processed_L3"
        "PPF_L2_Pref_Processed_DRAM"
        "PPF_L2_Shadowed_Pref"
        "PPF_L2_Squeezed_Pref"
        "PPF_L1_Stats_Count"
        "PPF_L2_Stats_Count"
        "PPF_L2_Total_CrossCore_Useful_Value"
        "PPF_L2_Total_SingleCore_Useful_Value"
        "PPF_L2_Type_CrossCore_Harmful"
        "PPF_L2_Type_CrossCore_Useful"
        "PPF_L2_Type_Selfish"
        "PPF_L2_Type_Selfless"
        "PPF_L2_Type_SingleCore_Harmful"
        "PPF_L2_Type_SingleCore_Useful"
        "PPF_L2_Type_Useless"
        )

multiStatsString=("ipc_total"
        "commit.op_class_0::total"
        "commit.op_class_0::MemRead"
        "commit.op_class_0::MemWrite"
        "commit.op_class_0::FloatMemRead"
        "commit.op_class_0::FloatMemWrite"
        "dcache.blocked_cycles::no_mshrs"
        "dcache.blocked_cycles::no_targets"
        "dcache.overall_miss_rate::total"
        "dcache.demand_miss_rate::total"
        "dcache.prefetcher.num_hwpf_issued"
        "dcache.prefetcher.pfIdentified"
        "dcache.prefetcher.pfBufferHit"
        "dcache.prefetcher.pfInCache"
        "dcache.prefetcher.pfRemovedFull"
        "dcache.prefetcher.pfSpanPage"
        "l2.overall_miss_rate::total"
        "l2.demand_miss_rate::total"
        "l2.prefetcher.num_hwpf_issued"
        "l2.prefetcher.pfIdentified"
        "l2.prefetcher.pfBufferHit"
        "l2.prefetcher.pfInCache"
        "l2.prefetcher.pfRemovedFull"
        "l2.prefetcher.pfSpanPage"
        "demand_requests_hit_L1DCache"
        "demand_requests_hit_L1ICache"
        "demand_requests_hit_L2Cache"
        "demand_requests_hit_L3Cache"
        "demand_requests_miss_L1DCache"
        "demand_requests_miss_L1ICache"
        "demand_requests_miss_L2Cache"
        "demand_requests_miss_L3Cache"
        "ppf_prefetch_accepted_from_L2Cache"
        "ppf_prefetch_rejected_from_L2Cache"
        "ppf_prefetch_threshing_from_L2Cache"
        "ppf_prefetch_untrained_pref_from_L2Cache"
        "ppf_prefetch_untrained_dmd_from_L2Cache"
        "ppf_prefetch_sent_from_L2Cache_to_L1DCache"
        "ppf_prefetch_sent_from_L2Cache_to_L2Cache"
        "ppf_prefetch_sent_from_L2Cache_to_L3Cache"
        "ppf_prefetch_sent_from_L2Cache_to_DRAM"
        "ppf_prefetch_dismissed_pref_training_from_L2Cache"
        "ppf_training_type_GoodPref_for_L2Cache"
        "ppf_training_type_BadPref_for_L2Cache"
        "ppf_training_type_UselessPref_for_L2Cache"
        "ppf_training_type_DemandMiss_for_L2Cache"
        "prefetch_avg_process_cycle_from_L2Cache_in_L1DCache"
        "prefetch_avg_process_cycle_from_L2Cache_in_L2Cache"
        "prefetch_avg_process_cycle_from_L2Cache_in_L3Cache"
        "prefetch_avg_process_cycle_from_L2Cache_in_DRAM"
        "prefetch_avg_waiting_cycle_from_L2Cache"
        "prefetch_dismissed_level_down_L2Cache"
        "prefetch_dismissed_level_up_late_L2Cache"
        "prefetch_dismissed_level_up_nowb_L2Cache"
        "prefetch_from_L2Cache_hit_L3Cache"
        "prefetch_from_L2Cache_processed_in_L1DCache"
        "prefetch_from_L2Cache_processed_in_L2Cache"
        "prefetch_from_L2Cache_processed_in_L3Cache"
        "prefetch_from_L2Cache_processed_in_DRAM"
        "prefetch_shadowed_L2Cache"
        "prefetch_squeezed_L2Cache"
        "prefetch_stats_count_from_L1DCache"
        "prefetch_stats_count_from_L2Cache"
        "prefetch_total_useful_value_from_L2Cache_type_cross_core"
        "prefetch_total_useful_value_from_L2Cache_type_single_core"
        "prefetch_useful_type_from_L2Cache_cross_core_harmful"
        "prefetch_useful_type_from_L2Cache_cross_core_useful"
        "prefetch_useful_type_from_L2Cache_selfish"
        "prefetch_useful_type_from_L2Cache_selfless"
        "prefetch_useful_type_from_L2Cache_single_core_harmful"
        "prefetch_useful_type_from_L2Cache_single_core_useful"
        "prefetch_useful_type_from_L2Cache_useless"
        )

################################################################

singleStats() {
    statsIndex=0
    while [ x${singleStatsList[$statsIndex]} != x ]
    do
        if [ ${singleStatsList[$statsIndex]} = $1 ]
        then break
        else statsIndex=$[statsIndex + 1]
        fi
    done
    if [ $statsIndex = ${#singleStatsList[*]} ]
    then
        echo "Error: Stats \"$1\" not found in single stats list."
        exit -1
    fi
    statsString=${singleStatsString[$statsIndex]}
    statsVal=`eval "awk '/$statsString/ {print \\$2}' $2"`
    if [ x$(eval "echo \$statsFlag$1") = x ]
    then
        statsTitle="$statsTitle, $1"
        eval "statsFlag$1=1"
    fi
    echo -n "$statsVal, "
}

multiStats() {
    statsIndex=0
    while [ x${multiStatsList[$statsIndex]} != x ]
    do
        if [ ${multiStatsList[$statsIndex]} = $1 ]
        then break
        else statsIndex=$[statsIndex + 1]
        fi
    done
    if [ $statsIndex = ${#multiStatsList[*]} ]
    then
        echo "Error: Stats \"$1\" not found in multi stats list."
        exit -1
    fi
    statsString=${multiStatsString[$statsIndex]}
    if [ "x`echo $statsString | grep "::total"`" != x ]
    then statsVal=`eval "awk '/$statsString/ {print \\$2}' $2"`
    else statsVal=`eval "awk '/$statsString/ {print \\$1, \\$2}' $2 | sed '/::total/d' | awk '{print \\$2}'"`
    fi
    if [ x$(eval "echo \$statsFlag$1") = x ]
    then
        eval "statsFlag$1=1"
        statsTitle="$statsTitle, $1"
        if [ "`echo ${statsVal}x`" = x ]
        then echo -n "-, "
        else echo -n "`echo "$taskNum ${statsVal}" | $root/script/avg`, "
        fi
    else
        if [ "`echo ${statsVal}x`" = x ]
        then echo -n "-, "
        else echo -n "`echo "$taskNum ${statsVal}" | $root/script/avg`, "
        fi
    fi
}

statsFile() {
    taskDir=$root/data/$testFolder/$file/$testName
    fileName="$taskDir/stats*"
    if [ -f $fileName ]
    then
        echo -n "$file, $taskNum, $testName, " >> $1
        for stats in ${statsList[*]}
        do
            if [ "x`echo ${multiStatsList[*]} | grep $stats`" != x ]
            then multiStats $stats $fileName >> $1
            elif [ "x`echo ${singleStatsList[*]} | grep $stats`" != x ]
            then singleStats $stats $fileName >> $1
            else
                echo "Error: Stats \"$stats\" not found in statsList."
                exit -1
            fi
        done
        echo >> $1
    else echo "$file, $taskNum, $testName, Error" >> $1
    fi
}

if [ x$1 = x ]
then
    echo ">> Result will be saved to $localDir/stats.csv"
    targetFile=$localDir/stats.csv
else targetFile=$1
fi
rm -rf $targetFile
mkdir -p `dirname $targetFile`
if [ $? != 0 ]
then
    echo "Error: Failed to build dir for target \"$targetFile\"."
    exit -1
fi

for target in $testTarget
do
    testFile=`cd $root/test_script && find ./$target -type f`
    for file in $testFile
    do
        echo "-- Processing Test File $file"
        for testName in $testList
        do
            taskNum=`basename $file | sed 's/task\([0-9]*\).*/\1/g'`
            statsFile $targetFile
            echo "   $testName Done."
        done
    done
done
sed -i "1i\\$statsTitle" $targetFile
