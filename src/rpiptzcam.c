#include "config.h"

#include <string.h>
#include <math.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gst/gst.h>

#include <mosquitto.h>

typedef struct _RpiImagePipe RpiImagePipe;
struct _RpiImagePipe {
	GstElement *pipe;
	GstElement *src;
	GstElement *queue;
	GstElement *capsfilter;

	GstElement *encoder;
	GstElement *capsencoder;

	GstElement *parser;
	GstElement *metadata;
	GstElement *tee;
	GstElement *tee_queue_1;
	GstElement *filesink;

	GstElement *progress;
	GstElement *rtp_pay;

	GstElement *decodebin;

	GstElement *tee_queue_2;
	GstElement *videosink;

	GstElement *tee_queue_3;
	GstElement *hlssink;

	GstElement *tee_queue_4;
	GstElement *flvmux;
	GstElement *rtmpsink;

	gint width;
	gint height;
	gint fps;

	guint bitrate;

	gchar *udphost;
	guint udpport;
};

typedef struct _CameraPTZ CameraPTZ;
struct _CameraPTZ {
	float zoom;
	float x;
	float y;
	float rx;
	float ry;
	float h;
	float w;
	guint zoom_id;
	guint zoom_to;
	guint zoom_from;
};

typedef struct _VideoMQTT VideoMQTT;
struct _VideoMQTT {
	struct mosquitto *tt;
	gchar *host;
	int port;
	gchar *clientid;

	gchar *topic_prefix;

	gchar *ca;
	gchar *crt;
	gchar *key;

	GSource *src_in;
	GSource *src_out;
};

VideoMQTT mq;
RpiImagePipe rpi;
CameraPTZ ptz;
GMainLoop *loop;
gint iso=100;

gboolean record=false;
gboolean stream=false;
gboolean use_h264=false;
gboolean use_mjpg=false;
gboolean use_xv=false;
gboolean use_kms=false;
gchar *hls;
gchar *rtmp;

int keepalive = 120;
bool clean_session = true;

static gboolean
set_tag(GstElement *gstjpegenc, gpointer data)
{
RpiImagePipe *gs=(RpiImagePipe *)data;
GstEvent *te;

return TRUE;
}

static gboolean generate_anotate(gpointer data)
{
set_tag(rpi.metadata, &rpi);

return TRUE;
}

