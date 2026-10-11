export COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1
export ECALC_NP=auto                         
export RNS_DIST_CACHE_FIT=1                  
export RNS_DIST_CACHE_PARTIAL=1              
export MN_OUT_DKM_HI=1                       
export MN_T_CHUNK_MB=1024                    
export DM_MN_LEAN=1                          
export COMM_LAYER_VSLOT_SHARE=1              
export COMM_OFI=1                            
export COMM_SHMEM_ROUND_MB=1024              
export COMM_SHMEM_POOL_MB=1536               
export SHMEM_SYMMETRIC_HEAP_SIZE=2048M XT_SYMMETRIC_HEAP_SIZE=2048M 
export MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0
export ECALC_MEM_GUARD_GB=6                  
export ECALC_VERBOSE=2 MEM_REPORT_DEVS=1
unset ECALC_CHECKPOINT ECALC_CKPT_TOP BS_CKPT_DIR  
