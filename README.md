# DDNetHvH Server - FNG

A dedicated server for DDNet (DDraceNetwork) featuring Hacker vs Hacker gameplay in FNG mode.

# Installation

## Clone the repository
```sh
git clone https://github.com/ddnethvh/alivefng
cd alivefng
```

## Clone the libraries (Linux)
```sh
sudo apt install build-essential cargo cmake git glslang-tools google-mock libavcodec-extra libavdevice-dev libavfilter-dev libavformat-dev libavutil-dev libcurl4-openssl-dev libfreetype6-dev libglew-dev libnotify-dev libogg-dev libopus-dev libopusfile-dev libpng-dev libsdl2-dev libsqlite3-dev libssl-dev libvulkan-dev libwavpack-dev libx264-dev python3 rustc spirv-tools
# Or:
sudo apt install build-essential cargo cmake git google-mock libcurl4-openssl-dev libsqlite3-dev libssl-dev python3 rustc
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
./fng2_srv
```