static gboolean
rpiimagepipe(bool h264, bool mjpg)
{
char caps[256];
rpi.pipe=gst_pipeline_new("pipeline");

// Source
rpi.src=gst_element_factory_make("libcamerasrc", "video");
if (!rpi.src) {
	g_print("Failed to create libcamerasrc, fallback to v4l interface.\n");
	rpi.src=gst_element_factory_make("v4l2src", "video");
} else {
	g_object_set(rpi.src, "af-mode", 2, NULL);
}
rpi.queue=gst_element_factory_make("queue", "queue");

// Filtering/Caps
rpi.capsfilter=gst_element_factory_make("capsfilter", "capsfilter");

int bitrate=7500000;

// Encoding
if (h264 && !mjpg) {
	g_print("Capturing h264, %d x %d %d fps.\n", rpi.width,rpi.height, rpi.fps);
	snprintf(caps, sizeof(caps), "video/x-raw,width=%d,height=%d,framerate=%d/1,format=NV21,interlace-mode=progressive,colorimetry=bt709", rpi.width, rpi.height, rpi.fps);

	GstCaps *cr=gst_caps_from_string(caps);
	g_object_set(rpi.capsfilter, "caps", cr, NULL);
	gst_caps_unref(cr);

	rpi.encoder=gst_element_factory_make("v4l2h264enc", "encoder");
//	g_object_set(rpi.encoder, "extra-controls", "controls,video_bitrate_mode=0,sequence_header_mode=1,repeat_sequence_header=1,h264_minimum_qp_value=22,h264_maximum_qp_value=32,h264_i_frame_period=30,h264_profile=4,h264_level=15;", NULL);

	rpi.capsencoder=gst_element_factory_make("capsfilter", "capsencoder");
	snprintf(caps, sizeof(caps), "video/x-h264,profile=high,bitrate=%d,level=(string)4", bitrate);

	GstCaps *cre=gst_caps_from_string(caps);
	g_object_set(rpi.capsencoder, "caps", cre, NULL);
	gst_caps_unref(cre);

	rpi.parser=gst_element_factory_make("h264parse", "h264");
	g_object_set(rpi.parser, "config-interval", 1, NULL);

	g_print("Capture configured\n");
} else if (mjpg && !h264) {
	g_print("Capturing mjpg, %d x %d %d fps.\n", rpi.width,rpi.height, rpi.fps);
	snprintf(caps, sizeof(caps), "image/jpeg,width=%d,height=%d,framerate=%d/1", rpi.width,rpi.height, rpi.fps);

	GstCaps *cr=gst_caps_from_string(caps);
	g_object_set(rpi.capsfilter, "caps", cr, NULL);
	gst_caps_unref(cr);

	rpi.parser=gst_element_factory_make("jpegparse", "jpeg");
	g_print("Capture configured\n");
} else {
	g_print("No format set, (--h264 or --mjpeg)\n");
	return false;
}

rpi.metadata=gst_element_factory_make("matroskamux", "mux");

rpi.progress=gst_element_factory_make("progressreport", "progress");

// Tee + queues
rpi.tee=gst_element_factory_make("tee", "tee");
rpi.tee_queue_1=gst_element_factory_make("queue", "queue1");
rpi.tee_queue_2=gst_element_factory_make("queue", "queue2");
rpi.tee_queue_3=gst_element_factory_make("queue", "queue3");
rpi.tee_queue_4=gst_element_factory_make("queue", "queue4");

// Record to local file
if (record) {
	g_print("Recording to local file.\n");

	rpi.filesink=gst_element_factory_make("filesink", "filesink");
	g_object_set(rpi.filesink, "location", "video.mkv", NULL);
} else {
	if (!use_xv && !use_kms) {
		rpi.filesink=gst_element_factory_make("fakesink", "fakefilesink");
	} else if (use_kms) {
		rpi.filesink=gst_element_factory_make("kmsink", "fakefilesink");
	} else if (use_xv) {
		rpi.filesink=gst_element_factory_make("xvimagesink", "fakefilesink");
	} else {
		g_print("Huh?\n");
		rpi.filesink=gst_element_factory_make("fakesink", "fakefilesink");
	}
}
// tcpserversink host=192.168.1.89 recover-policy=keyframe sync-method=latest-keyframe
//rpi.videosink=gst_element_factory_make("tcpserversink", "tcpsink");
//rtph264pay pt=96 config-interval=1 ! udpsink host=192.168.1.149 port=5000
//rpi.videosink=gst_element_factory_make("fakesink", "tcpsink");

g_object_set(rpi.tee_queue_2, "leaky", 2, NULL);
g_object_set(rpi.tee_queue_3, "leaky", 2, NULL);
g_object_set(rpi.tee_queue_4, "leaky", 2, NULL);

gst_bin_add_many(GST_BIN(rpi.pipe),
	rpi.src,
	rpi.encoder,
	rpi.capsencoder,
	rpi.queue,
	rpi.capsfilter,
	rpi.tee,
	rpi.parser,
	rpi.metadata,
	rpi.filesink,
	rpi.progress,
	rpi.tee_queue_1,
	rpi.tee_queue_2,
	rpi.tee_queue_3,
	rpi.tee_queue_4,
	NULL);

// UDPsink to host
if (stream && h264) {
	g_print("UDP h264 rtp to: %s:%d\n", rpi.udphost, rpi.udpport);

	rpi.rtp_pay=gst_element_factory_make("rtph264pay", "rtpay");
	g_object_set(rpi.rtp_pay, "pt", 96, "config-interval", 1, NULL);

	rpi.videosink=gst_element_factory_make("udpsink", "streamsink");
	g_object_set(rpi.videosink, "host", rpi.udphost, "port", rpi.udpport, NULL);

	gst_bin_add_many(GST_BIN(rpi.pipe), rpi.rtp_pay, rpi.videosink, NULL);
} else if (stream && mjpg) {
	g_print("UDP mjpg rtp to: %s:%d\n", rpi.udphost, rpi.udpport);

	rpi.rtp_pay=gst_element_factory_make("rtpjpegpay", "rtpay");
	g_object_set(rpi.rtp_pay, "pt", 26, "config-interval", 1, NULL);

	rpi.videosink=gst_element_factory_make("udpsink", "streamsink");
	g_object_set(rpi.videosink, "host", rpi.udphost, "port", rpi.udpport, NULL);

	gst_bin_add_many(GST_BIN(rpi.pipe), rpi.rtp_pay, rpi.videosink, NULL);
}

// HLS
if (hls && h264) {
	g_print("HLS to: %s\n", hls);
	rpi.hlssink=gst_element_factory_make("hlssink", "hlssink");
	g_object_set(rpi.hlssink, "location", hls, "max-files", 20, "target-duration", 60, NULL);

	g_object_set(rpi.tee_queue_3, "min-threshold-buffers", 5, NULL);
	gst_element_link_many(rpi.tee, rpi.tee_queue_3, rpi.hlssink, NULL);

	gst_bin_add(GST_BIN(rpi.pipe), rpi.hlssink);
}

if (rtmp && h264) {
	g_print("RTMP to: %s\n", rtmp);

	rpi.flvmux=gst_element_factory_make("flvmux", "flvmux");
	g_object_set(rpi.flvmux, "streamable", true, NULL);

	rpi.rtmpsink=gst_element_factory_make("rtmpsink", "rtmpsink");
	g_object_set(rpi.rtmpsink, "location", rtmp, NULL);

	g_object_set(rpi.tee_queue_4, "min-threshold-buffers", 5, NULL);
	gst_element_link_many(rpi.tee, rpi.tee_queue_4, rpi.flvmux, rpi.rtmpsink, NULL);

	gst_bin_add_many(GST_BIN(rpi.pipe), rpi.flvmux, rpi.rtmpsink, NULL);
}

if (rpi.encoder) {
	gst_element_link_many(rpi.src, rpi.capsfilter, rpi.encoder, rpi.capsencoder, rpi.queue, rpi.parser, rpi.progress, rpi.tee, NULL);
} else {
	gst_element_link_many(rpi.src, rpi.capsfilter, rpi.queue, rpi.parser, rpi.progress, rpi.tee, NULL);
}

if (record) {
	gst_element_link_many(rpi.tee, rpi.tee_queue_1, rpi.metadata, rpi.filesink, NULL);
} else if (!use_xv) {
	gst_element_link_many(rpi.tee, rpi.tee_queue_1, rpi.filesink, NULL);
} else if (use_xv) {
	rpi.decodebin=gst_element_factory_make("decodebin", "decodebin");
	gst_bin_add_many(GST_BIN(rpi.pipe), rpi.decodebin, NULL);
	gst_element_link_many(rpi.tee, rpi.tee_queue_1, rpi.decodebin, rpi.filesink, NULL);
}

if (stream) {
	gst_element_link_many(rpi.tee, rpi.tee_queue_2, rpi.rtp_pay, rpi.videosink, NULL);
}

// Setup
//g_object_set(rpi.src, "num-buffers", 25, NULL);
// annotation-mode=12 sensor-mode=2 exposure-mode=2 drc=3
//g_object_set(rpi.videosink, "recover-policy", 1, "sync-method", 3, NULL);

return true;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
switch (GST_MESSAGE_TYPE (msg)) {
	case GST_MESSAGE_EOS:
		g_print ("End of stream\n");
		mqtt_publish_info_topic_int(mq.tt, "video", "streaming", 0);
		g_main_loop_quit(loop);
	break;
	case GST_MESSAGE_BUFFERING: {
		int buffering;

		gst_message_parse_buffering(msg, &buffering);
		g_print ("Buffering: %d\n", buffering);
	}
	break;
	case GST_MESSAGE_STATE_CHANGED:
		g_print ("State\n");
	break;
	case GST_MESSAGE_INFO:
		g_print ("Info\n");
	break;
	case GST_MESSAGE_STREAM_STATUS: {
		GstStreamStatusType type;
		GstElement *owner;

		gst_message_parse_stream_status(msg, &type, &owner);
		g_print ("Stream status %d\n", type);
	}
	break;
	case GST_MESSAGE_STREAM_START:
		g_print ("Stream started\n");
		mqtt_publish_info_topic_int(mq.tt, "video", "streaming", 1);
	break;
	case GST_MESSAGE_WARNING:
	case GST_MESSAGE_ERROR: {
		gchar  *debug;
		GError *error;

		gst_message_parse_error(msg, &error, &debug);
		g_free (debug);

		g_printerr("Error: %s\n", error->message);
		g_error_free(error);
		g_main_loop_quit(loop);
	}
	break;
	default:
		g_print("Unhandled message %d\n", GST_MESSAGE_TYPE (msg));
	break;
}

return TRUE;
}

