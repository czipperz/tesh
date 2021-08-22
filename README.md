# MYPROJECT

## Building

1. Clone the repository and the submodules.

```
git clone MYPROJECTPATH
cd MYPROJECT
git submodule init
git submodule update
```

2. Build MYPROJECT by running (on all platforms):

```
./build-release
```

3. After building, MYPROJECT can be ran via `./build/release/MYPROJECT`.

## Optimizing
We use Tracy to optimize MYPROJECT.  See the
[manual](https://bitbucket.com/wolfpld/tracy/downloads/tracy.pdf) for more information.

To prepare we have to build MYPROJECT with Tracy enabled and also build Tracy's
profiler.  Once both are built, we then run the profiler and MYPROJECT at the same time.

Build MYPROJECT with Tracy enabled:
```
./build-tracy
```

Build `tracy/profiler` by following the instructions in the Tracy manual.  On *nix:
```
cd tracy/profiler/build/unix
make release
```

Then we run Tracy:
```
./tracy/profiler/build/unix/Tracy-release
```

Then run MYPROJECT with Tracy enabled.  Run it as the
super user to enable context switching recognition.
```
sudo ./build/tracy/MYPROJECT
```
