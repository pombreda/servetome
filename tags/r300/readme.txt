lSTM
====

Intro
-----
Welcome to Linux ServeToMe (lSTM), a command line based python server for the
iPad/iPhone application StreamToMe. The python script is accompanied by a
custom build of ffmpeg.

The program is configured via an stm.cfg file in the same directory as stm.py
and should be fairly self explanatory. Please make sure the given temp/bin
directories exist before running.

Thanks to Matt Gallagher the author of StreamToMe for his help with the ffmpeg
patching/compile and for answering technical questions on STM operation.

TODO: Password support

Contents
--------
stm.py   - ServeToMe application
stm.cfg  - Example config file
stmd     - Example /etc/init.d script to daemonise things
bin/...  - Binary ffmpeg executables for linux & windows
temp/... - Where stm.py puts all its temp files

Instructions
------------
Unpack into a directory of your choice, move/edit stmd to /etc/init.d if you plan
to run as a daemon, ignore if not. Edit the stm.cfg to suit your config, you
shuold really only need to change:

tempDir=/home/kate/ServeToMe/temp
execDir=/home/kate/ServeToMe/bin
directoryList=/data/media/arrivals,/data/media/movies,data/media/music

These should reflect your chosen locations, then either run "./stm.py" or
"/etc/init.d/stmd start", if you are running as a daemon with the stm init.d
script then all paths must be absolute, relative is OK if you are running on
the command line.

Hope you like it.

Kate
