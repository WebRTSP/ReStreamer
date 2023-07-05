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
