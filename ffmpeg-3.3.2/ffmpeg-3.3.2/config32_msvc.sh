#! /bin/bash

./configure --arch=x86 --enable-gpl --enable-nonfree --enable-version3 --enable-shared --disable-static --enable-debug --enable-cuda --enable-cuvid --enable-nvenc --cpu=native --extra-cflags=-IC:/msys32/1.0/local/include --extra-ldflags=-LIBPATH:C:/msys32/1.0/local/lib --toolchain=msvc

