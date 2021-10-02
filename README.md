# rpiptzcam

A simple poor-mans Raspberry Pi digital PTZ camera.

MQTT controlled.

 mosquitto_pub -t /video/exposure-mode -m 2
 mosquitto_pub -t /video/iso -m 400
 mosquitto_pub -t /video/drc -m 1

 mosquitto_pub -t /video/zoom -m 1
 mosquitto_pub -t /video/zoom -m 2

 mosquitto_pub -t /video/zoom -m 150
 mosquitto_pub -t /video/zoom -m 450
