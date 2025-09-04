[![rtsp-to-webrtsp](https://snapcraft.io/rtsp-to-webrtsp/badge.svg)](https://snapcraft.io/rtsp-to-webrtsp)

# Media URLs ReStreamer

Intended to help play RTSP streams from IP Cams (and some other URL types) in browsers.

Online demo: http://ipcam.stream:5080

## How to install it as Snap package and try
* Run: `sudo snap install rtsp-to-webrtsp --edge`;
* Open in your browser: `http://your.server.address:5080/`;

## How to edit config file
1. `sudoedit /var/snap/rtsp-to-webrtsp/common/restreamer.conf`;
2. To load updated config it's required to restart Snap: `sudo snap restart rtsp-to-webrtsp`;

## How to configure your own source
1. In config replace `streamers` section with something like
```
streamers: (
  {
    name: "Your source name"
    url: "rtsp://your.ip.cam.address:port/path"
  }
)
```
2. Restart Snap: `sudo snap restart rtsp-to-webrtsp`;

## How to access it behind router/NAT with port forwarding
1. In config file in `webrtc` section uncomment `stun-server` (and maybe replace value with your preferable STUN server), `min-rtp-port` and `rtp-ports-count` (and provide some reasonable values)
```
webrtc: {
  stun-server: "stun://stun.l.google.com:19302"
  min-rtp-port: 6000
  rtp-ports-count: 100
}
```
2. _[optional]_ prevent Coturn from starting (it's useless in that case) by `use-coturn: false` in `agents` section
```
agents: {
  use-coturn: false
}
```   
4. Configure port forwarding on your router for following ports:
  * TCP 5080
  * TCP 5554
  * UDP 6000-6100 (or what you've specified in `webrtc` section)
5. Restart Snap: `sudo snap restart rtsp-to-webrtsp`;
6. Open in your browser http://your.router.ip.address:5080/

## How to use it to access IP Cam not accessible directly (and without any port forwarding)
1. Install it on some VPS/VDS/Dedicated with public IP;
2. _[optional][recommended]_ [Configure TLS](#how-enable-tls-with-lets-encrypt-certificate-highly-recommended) and users in `users` section of config file;
3. In config file replace (or add to) `streamers` section `proxy` streamer for Remote Agent
```
streamers: (
  {
    name: "Proxy for Remote Agent"
    type: "proxy"
    agent-token: "some random and pretty long string"
  }
)
```
4. Restart Snap: `sudo snap restart rtsp-to-webrtsp`;
5. Install app Snap package on some device on network where IP Cam is accessible directly and configure it [as Remote Agent](https://github.com/WebRTSP/RecordStreamer#how-to-configure-it-as-remote-agent) ;
6. Open in your browser https://your.server.address:5443/ if you have TLS enabled and http://your.server.address:5080/ if not;

## How to configure it as Remote Agent
1. Install it on some device (you can use something like Raspberry Pi);
2. Add `signalling-server` section with the same properties you've defined on the server
```
signalling-server: {
  host: "your.server.address"
  tls: true // or `false` if you didn't configure TLS on server
  uri: "Proxy for Remote Agent"
  token: "some random and pretty long string"
}
```
3. [Configure streamer](#how-to-configure-your-own-source) for IP Cam you want access to ;
4. Restart Snap: `sudo snap restart rtsp-to-webrtsp`;

## How to use it as Cloud NVR for IP Cam not accessible directly
1. In config file replace `streamers` section with something like
```
streamers: (
  {
    restream: false
    name: "NVR"
    type: "record"
    record-token: "some-random-string"
    recordings-dir: "recordings" // path relative to %SNAP_COMMON% (/var/snap/rtsp-to-webrtsp/common/) where recordings will be placed
    recordings-dir-max-size: 1024
    recording-chunk-size: 10
  }
)
```
2. Restart Snap: `sudo snap restart rtsp-to-webrtsp`;
3. Install [webrtsp-record-streamer](https://github.com/WebRTSP/RecordStreamer#how-to-install-it-as-snap-package) Snap package on some device on network where IP Cam is accessible directly;
4. Configure `webrtsp-record-streamer` as described [here](https://github.com/WebRTSP/RecordStreamer#how-to-use-it-as-streamer-for-cloud-nvr-with-motion-detection);
5. Finally with above config recordigns will be available in `/var/snap/rtsp-to-webrtsp/common/recordings/NVR/`;

## How enable TLS with Let's Encrypt certificate (highly recommended)
* Run: `./enableTLS.sh root@your.server.address:22 you@gmail.com`;
* Open in your browser: `https://your.server.address:5443/`;

## Troubleshooting
* To see application logs in realtime run: `sudo snap logs rtsp-to-webrtsp -f`;
