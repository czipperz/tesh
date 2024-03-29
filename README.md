# TESH

A combined shell and terminal emulator.  Bash compatible.
Focused on increasing scanability, readability, and navigability of output.

This repository is licensed under GPL3.  If you wish to
purchase a different license, email czipperz AT gmail DOT com.

Features:
* Separate command output
  - You can run multiple asynchronous commands at once
    without having them trample each-others' outputs or trampling the prompt.
  - Each command has a unique background color.
  - Commands are automatically timed and timestamped to ease comprehension.
* Auto paging
  - Tesh scrolls until the prompt is at the top of the screen.
  - No more piping to `less` to be able to page through your results!
* Easily distinguished input
  - All user input has a teal foreground color making it easily stand out.
* Script navigation
  - `Ctrl + Alt + b` and `Ctrl + Alt + f` seek backwards and forwards based on prompt.
  - Scripts can be reordered and/or hidden.
* Detach
  - Toggle having the terminal attached to a script with `Ctrl + Z`.
  - When attached, user input is sent to the process's `stdin`.
  - When detached, user input is appended to the prompt.
* Instantaneous path, file, and history completion.
* Full prompt manipulation available when editing `stdin` (a la readline) for every command.
* Customizable key bindings via shell functions.
  - `__tesh_F7` will run when `F7` is pressed.
  - Modifier keys `ctrl`, `alt`, and `shift` can be used as well:
    `__tesh_ctrl_alt_shift_F7`.
  - Control + Click on a link runs `__tesh_open`.
  - Control + Shift + E will dump the selected command's
    output to a file and run `__tesh_edit $FILE`.






## Building

1. Install the dependencies (see below).

2. Clone the repository and the submodules.

```
git clone https://github.com/czipperz/tesh
cd tesh
git submodule init
git submodule update
```

3. Build tesh by running (on all platforms):

```
./build-release
```

4. After building, Tesh can be ran via `./build/release/tesh`.

### Linux and OSX

Required packages: a C++ compiler, ncurses, and SDL (`SDL2`, `SDL2_image`, `SDL2_ttf`).

On Ubuntu you can install: `libncurses5 libsdl2-dev libsdl2-image-2.0-0 libsdl2-ttf-2.0-0`.

On Arch you can install: `ncurses sdl2 sdl2_image sdl2_ttf`.

### Windows

After cloning but before building you must download the [SDL], [SDL_ttf], and [SDL_image]
"Development Libraries" and place them in the `SDL`, `TTF`, and `IMG` directories, respectively.

[SDL]: https://www.libsdl.org/download-2.0.php
[SDL_ttf]: https://www.libsdl.org/projects/SDL_ttf/
[SDL_image]: https://www.libsdl.org/projects/SDL_image/

You will also need to install [ImageMagick] as it is used to generate Tesh's icons.

[ImageMagick]: https://imagemagick.org/script/download.php

Next, to build the project you can either use the Visual Studio gui or the Windows build script.

* To use the Visual Studio gui, open the project as a
  folder in Visual Studio and click Build -> Build All.
* To use CMake, run the PowerShell build script: `.\build-release.ps1`.  But you must set
  `$env:VCINSTALLDIR` to the path of the `VC` directory (for example `$env:VCINSTALLDIR =
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC"`).

After building, Tesh can be ran via `.\build\release\tesh.exe`.






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
