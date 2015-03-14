

# Introduction #

Python port of the StreamToMe Server. [StreamToMe](http://projectswithlove.com/streamtome) is an application that transcodes your video/music library live to your iPad, iPhone and iPod.


# Build Environemnt (Author) #
Ubuntu 10.04 Server using Python 2.6.5

# Build FFMpeg from source (Debian Lenny) #
This is just an step by step guide as I was trying to build svn checkout of FFMpeg-stm. I may have missed some steps since I have build FFMpeg in the past and might have installed some prerequisites already. Please note that Debian Lenny comes with Python 2.4. We need to install Python2.6 experimental to run the stm.py server. Add the testing repositories into your apt sources to install Python 2.6.



Check out from the svn repository...
```
svn checkout https://servetome.googlecode.com/svn/trunk/ servetome
cd servetome
```


We are going to build x264...
```
apt-get install checkinstall yasm git-core
git clone git://git.videolan.org/x264.git
cd x264
./configure
make
sudo checkinstall --pkgname=x264 --pkgversion "1:0.svn`date +%Y%m%d`" --backup=no --default
cd ..
```

Next build theora...
```
sudo apt-get install libogg-dev

wget http://downloads.xiph.org/releases/theora/libtheora-1.1.1.tar.gz
tar xzvf libtheora-1.1.1.tar.gz
cd libtheora-1.1.1
./configure
make
checkinstall --pkgname=libtheora --pkgversion "1.1.1" --backup=no --default
cd ..
```

At last, FFMpeg...
```
cd ffmpeg-stm

wget http://www.archlinux.org/packages/extra/i686/enca/download/

tar -zxvf enca-1.13-1-i686.pkg.tar.gz

sudo apt-get install libfribidi-dev
sudo apt-get install libass-dev
sudo apt-get install libenca-dev
sudo apt-get install libopencore-amrnb-dev
sudo apt-get install libopencore-amrwb-dev 

sudo ./configure --disable-shared --enable-static --enable-gpl --enable-version3 --enable-nonfree --enable-postproc --enable-pthreads --enable-libfaac --enable-libfaad --enable-libmp3lame --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libtheora --enable-libx264 --enable-encoder=mjpeg --enable-avfilter-lavf --enable-bzlib --enable-avfilter --extra-libs=/usr/lib/libfribidi.a --extra-libs=/usr/lib/libass.a --extra-libs=/usr/lib/libfontconfig.a --extra-libs=/usr/lib/libfreetype.a --extra-libs=/usr/lib/libexpat.a --extra-libs=/usr/lib/libfaac.a --extra-libs=/usr/lib/libfaad.a --enable-runtime-cpudetect --arch=x86 --cpu=pentium-m --disable-debug --enable-muxer=mpegts --enable-muxer=image2 --enable-muxer=image2pipe --extra-cflags=--static --extra-libs='-static -L/usr/lib' --extra-libs=./usr/lib/libenca.a

./make
```


Before we run the install for FFMpeg, please note that it will make a copy of "ffmpeg" into /usr/bin. If the file already exist, then it will not overwrite. But just wanted to be sure you understand that if you have not installed FFMpeg before, all future calls to ffmpeg will call this compiled version in /usr/bin that is specifically for StreamToMe (if it was overwritten).

```
sudo checkinstall --pkgname=ffmpeg-stm --pkgversion "4:0.5+svn`date +%Y%m%d`" --backup=no --default
```

Now we are going to copy the binary to the /stm/bin directory...
```
cp ./ffmpeg ../stm/bin/ffmeg-stm
```

Under Lenny, the command to start the server is "python2.6 stm.py"

If you find any error or some step I have missed, please feel free to email me. Enjoy.