gboolean on_sigint(gpointer data)
{
g_return_val_if_fail(data, FALSE);

g_print ("SIGINT\n");

RpiImagePipe *g=(RpiImagePipe *)data;

gst_element_send_event(g->pipe, gst_event_new_eos());

g_print ("EOS Sent\n");

return FALSE;
}

int mqtt_publish_info_topic_int(struct mosquitto *mqtt, const char *prefix, const char *topic, int value)
{
int r;
char ftopic[80];
char data[256];

snprintf(ftopic, sizeof(ftopic), "/%s/%s", prefix, topic);
snprintf(data, sizeof(data), "%d", value);

r=mosquitto_publish(mqtt, NULL, ftopic, strlen(data), data, 0, false);
if (r!=MOSQ_ERR_SUCCESS)
	fprintf(stderr, "MQTT Publish for info [%s] failed with %s\n", topic, mosquitto_strerror(r));

return r;
}

static void mqtt_log_callback(struct mosquitto *m, void *userdata, int level, const char *str)
{
fprintf(stderr, "[MQTT-%d] %s\n", level, str);
}

void set_zoom(int zoom)
{
float s=(float)(CLAMP(zoom, 1, 1000))/1000.0; // 0-1000
float hw=1,xy=0;
float px,py=0;
float sw=4608, sh=2592;
int ix,iy,ih,iw;

if (zoom<0) {
 ptz.zoom=1;
 ptz.rx=0;
 ptz.ry=0;
 ptz.w=sw;
 ptz.h=sh;
} else {
 ptz.zoom=s;
 ptz.w=sw*s;
 ptz.h=sh*s;
 ptz.rx=(sw-ptz.w)/2.0;
 ptz.ry=(sh-ptz.h)/2.0;
}

fprintf(stderr, "Zoom: %d S: %f XY: %f SensXY-WH %f,%f-%f,%f\n", zoom, s, xy, ptz.rx, ptz.ry, ptz.w, ptz.h);

ix=(int)ptz.rx;
iy=(int)ptz.ry;
iw=(int)ptz.w;
ih=(int)ptz.h;

GValue x = G_VALUE_INIT;
g_value_init(&x, G_TYPE_INT);
g_value_set_int(&x, ix);

GValue y = G_VALUE_INIT;
g_value_init(&y, G_TYPE_INT);
g_value_set_int(&y, iy);

GValue w = G_VALUE_INIT;
g_value_init(&w, G_TYPE_INT);
g_value_set_int(&w, iw);

GValue h = G_VALUE_INIT;
g_value_init(&h, G_TYPE_INT);
g_value_set_int(&h, ih);

// Create the GstValueArray
GValue roi = G_VALUE_INIT;
g_value_init(&roi, GST_TYPE_ARRAY);
gst_value_array_append_value(&roi, &x);
gst_value_array_append_value(&roi, &y);
gst_value_array_append_value(&roi, &w);
gst_value_array_append_value(&roi, &h);

g_object_set_property (G_OBJECT(rpi.src), "scaler-crop", &roi);
g_value_unset(&roi);
}

