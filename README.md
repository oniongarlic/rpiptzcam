# rpiptzcam

A simple poor-mans Raspberry Pi digital PTZ camera.

Now supports libcamera using libcamerasrc with gstreamer.

Uses MQTT for remote control.

## Usage:

*  -q, --mqtthost=host         MQTT Host
*  -c, --mqttclient=client     MQTT Client ID
*  -p, --mqttport=port         MQTT port
*  -w, --width=width           Video width
*  -h, --height=height         Video height
*  -f, --fps=fps               Video FPS
*  -b, --bitrate=bitrate       Video bitrate
*  --record                    Record localy
*  --xvsink                    Use xvimagesink instead of faksesink
*  --h264                      Use h264, default
*  --mjpeg                     Use MJPG
*  --stream                    UDP Stream 
*  --strhost                   UDP Sink host
*  --strport                   UDP Sink port
*  --hls                       Stream HLS to given path
*  --rtmp                      Stream to RTMP to given url

## Stream with UDP to 192.168.1.123

./rpiptzcam --h264 --stream --strhost 192.168.1.123

## Listen to UDP stream on 192.168.1.123

gst-launch-1.0 udpsrc port=5000 ! 'application/x-rtp, encoding-name=H264, payload=96' ! rtph264depay ! queue ! h264parse ! avdec_h264 ! videoconvert ! xvimagesink

## Zoom (0-1000)

* mosquitto_pub -t /video/zoom -m 800
* mosquitto_pub -t /video/zoom/auto -m 500
