#!/bin/bash

# This script is intended to setup a clean Raspberry Pi system for running Jamulus
OPUS="opus-1.1"

echo "TODO: sudo apt-get install [needed libraries for compilation and runtime]"

# Opus audio codec, custom compilation with custom modes and fixed point support
if [ -d "${OPUS}" ]; then
  echo "The Opus directory is present, we assume it is compiled and ready to use. If not, delete the opus directory and call this script again."
else
  wget https://archive.mozilla.org/pub/opus/${OPUS}.tar.gz
  tar -xzf ${OPUS}.tar.gz
  rm ${OPUS}.tar.gz
  cd ${OPUS}
  ./configure --enable-custom-modes --enable-fixed-point
  make
  cd ..
fi

# Jack audio without DBUS support
if [ -d "jack2" ]; then
  echo "The Jack2 directory is present, we assume it is compiled and ready to use. If not, delete the jack2 directory and call this script again."
else
  git clone https://github.com/jackaudio/jack2.git
  cd jack2
  echo "TODO jack2"
  cd ..
fi

# compile Jamulus with external Opus library
cd ..
qmake "CONFIG+=opus_shared_lib" Jamulus.pro
make
cd distributions

# start Jack2 and Jamulus in headless mode
cd ..
LD_LIBRARY_PATH="distributions/${OPUS}/.libs"
export LD_LIBRARY_PATH
echo "TODO: -dhw:MBox"
#jackd -P70 -p16 -t2000 -d alsa -dhw:MBox -p 128 -n 3 -r 48000 -s &
jackd -P70 -p16 -t2000 -d alsa -dhw:MBox -p 256 -n 3 -r 48000 -s &
./Jamulus -n -c jamulus.fischvolk.de

