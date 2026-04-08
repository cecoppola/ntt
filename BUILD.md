 Libraries
 We're going to use the Cray HPE programming environment for AMD HIP ROCm. So we're going to be relying on functions in the ROCm library as well as the Cray Scientific Library for both CPU and GPU.

Here are some of the modules available on the target system:
 PrgEng-cray-amd/8.5.0
 cray-mpich/9.0.1
 cray-libsci/25.09.0
 cray-hdf5-parallel/1.14.3.7
 crayclang/20.0
 cray-openshmemx/11.8.0
 cray-cti/2.20.0
 cray-libsci_acc/25.03.0
 cray-dsmml/0.3.1
 craype-accel-amd-gfc942
 craype-x86-genoa
 amd/7.0.3
 rocm/7.0.3
 gsl/2.8

It would be helpful to begin making our code compatible with these and writing it so it will interact with these smoothly later.