#! /bin/bash

./configure --arch=x86_64 --enable-gpl --enable-nonfree --enable-version3 --enable-shared --disable-static --enable-debug --enable-cuda --enable-cuvid --enable-nvenc --cpu=native --extra-cflags=-IC:/msys/1.0/local/include --extra-ldflags=-LIBPATH:C:/msys/1.0/local/lib --toolchain=msvc

