[![rtsp-to-webrtsp](https://snapcraft.io/rtsp-to-webrtsp/badge.svg)](https://snapcraft.io/rtsp-to-webrtsp)

## Media URLs ReStreamer

Intended to help play RTSP streams from IP Cams (and some other URL types) in browsers.

Online demo: http://ipcam.stream:5080

### How to install it from snap and configure your own source

1. Install application snap package: `sudo snap install rtsp-to-webrtsp --edge`
2. Open config for edit: `sudoedit /var/snap/rtsp-to-webrtsp/common/restreamer.conf`
3. In opened editor replace `streamers` section with something like

```
streamers: (
  {
    name: "Your source name",
    url: "rtsp://your_source_ip_or_dns:port/path"
  }
)
```

4. Restart snap package to use updated config: `sudo snap restart rtsp-to-webrtsp`
5. Open in browser `http://your_server_ip_or_dns:5080/`
6. To see application logs on server in realtime you can run `sudo snap logs rtsp-to-webrtsp -f`

### How to build it from sources and run (Ubuntu 20.10)

1. Install required packages:
```
sudo apt install build-essential git cmake \
    libspdlog-dev libconfig-dev libssl-dev \
    libgstreamer1.0-dev libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-nice
```

2. Build and install libwebsockets (at least v4.1 is required):
```
git clone https://github.com/warmcat/libwebsockets.git --branch v4.1-stable --depth 1
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
