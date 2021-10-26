# TESH

A combined shell and terminal emulator.  Bash compatible.
Focused on increasing scanability, readability, and navigatability of output.

This repository is licensed under GPL3.  If you wish to
purchase a different license, email czipperz AT gmail DOT com.

Features:
* Unique color -- each script (a line of input) is given a unique background
  color allowing you to easily see which script cause which outputs.
* Auto paging -- terminal scrolls until the prompt is at the top of the screen.
* Easily distinguished input -- all user input has a
  teal foreground color making it easily stand out.
* Script navigation -- `Ctrl + Alt + b` and `Ctrl + Alt + f`
  seek backwards and forwards based on prompt.
* Attach -- toggle having the terminal attached to a script with `Ctrl + Z`.
  When attached, user input is sent to the process's `stdin`.

## Building

1. Clone the repository and the submodules.

```
git clone https://github.com/czipperz/tesh
cd tesh
git submodule init
git submodule update
```

2. Build tesh by running (on all platforms):

```
./build-release
```

3. After building, Tesh can be ran via `./build/release/tesh`.

## Optimizing

You can use Tracy to profile your project.  See [the Tracy manual] for more information.

[the Tracy manual]: https://bitbucket.com/wolfpld/tracy/downloads/tracy.pdf

First we have to build the Tracy configuration of Tesh and build the Tracy
profiler.  Then run the profiler, and finally run the Tracy build of Tesh.

Build Tesh with Tracy enabled:
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

Then run Tesh with Tracy enabled.  Run it as the
super user to enable context switching recognition.
```
sudo ./build/tracy/tesh
```