void set_panning(int x, int y)
{
float px,py;
float p=1.0-ptz.h;

ptz.x=CLAMP((float)x/100.0, -1.0, 1.0);
ptz.y=CLAMP((float)y/100.0, -1.0, 1.0);

fprintf(stderr, "PAN %f %f\n", ptz.x, ptz.y);

px=CLAMP((ptz.rx+ptz.x*p), 0, ptz.w);
py=CLAMP((ptz.ry+ptz.y*p), 0, ptz.h);

fprintf(stderr, "PAN %f %f\n", px, py);

g_object_set(rpi.src, "roi-x", px, NULL);
g_object_set(rpi.src, "roi-y", py, NULL);
}

static gboolean zoom_auto(gpointer user_data)
{
if (ptz.zoom_from==ptz.zoom_to) {
	ptz.zoom_id=0;
  	g_print("AutoZoom: Done\n");
	return FALSE;
}
if (ptz.zoom_from>ptz.zoom_to)
	ptz.zoom_from--;
else if (ptz.zoom_from<ptz.zoom_to)
	ptz.zoom_from++;

set_zoom(ptz.zoom_from);

return TRUE;
}

void on_message(struct mosquitto *m, void *userdata, const struct mosquitto_message *msg)
{
char data[256];

fprintf(stderr, "[MSG] %s\n", msg->topic);
fwrite(msg->payload, 1, msg->payloadlen, stdout);

if (msg->payloadlen>250)
 return;

memcpy(data, msg->payload, msg->payloadlen);
data[msg->payloadlen]=NULL;

if (strstr(msg->topic, "/drc")!=NULL) {
  int tmp=atoi(data);
  g_object_set(rpi.src, "drc", tmp, NULL);
} else if (strstr(msg->topic, "/exposure-mode")!=NULL) {
  int tmp=atoi(data);
  g_object_set(rpi.src, "exposure-mode", tmp, NULL);
} else if (strstr(msg->topic, "/annotation-mode")!=NULL) {
  int tmp=atoi(data);
  g_object_set(rpi.src, "annotation-mode", tmp, NULL);
} else if (strstr(msg->topic, "/annotation-text")!=NULL) {
  g_object_set(rpi.src, "annotation-text", data, NULL);
} else if (strstr(msg->topic, "/iso")!=NULL) {
  int tmp=atoi(data);
  tmp=CLAMP(tmp, 100, 3600);
  g_object_set(rpi.src, "iso", tmp, NULL);
} else if (strstr(msg->topic, "/brightness")!=NULL) {
  double tmp=atof(data);
  tmp=CLAMP(tmp, -100.0, 100.0);
  g_object_set(rpi.src, "brightness", tmp/100.0, NULL);
} else if (strstr(msg->topic, "/contrast")!=NULL) {
  double tmp=atof(data);
  tmp=CLAMP(tmp, -100.0, 100.0);
  g_object_set(rpi.src, "contrast", tmp/100.0, NULL);
} else if (strstr(msg->topic, "/saturation")!=NULL) {
  double tmp=atof(data);
  tmp=CLAMP(tmp, -100.0, 100.0);
  g_object_set(rpi.src, "saturation", tmp/100.0, NULL);
} else if (strstr(msg->topic, "/sharpness")!=NULL) {
  double tmp=atof(data);
  tmp=CLAMP(tmp, -100.0, 100.0);
  g_object_set(rpi.src, "sharpness", tmp/100.0, NULL);
} else if (strstr(msg->topic, "/xy")!=NULL) {
  int x,y, r;

  r=sscanf(data, "%d,%d", &x, &y);
  if (r==2) {
   set_panning(x,y);
  } else {
   g_print("Invalid xy format\n");
  }
} else if (strstr(msg->topic, "/zoom/auto")!=NULL) {
  int tmp=atoi(data);
  if (tmp==0) {
	g_source_remove(ptz.zoom_id);
	ptz.zoom_id=0;
	ptz.zoom_to=0;
	ptz.zoom_from=ptz.zoom*1000.0;
  } else {
	ptz.zoom_from=ptz.zoom*1000.0;
	ptz.zoom_to=CLAMP(tmp, 0, 1000);
	if (ptz.zoom_id>0)
		g_source_remove(ptz.zoom_id);
	ptz.zoom_id=g_timeout_add(33, zoom_auto, NULL);
  	g_print("AutoZoom: %d -> %d\n", ptz.zoom_from, ptz.zoom_to);
  }
} else if (strstr(msg->topic, "/zoom")!=NULL) {
  int tmp=atoi(data);
  set_zoom(tmp);
} else if (strstr(msg->topic, "/focus/mode")!=NULL) {
  int tmp=atoi(data);
  g_object_set(rpi.src, "af-mode", CLAMP(tmp, 0, 2), NULL);
} else if (strstr(msg->topic, "/focus/range")!=NULL) {
  int tmp=atoi(data);
  g_object_set(rpi.src, "af-range", CLAMP(tmp, 0, 2), NULL);
} else if (strstr(msg->topic, "/focus/speed")!=NULL) {
  int tmp=atoi(data);
  g_object_set(rpi.src, "af-speed", CLAMP(tmp, 0, 1), NULL);
} else if (strstr(msg->topic, "/stop")!=NULL) {
  gst_element_send_event(rpi.src, gst_event_new_eos());
} else {
   g_print("Unhandled topic\n");
}
}

