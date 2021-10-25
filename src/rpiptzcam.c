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
	GstElement *imageenc;
	GstElement *metadata;
	GstElement *tee;
	GstElement *tee_queue_1;
	GstElement *tee_queue_2;
	GstElement *filesink;
	GstElement *progress;
	GstElement *rtp_pay;
	GstElement *videosink;
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
};

typedef struct _VideoMQTT VideoMQTT;
struct _VideoMQTT {
	struct mosquitto *tt;
	const char *host;
	const char *clientid;
	const char *topic_prefix;
	const char *ca;
	const char *crt;
	const char *key;

	GSource *src_in;
	GSource *src_out;
};

VideoMQTT mq;
RpiImagePipe rpi;
CameraPTZ ptz;
GMainLoop *loop;
gint iso=100;

int port = 1883;
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

// jpegparse ! matroskamux ! queue ! tcpserversink host=192.168.1.89 recover-policy=keyframe sync-method=latest-keyframe

static void
rpiimagepipe(bool h264)
{
rpi.pipe=gst_pipeline_new("pipeline");

// Source
rpi.src=gst_element_factory_make("rpicamsrc", "video");
rpi.queue=gst_element_factory_make("queue", "queue");

// Filtering/Caps
rpi.capsfilter=gst_element_factory_make("capsfilter", "capsfilter");

// Encoding
if (h264) {
	GstCaps *cr=gst_caps_from_string ("video/x-h264,width=1640,height=1232,framerate=5/1");
	g_object_set(rpi.capsfilter, "caps", cr, NULL);
	gst_caps_unref(cr);

	rpi.imageenc=gst_element_factory_make("h264parse", "h264");
	g_object_set(rpi.imageenc, "config-interval", 1, NULL);
} else {
	// Framerate, 1 FPS
	// GstCaps *cr=gst_caps_from_string ("image/jpeg,width=2592,height=1944,framerate=5/1");
	GstCaps *cr=gst_caps_from_string ("image/jpeg,width=1640,height=1232,framerate=5/1");
	g_object_set(rpi.capsfilter, "caps", cr, NULL);
	gst_caps_unref(cr);

	rpi.imageenc=gst_element_factory_make("jpegparse", "jpeg");
}

rpi.metadata=gst_element_factory_make("matroskamux", "mux");

rpi.progress=gst_element_factory_make("progressreport", "progress");

// Tee
rpi.tee=gst_element_factory_make("tee", "tee");
rpi.tee_queue_1=gst_element_factory_make("queue", "queue1");
rpi.tee_queue_2=gst_element_factory_make("queue", "queue2");

// Sink(s)
rpi.filesink=gst_element_factory_make("filesink", "filesink");
// tcpserversink host=192.168.1.89 recover-policy=keyframe sync-method=latest-keyframe
//rpi.videosink=gst_element_factory_make("tcpserversink", "tcpsink");
//rtph264pay pt=96 config-interval=1 ! udpsink host=192.168.1.149 port=5000
//rpi.videosink=gst_element_factory_make("fakesink", "tcpsink");

g_object_set(rpi.tee_queue_2, "leaky", 2, NULL);

rpi.videosink=gst_element_factory_make("udpsink", "streamsink");
g_object_set(rpi.videosink, "host", "192.168.1.149", "port", 5000, NULL);

rpi.rtp_pay=gst_element_factory_make("rtph264pay", "rtpay");
g_object_set(rpi.rtp_pay, "pt", 96, "config-interval", 1, NULL);

gst_bin_add_many(GST_BIN(rpi.pipe), rpi.src, rpi.queue,
	rpi.capsfilter,
	rpi.tee, rpi.imageenc, rpi.metadata,
	rpi.tee_queue_1,
	rpi.tee_queue_2,
	rpi.filesink,
	rpi.progress,
	rpi.rtp_pay,
	rpi.videosink, NULL);

gst_element_link_many(rpi.src, rpi.queue, rpi.capsfilter, rpi.tee, NULL);
gst_element_link_many(rpi.tee, rpi.tee_queue_1, rpi.imageenc, rpi.metadata, rpi.filesink, NULL);
gst_element_link_many(rpi.tee, rpi.tee_queue_2, rpi.rtp_pay, rpi.videosink, NULL);

// Setup
//g_object_set(rpi.src, "num-buffers", 25, NULL);
// annotation-mode=12 sensor-mode=2 exposure-mode=2 drc=3
g_object_set(rpi.src, "sensor-mode", 2, "annotation-mode", 12, "exposure-mode", 2, "keyframe-interval", 5, NULL);
g_object_set(rpi.filesink, "location", "video.mkv", NULL);
//g_object_set(rpi.videosink, "recover-policy", 1, "sync-method", 3, NULL);
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
switch (GST_MESSAGE_TYPE (msg)) {
	case GST_MESSAGE_EOS:
		g_print ("End of stream\n");
		mqtt_publish_info_topic_int(mq.tt, "video", "streaming", 0);
		g_main_loop_quit(loop);
	break;
	case GST_MESSAGE_BUFFERING:
		g_print ("Buffering\n");
	break;
	case GST_MESSAGE_STATE_CHANGED:
		g_print ("State\n");
	break;
	case GST_MESSAGE_INFO:
		g_print ("Info\n");
	break;
	case GST_MESSAGE_STREAM_STATUS:
		g_print ("Stream\n");
	break;
	case GST_MESSAGE_STREAM_START:
		g_print ("Stream started\n");
		mqtt_publish_info_topic_int(mq.tt, "video", "streaming", 1);
	break;
	case GST_MESSAGE_WARNING:
	case GST_MESSAGE_ERROR: {
		gchar  *debug;
		GError *error;

		gst_message_parse_error (msg, &error, &debug);
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
float s=(float)zoom/100.0;
float hw=1,xy=0;
float px,py=0;

if (zoom<0)
  return;

if (zoom>99) {
  hw=1.0/s;
  xy=0.5-(0.5/s);

  fprintf(stderr, "%d S: %f XY: %f HW: %f\n", zoom, s, xy, hw);
} else {
  s=(float)zoom;
}

if (zoom>1) {
  char txt[80];
  snprintf(txt, sizeof(txt), "Zoom: %f.1", s);
  g_object_set(rpi.src, "annotation-mode", 1, "annotation-text", txt, NULL);
} else {
  g_object_set(rpi.src, "annotation-mode", 12, "annotation-text", "", NULL);
}

ptz.zoom=s;
ptz.rx=xy;
ptz.ry=xy;
ptz.h=hw;
ptz.w=hw;

switch(zoom) {
  case 0:
  case 1:
    g_object_set(rpi.src, "roi-x", 0.0, NULL);
    g_object_set(rpi.src, "roi-y", 0.0, NULL);
    g_object_set(rpi.src, "roi-w", 1.0, NULL);
    g_object_set(rpi.src, "roi-h", 1.0, NULL);
  break;
  default:
    g_object_set(rpi.src, "roi-x", xy+ptz.x, NULL);
    g_object_set(rpi.src, "roi-y", xy+ptz.y, NULL);
    g_object_set(rpi.src, "roi-w", hw, NULL);
    g_object_set(rpi.src, "roi-h", hw, NULL);
  break;
}

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
  int tmp=atoi(data);
  tmp=CLAMP(tmp, -100, 100);
  g_object_set(rpi.src, "brightness", tmp, NULL);
} else if (strstr(msg->topic, "/contrast")!=NULL) {
  int tmp=atoi(data);
  tmp=CLAMP(tmp, -100, 100);
  g_object_set(rpi.src, "contrast", tmp, NULL);
} else if (strstr(msg->topic, "/saturation")!=NULL) {
  int tmp=atoi(data);
  tmp=CLAMP(tmp, -100, 100);
  g_object_set(rpi.src, "saturation", tmp, NULL);
} else if (strstr(msg->topic, "/sharpness")!=NULL) {
  int tmp=atoi(data);
  tmp=CLAMP(tmp, -100, 100);
  g_object_set(rpi.src, "sharpness", tmp, NULL);
} else if (strstr(msg->topic, "/xy")!=NULL) {
  int x,y, r;

  r=sscanf(data, "%d,%d", &x, &y);
  if (r==2) {
   set_panning(x,y);
  } else {
   g_print("Invalid xy format\n");
  }
} else if (strstr(msg->topic, "/zoom")!=NULL) {
  int tmp=atoi(data);
  set_zoom(tmp);
} else {
   g_print("Unhandled topic\n");
}
}

gint connect_mqtt()
{
int ct=0,cttr=10;
while (mosquitto_connect(mq.tt, mq.host, port, keepalive)) {
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
int i=0,r;
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

gint main(gint argc, gchar **argv)
{
GstBus *bus;
int bus_watch_id;

gst_init(&argc, &argv);

mq.host="localhost";
mq.clientid="ta-rpivideo";

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
mosquitto_subscribe(mq.tt, NULL, "/video/xy", 0);

ptz.zoom=1.0;
ptz.x=0.0;
ptz.y=0.0;
ptz.rx=0.0;
ptz.ry=0.0;
ptz.h=1.0;
ptz.w=1.0;

rpiimagepipe(true);

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

g_main_loop_run(loop);

g_main_loop_unref(loop);

g_source_destroy(mq.src_in);

gst_object_unref(bus);
gst_element_set_state(rpi.pipe, GST_STATE_NULL);
gst_object_unref(rpi.pipe);

mosquitto_lib_cleanup();

return 0;
}
