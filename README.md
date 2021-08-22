# MYPROJECT

This repository provides a base project for C++ projects.

This repository is licensed under GPL3.  If you wish to
purchase a different license, email czipperz AT gmail DOT com.

Features:
* CMake build script.
* cz and Tracy integration.
* GNU Global tag generation.
* Build scripts (debug, release, release-debug, tracy).
* Catch2 tests.

Getting started:
1. Clone the repository by following step 1 of [Building](#Building).
2. Change all instances of `MYPROJECTURL` with the url of your project.
3. Change all instances of `MYPROJECT` with the name of your project.
4. Delete this part of the readme.
5. Relicense the project.

The template project builds a program.  To make a library:
1. Change step 3 of the build instructions below.
2. Delete or rewrite the [Optimizing](#Optimizing) section.
3. Make the following changes to `CMakeLists.txt`:
   a. Remove all lines including `PROGRAM_NAME`.
   b. Change `LIBRARY_NAME` to `${PROJECT_NAME}` (no `-test` suffix).

## Building

1. Clone the repository and the submodules.

```
git clone MYPROJECTURL
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

You can use Tracy to profile your project.  See [the Tracy manual] for more information.

[the Tracy manual]: https://bitbucket.com/wolfpld/tracy/downloads/tracy.pdf

First we have to build the Tracy configuration of MYPROJECT and build the Tracy
profiler.  Then run the profiler, and finally run the Tracy build of MYPROJECT.

Build MYPROJECT with Tracy enabled:
```
./build-tracy
```

Build `tracy/profiler` by following the instructions in the Tracy manual.  For example, on *nix:
```
cd tracy/profiler/build/unix
make release
```

Then we run Tracy:
```
./tracy/profiler/build/unix/Tracy-release &
```

Then run MYPROJECT with Tracy enabled.  Run it as the
super user to enable context switching recognition.
```
sudo ./build/tracy/MYPROJECT
```