static gboolean pipeline_update(gpointer user_data)
{
float focus;
int tmp;

g_object_get(rpi.src, "lens-position", &focus, NULL);

// g_print("Focus: %f\n", focus);

tmp=(int)(focus*10000.0);

mqtt_publish_info_topic_int(mq.tt, "video", "focus/position", tmp);

return TRUE;
}

gint connect_mqtt()
{
int ct=0,cttr=10;
while (mosquitto_connect(mq.tt, mq.host, mq.port, keepalive)) {
	fprintf(stderr, "Unable to connect, retrying in 10 seconds.\n");
	ct++;
	if (ct>cttr) {
		fprintf(stderr, "Failed to connect in %d tries, giving up.\n", ct);
		return -1;
	}
	sleep(10);
}
return 0;
}

gboolean mqtt_loop_sleep(gpointer data)
{
int r;
r=mosquitto_loop(mq.tt, 1, 1);
if (r!=MOSQ_ERR_SUCCESS)
	return TRUE;
switch (r) {
	case MOSQ_ERR_CONN_LOST:
		// Try to reconnect
		mosquitto_reconnect(mq.tt);
	break;
	case MOSQ_ERR_NO_CONN:
		mosquitto_reconnect(mq.tt);
	break;
	default:
		// Other failure
	break;
	}

return TRUE;
}

