export LD_LIBRARY_PATH=/opt/rocm/lib:~/Desktop/libflame/lib/x86_64-unknown-linux-gnu/:$LD_LIBRARY_PATH

sudo LD_LIBRARY_PATH=/opt/rocm/lib:~/Desktop/libflame/lib/x86_64-unknown-linux-gnu/:$LD_LIBRARY_PATH runTracer.sh ./test_Chol.x < input0
mv trace.rpd trace_1_mstream_patch.rpd

sudo LD_LIBRARYPATH=/opt/rocm/lib:~/Desktop/libflame/lib/x86_64-unknown-linux-gnu/:$LD_LIBRARY_PATH runTracer.sh ./test_Chol.x < input1
mv trace.rpd trace_8_mstream_patch.rpd
