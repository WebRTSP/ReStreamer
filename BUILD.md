### How to build it from sources and run (Ubuntu 24.04)

1. Install required packages:
```
sudo apt install build-essential git cmake \
    libwebsockets-dev libspdlog-dev libconfig-dev libssl-dev \
    gsoap libgsoap-dev libmicrohttpd-dev libnice-dev \
    libgstreamer1.0-dev libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-nice
```

2. Build application:
```
git clone https://github.com/WebRTSP/ReStreamer.git --recursive
mkdir -p ReStreamer-build
cd ReStreamer-build
cmake ../ReStreamer
make -j4
cd -
```

3. Prepare config: `cp ReStreamer/restreamer.conf.sample ~/.config/restreamer.conf`
4. [optional] Edit config:`vim ~/.config/restreamer.conf`
5. Run application: `cd ReStreamer && ../ReStreamer-build/ReStreamer`
6. Open in browser `http://localhost:5080/`
