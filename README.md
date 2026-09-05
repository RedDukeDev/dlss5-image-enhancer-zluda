# DLSS 5 Image Enhancer - with AMD/Zluda support
This project is a proof-of-concept tool which uses Nvidia's DLSS5 to enhance a single picture.
It also was made as a testing ground for my attempt of running DLSS5 on AMD gpus, specifically RDNA4 and RDNA3 (tough fp8 emulation).
Older gpus *may* work too, but they aren't the target for this experiment.

It runs thanks to [my own Zluda Fork](https://github.com/RedDukeDev/ZLUDA), which implements the missing features required by the DLSS5 network.

I also forked Zluda's version of LLVM and made a small change which should, in theory, make possible to use the native FP16 hardware on supported cards instead of relying on software emulation, you can find it [HERE](https://github.com/RedDukeDev/llvm-project)

## How to use
Download the zip from the [Release section](https://github.com/RedDukeDev/dlss5-image-enhancer-zluda/releases), and run dlss5-image-enhancer.exe
On the top-right side, you have to select the required DLLs. 
For AMD, nvcuda.dll and nvapi64.dll are already included. they aren't the official nvidia libraries, those are actually from the Zluda project.

nvngx.dll is included too, this isn't the official dll, it's a custom re-implmentation, the source is included in the project under the "ngx_runtime" directory


The "Network" one is the nvngx_dlssnr.dll which is the library that actually contains the DLSS5 code. This one is the official Nvidia library, and it's not included in this project. you have to get it from a game which uses it (for example NBA 2K27), or get it from one of the countless community projects that are using it, like the RenoDX plugin for Reshade.

## Note on cache compilation
On the first launch, the program will have to translate the cuda modules to something that the AMD code can run natively, this will stored in AppData/Local/zluda/ComputeCache.
It will take A LOT of time, but it's only needed once.

## For Nvidia users
I also made a "nvidia mode", which tries to run the dlss using the official drivers. you still need to provide the nvngx_dlssnr.dll library.

NOTE: This feature isn't tested yet, since i don't have an nvidia gpu to test with at the moment.


# Frequently Asked Questions (FAQ)

### Why there isn't a pull request to the official ZLUDA project?
It's because most of the code is AI-generated and i'm not sure at all if all the code actually makes sense of if there's some garbage which shouldn't be there. 
The performance are still painfully bad
I'm not making a pull request containing code that i can't fully understand. But it's still available to everyone, hoping that people more skilled than me can help me and the whole community to achieve a proper way to handle this.

### Why didn't you make something to use this on games?
I actually built an experimental plugin for Reshade, but the performances are so bad that isn't really usable at the moment. I'll probably publish it if i can make some improvement.

### Will this work on Linux?
Not at the moment, but i'll probably try to put some effort to it if i get playable performance.

The problem is that while Zluda itself can work on Linux, it does trough Linux .so libraries, while the dlss5 is designed to run on windows only. and i'm not aware on way to run the windows version of Zluda and ROCm on Proton.

It should be possible, in theory, to make Proton/Wine to bridge nvcuda.dll to libcuda.so, but i didn't tried to that, yet.

### Do you know DLSS-NR-on-AMD by danielblnc?
Yes, i'm aware of that project, but that's totally unrelated to mine.
His approach is by far better performing right now, but since there's no code available, i really can't tell how the two project differ.