static gboolean _mqtt_socket_out(gint fd, GIOCondition condition, gpointer user_data)
{
mosquitto_loop_write(mq.tt, 1);

g_source_destroy(mq.src_out);
// g_source_unref(mq.src_out);

return FALSE;
}

static gboolean _mqtt_socket_in(gint fd, GIOCondition condition, gpointer user_data)
{
int r=mosquitto_loop_read(mq.tt, 1);

if (r == MOSQ_ERR_CONN_LOST) {
	mosquitto_reconnect(mq.tt);
}

if (mosquitto_want_write(mq.tt)) {
	int fd=mosquitto_socket(mq.tt);

	mq.src_out=g_unix_fd_source_new(fd, G_IO_OUT);
	g_source_set_callback(mq.src_out, (GSourceFunc)_mqtt_socket_out, loop, NULL);
	g_source_attach(mq.src_out, NULL);
}

return TRUE;
}

static gboolean _mqtt_socket_misc(gpointer user_data)
{
mosquitto_loop_misc(mq.tt);

#if 0
if (mosquitto_want_write(mq.tt)) {
	mosquitto_loop_write(mq.tt, 1);
}
#endif

return TRUE;
}

static GOptionEntry entries[] =
{
  { "mqtthost", 'q', 0, G_OPTION_ARG_STRING, &mq.host, "MQTT Host", "host" },
  { "mqttclient", 'c', 0, G_OPTION_ARG_STRING, &mq.clientid, "MQTT Client ID", "client" },
  { "mqttport", 'p', 0, G_OPTION_ARG_INT, &mq.port, "MQTT port", "port" },

  { "width", 'w', 0, G_OPTION_ARG_INT, &rpi.width, "Video width", "width" },
  { "height", 'h', 0, G_OPTION_ARG_INT, &rpi.height, "Video height", "height" },
  { "fps", 'f', 0, G_OPTION_ARG_INT, &rpi.fps, "Video FPS", "fps" },

  { "bitrate", 'b', 0, G_OPTION_ARG_INT, &rpi.bitrate, "Video bitrate", "bitrate" },

  { "record", 0, 0, G_OPTION_ARG_NONE, &record, "Record localy", NULL },

  { "xvsink", 0, 0, G_OPTION_ARG_NONE, &use_xv, "Use xvimagesink instead of faksesink", NULL },

  { "h264", 0, 0, G_OPTION_ARG_NONE, &use_h264, "Use h264, default", NULL },
  { "mjpeg", 0, 0, G_OPTION_ARG_NONE, &use_mjpg, "Use MJPG", NULL },

  { "stream", 0, 0, G_OPTION_ARG_NONE, &stream, "Stream", NULL },
  { "strhost", 0, 0, G_OPTION_ARG_STRING, &rpi.udphost, "UDP Sink host", NULL },
  { "strport", 0, 0, G_OPTION_ARG_INT, &rpi.udpport, "UDP Sink host", NULL },

  { "hls", 0, 0, G_OPTION_ARG_STRING, &hls, "Stream HLS to given path", NULL },
  { "rtmp", 0, 0, G_OPTION_ARG_STRING, &rtmp, "Stream to RTMP to given url", NULL },

  { NULL }
};

