# DLSS 5 Image Enhancer - with AMD/Zluda support
This project is a proof-of-concept tool which uses Nvidia's DLSS5 to enhance a single picture.
It also was made as a testing ground for my attempt of running DLSS5 on AMD gpus, specifically RDNA4 and RDNA3 (through fp8 emulation).
Older gpus *may* work too, but they aren't the target for this experiment.

It runs thanks to [my own Zluda Fork](https://github.com/RedDukeDev/ZLUDA), which implements the missing features required by the DLSS5 network.

I also forked Zluda's version of LLVM and made a small change which should, in theory, make possible to use the native FP16 hardware on supported cards instead of relying on software emulation, you can find it [HERE](https://github.com/RedDukeDev/llvm-project)

## How to use
Download the zip from the [Release section](https://github.com/RedDukeDev/dlss5-image-enhancer-zluda/releases), and run dlss5-image-enhancer.exe
On the top-right side, you have to select the required DLL.

The "Network" DLL is the nvngx_dlssnr.dll which is the library that actually contains the DLSS5 code. This one is the official Nvidia library, and it's not included in this project. you have to get it from a game which uses it (for example NBA 2K27), or get it from one of the countless community projects that are using it, like the RenoDX plugin for Reshade.
NOTE: many projects ship a custom version of the DLSS5 library modified to run on the RTX 4000, 3000 and 2000 series. That library cannot be used with Zluda, we need the original one. Should instead be fine for Nvidia users.

AMD users will also have to install the [official HIP SDK for Windows](https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html)

## For Nvidia users
I also made a "nvidia mode", which tries to run the dlss using the official drivers. you still need to provide the nvngx_dlssnr.dll library.

# Building from source
To build the application, you can use the "build.bat" script. It requires the QT toolkit for MSVC 2022 to be installed in `C:\Qt\6.11.2\msvc2022_64`
The windows version of my Zluda fork is compiled using MSVC too, and that's why the script uses that by default.
It should in theory be possible to use the mingw toolkit too, but you'll need to manually compile the Zluda libraries too.
I did not test mingw for this project.

## Note on cache compilation
The program needs the cuda modules to be translated to something that the AMD code can run natively. 
Zluda does exactly that, but some steps require lots of time.
That's why the program comes with a pre-built cache, which is shipped into `zluda/ComputeCache`.

If the cache file cannot be found for any reason, the program is still able to build it itself, but that takes A LOT of time. The cache will be stored in `AppData/Local/zluda/ComputeCache`.

### Building the cache manually (for releases)
If you compiled the project from source and made some changes to the Zluda code, you may want to re-build the cache manually.
The program itself is able to build it, but only for the GPU you are using. For releasing a cache file which can be used by anyone, you can use the `build_cache.bat` script.

You'll need to run it as `build_cache.bat <path to nvngx_dlssnr.dll>`. It reads the code modules out of the network, translates them once per GPU target, and merges everything into `dist\zluda\ComputeCache\zluda2.db`.

It needs ZLUDA's `nvcuda.dll` in `dist\zluda`, an AMD GPU with the HIP SDK, and Python, since the script relies on `tools\prepare_cache.py`.

The script currently builds the cache for all the generic targets for RDNA2, RDNA3 and RDNA4, and also for the gfx1100 and gfx1101 targets, since there are some custom optimizations for those cards. You can select which targets to build by editing `NATIVE_TARGETS` and `GENERIC_TARGETS` in the `tools\prepare_cache.py` python script.

# Known Issues
Nothing at the moment. if you find any, please report it into the "issues" section on GitHub.

# Frequently Asked Questions (FAQ)

### Why there isn't a pull request to the official ZLUDA project?
It's because most of the code is AI-generated and i'm not sure at all if all the code actually makes sense or if there's some garbage which shouldn't be there. 
The performance are still painfully bad
I'm not making a pull request containing code that i can't fully understand. But it's still available to everyone, hoping that people more skilled than me can help me and the whole community to achieve a proper way to handle this.

### Why didn't you make something to use this on games?
I actually built an experimental plugin for Reshade, but the performances are so bad that isn't really usable at the moment. I'll probably publish it if i can make some improvement.

### Will this work on Linux?
Not at the moment, but i'll probably try to put some effort to it if i get playable performance.

The problem is that while Zluda itself can work on Linux, it does through Linux .so libraries, while the dlss5 is designed to run on windows only. and i'm not aware on way to run the windows version of Zluda and ROCm on Proton.

It should be possible, in theory, to make Proton/Wine to bridge nvcuda.dll to libcuda.so, but i didn't try to do that, yet.

### Do you know DLSS-NR-on-AMD by danielblnc?
Yes, i'm aware of that project, but that's totally unrelated to mine.
His approach is by far better performing right now, but since there's no code available, i really can't tell how the two projects differ.
