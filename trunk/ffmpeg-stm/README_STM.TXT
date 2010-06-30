These files are updated versions of ffmpeg files as checked out from the git repository on Mon Feb 8 14:46:38 2010 +1100 (commit 211ff5d314bf18e9e135c27cd3d11f0a1f480245).

This repository must then be integrated with the GSoC version of libavfilter (as per the SoC libavfilter instructions).

ffmpeg-servetome links against libmp3lame, libxml2, libx264, libz, libbz2, libiconv, libfontconfig, libfreetype, libfribidi and libass. libexpat

ffmpeg-stm.c replaces ffmpeg.c

STATIC CONFIG
./configure --disable-shared --enable-static --enable-gpl --enable-version3 --enable-nonfree --enable-postproc --enable-pthreads --enable-libfaac --enable-libfaad --enable-libmp3lame --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libtheora --enable-libx264 --enable-encoder=mjpeg --enable-avfilter-lavf --enable-bzlib --enable-avfilter --extra-libs=/usr/lib/libfribidi.a --extra-libs=/usr/lib/libass.a --extra-libs=/usr/lib/libfontconfig.a --extra-libs=/usr/lib/libfreetype.a --extra-libs=/usr/lib/libexpat.a --extra-libs=/usr/lib/libfaac.a --extra-libs=/usr/lib/libfaad.a --enable-runtime-cpudetect --arch=x86 --cpu=pentium-m --disable-debug --enable-muxer=mpegts --enable-muxer=image2 --enable-muxer=image2pipe --extra-cflags=--static --extra-libs='-static -L/usr/lib' --extra-libs=~/Source/libenca.a