gint main(gint argc, gchar **argv)
{
GstBus *bus;
int bus_watch_id;
GError *error = NULL;
GOptionContext *context;

mq.host="localhost";
mq.port=1883;
mq.clientid="ta-rpivideo";

rpi.width=1920;
rpi.height=1080;
rpi.fps=30;
rpi.bitrate=8000000;

rpi.udphost="localhost";
rpi.udpport=5000;

ptz.zoom=1.0;
ptz.x=0.0;
ptz.y=0.0;
ptz.rx=0.0;
ptz.ry=0.0;
ptz.h=1.0;
ptz.w=1.0;

ptz.zoom_id=0;
ptz.zoom_from=1000;
ptz.zoom_to=1000;

gst_init(&argc, &argv);

context = g_option_context_new ("- test tree model performance");
g_option_context_add_main_entries (context, entries, NULL);
//g_option_context_add_group (context, gtk_get_option_group (TRUE));

if (!g_option_context_parse (context, &argc, &argv, &error)) {
	g_print ("option parsing failed: %s\n", error->message);
	exit (1);
}

if (rpiimagepipe(use_h264, use_mjpg)==false) {
 return 1;
}

g_print("MQTT: %s %d %s\n", mq.host, mq.port, mq.clientid);

mosquitto_lib_init();

mq.tt=mosquitto_new(mq.clientid, clean_session, NULL);
mosquitto_log_callback_set(mq.tt, mqtt_log_callback);
mosquitto_message_callback_set(mq.tt, on_message);

connect_mqtt();

mosquitto_subscribe(mq.tt, NULL, "/video/iso", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/saturation", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/contrast", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/brightness", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/sharpness", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/drc", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/exposure-mode", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/annotation-mode", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/annotation-text", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/roi", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/zoom", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/zoom/auto", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/xy", 0);

mosquitto_subscribe(mq.tt, NULL, "/video/focus/mode", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/focus/range", 0);
mosquitto_subscribe(mq.tt, NULL, "/video/focus/speed", 0);

mosquitto_subscribe(mq.tt, NULL, "/video/control/stop", 0);

bus = gst_pipeline_get_bus(GST_PIPELINE(rpi.pipe));
bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
gst_object_unref(bus);

g_timeout_add(200, generate_anotate, NULL);

g_unix_signal_add(SIGINT, on_sigint, &rpi.pipe);

mqtt_publish_info_topic_int(mq.tt, "video", "streaming", 0);

gst_element_set_state(rpi.pipe, GST_STATE_PLAYING);

loop=g_main_loop_new(NULL, TRUE);

int fd=mosquitto_socket(mq.tt);

mq.src_in=g_unix_fd_source_new(fd, G_IO_IN);
g_source_set_callback(mq.src_in, (GSourceFunc)_mqtt_socket_in, loop, NULL);
g_source_attach(mq.src_in, NULL);

//mq.src_out=g_unix_fd_source_new(fd, G_IO_OUT);
//g_source_set_callback(mq.src_out, (GSourceFunc)_mqtt_socket_out, loop, NULL);
//g_source_attach(mq.src_out, NULL);

g_timeout_add(100, _mqtt_socket_misc, loop);
g_timeout_add(1000, pipeline_update, loop);

g_main_loop_run(loop);

g_main_loop_unref(loop);

g_source_destroy(mq.src_in);

gst_object_unref(bus);
gst_element_set_state(rpi.pipe, GST_STATE_NULL);
gst_object_unref(rpi.pipe);

mosquitto_lib_cleanup();

return 0;
}
