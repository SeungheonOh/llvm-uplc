# LLVM UPLC

JIT Compile UPLC into binary. Currently, this compiles entire UPLC before running which has big initial cost. It would be interesting to implement some analysis engine runs regular CEK and only JIT compiles parts that executed many times but that is way more difficult.

It also has bytecode VM which is regular CEK that doesn't need any JIT compilation. Unlike conventional Plutus VMs, it will decode flat encoded script directly into custom bytecode defined at docs/bytecode.md. 

**This is experimental and NOT production ready** 
