# DDNetHvH Server - FNG

A dedicated server for DDNet (DDraceNetwork) featuring Hacker vs Hacker gameplay in FNG mode.

# Installation

## Clone the repository
```sh
git clone https://github.com/ddnethvh/alivefng
cd ddnet
```

## Clone the libraries (Windows)
```sh
git clone https://github.com/ddnet/ddnet-libs/
```

## Clone the libraries (Linux)
```sh
sudo apt install build-essential cargo cmake git glslang-tools google-mock libavcodec-extra libavdevice-dev libavfilter-dev libavformat-dev libavutil-dev libcurl4-openssl-dev libfreetype6-dev libglew-dev libnotify-dev libogg-dev libopus-dev libopusfile-dev libpng-dev libsdl2-dev libsqlite3-dev libssl-dev libvulkan-dev libwavpack-dev libx264-dev python3 rustc spirv-tools
```

## Build the server
```sh
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## Run the server
```sh
./ddnet-server
```

## Clone the maps
```sh
git clone https://github.com/teemods/opengores-maps
cd opengores-maps
rm -rf maps7 scripts
mv maps ../maps/
```

## Remove the maps (optional)
```sh
ls *.map | shuf | tail -n +50 | xargs -d '\n' rm
```