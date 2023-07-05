### How to build it from sources and run (Ubuntu 20.10)

1. Install required packages:
```
sudo apt install build-essential git cmake \
    libspdlog-dev libconfig-dev libssl-dev \
    gsoap libgsoap-dev libmicrohttpd-dev libnice-dev \
    libgstreamer1.0-dev libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-nice
```

2. Build and install libwebsockets (at least v4.1 is required):
```
git clone https://github.com/warmcat/libwebsockets.git --branch v4.2-stable --depth 1
mkdir -p libwebsockets-build
cd libwebsockets-build
cmake -DLWS_WITH_GLIB=ON ../libwebsockets
make -j4
sudo make install
cd -
```

3. Build application:
```
git clone https://github.com/WebRTSP/ReStreamer.git --recursive
mkdir -p ReStreamer-build
cd ReStreamer-build
cmake ../ReStreamer
make -j4
cd -
```

4. Prepare config: `cp ReStreamer/restreamer.conf.sample ~/.config/restreamer.conf`
5. [optional] Edit config:`vim ~/.config/restreamer.conf`
6. Run application: `cd ReStreamer && ../ReStreamer-build/ReStreamer`
7. Open in browser `http://localhost:5080/`
