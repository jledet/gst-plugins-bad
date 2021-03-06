/*
 * Copyright (C) 2010, 2013 Ole André Vadla Ravnås <oleavr@soundrop.com>
 * Copyright (C) 2013 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vtenc.h"

#include "coremediabuffer.h"
#include "corevideobuffer.h"
#include "vtutil.h"

#define VTENC_DEFAULT_USAGE       6     /* Profile: Baseline  Level: 2.1 */
#define VTENC_DEFAULT_BITRATE     768
#define VTENC_DEFAULT_FRAME_REORDERING TRUE
#define VTENC_DEFAULT_REALTIME FALSE

GST_DEBUG_CATEGORY (gst_vtenc_debug);
#define GST_CAT_DEFAULT (gst_vtenc_debug)

#define GST_VTENC_CODEC_DETAILS_QDATA \
    g_quark_from_static_string ("vtenc-codec-details")

enum
{
  PROP_0,
  PROP_USAGE,
  PROP_BITRATE,
  PROP_ALLOW_FRAME_REORDERING,
  PROP_REALTIME
};

typedef struct _GstVTEncFrame GstVTEncFrame;

struct _GstVTEncFrame
{
  GstBuffer *buf;
  GstVideoFrame videoframe;
};

static GstElementClass *parent_class = NULL;

static void gst_vtenc_get_property (GObject * obj, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_vtenc_set_property (GObject * obj, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static gboolean gst_vtenc_start (GstVideoEncoder * enc);
static gboolean gst_vtenc_stop (GstVideoEncoder * enc);
static gboolean gst_vtenc_set_format (GstVideoEncoder * enc,
    GstVideoCodecState * input_state);
static GstFlowReturn gst_vtenc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_vtenc_finish (GstVideoEncoder * enc);

static void gst_vtenc_clear_cached_caps_downstream (GstVTEnc * self);

static VTCompressionSessionRef gst_vtenc_create_session (GstVTEnc * self);
static void gst_vtenc_destroy_session (GstVTEnc * self,
    VTCompressionSessionRef * session);
static void gst_vtenc_session_dump_properties (GstVTEnc * self,
    VTCompressionSessionRef session);
static void gst_vtenc_session_configure_expected_framerate (GstVTEnc * self,
    VTCompressionSessionRef session, gdouble framerate);
static void gst_vtenc_session_configure_max_keyframe_interval (GstVTEnc * self,
    VTCompressionSessionRef session, gint interval);
static void gst_vtenc_session_configure_max_keyframe_interval_duration
    (GstVTEnc * self, VTCompressionSessionRef session, gdouble duration);
static void gst_vtenc_session_configure_bitrate (GstVTEnc * self,
    VTCompressionSessionRef session, guint bitrate);
static OSStatus gst_vtenc_session_configure_property_int (GstVTEnc * self,
    VTCompressionSessionRef session, CFStringRef name, gint value);
static OSStatus gst_vtenc_session_configure_property_double (GstVTEnc * self,
    VTCompressionSessionRef session, CFStringRef name, gdouble value);
static void gst_vtenc_session_configure_allow_frame_reordering (GstVTEnc * self,
    VTCompressionSessionRef session, gboolean allow_frame_reordering);
static void gst_vtenc_session_configure_realtime (GstVTEnc * self,
    VTCompressionSessionRef session, gboolean realtime);

static GstFlowReturn gst_vtenc_encode_frame (GstVTEnc * self,
    GstVideoCodecFrame * frame);
static void gst_vtenc_enqueue_buffer (void *outputCallbackRefCon,
    void *sourceFrameRefCon, OSStatus status, VTEncodeInfoFlags infoFlags,
    CMSampleBufferRef sampleBuffer);
static gboolean gst_vtenc_buffer_is_keyframe (GstVTEnc * self,
    CMSampleBufferRef sbuf);


#ifndef HAVE_IOS
static GstVTEncFrame *gst_vtenc_frame_new (GstBuffer * buf,
    GstVideoInfo * videoinfo);
static void gst_vtenc_frame_free (GstVTEncFrame * frame);

static void gst_pixel_buffer_release_cb (void *releaseRefCon,
    const void *dataPtr, size_t dataSize, size_t numberOfPlanes,
    const void *planeAddresses[]);
#endif

static GstStaticCaps sink_caps =
GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ NV12, I420 }"));

static void
gst_vtenc_base_init (GstVTEncClass * klass)
{
  const GstVTEncoderDetails *codec_details =
      GST_VTENC_CLASS_GET_CODEC_DETAILS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  const int min_width = 1, max_width = G_MAXINT;
  const int min_height = 1, max_height = G_MAXINT;
  const int min_fps_n = 0, max_fps_n = G_MAXINT;
  const int min_fps_d = 1, max_fps_d = 1;
  GstPadTemplate *sink_template, *src_template;
  GstCaps *src_caps;
  gchar *longname, *description;

  longname = g_strdup_printf ("%s encoder", codec_details->name);
  description = g_strdup_printf ("%s encoder", codec_details->name);

  gst_element_class_set_metadata (element_class, longname,
      "Codec/Encoder/Video", description,
      "Ole André Vadla Ravnås <oleavr@soundrop.com>, Dominik Röttsches <dominik.rottsches@intel.com>");

  g_free (longname);
  g_free (description);

  sink_template = gst_pad_template_new ("sink",
      GST_PAD_SINK, GST_PAD_ALWAYS, gst_static_caps_get (&sink_caps));
  gst_element_class_add_pad_template (element_class, sink_template);

  src_caps = gst_caps_new_simple (codec_details->mimetype,
      "width", GST_TYPE_INT_RANGE, min_width, max_width,
      "height", GST_TYPE_INT_RANGE, min_height, max_height,
      "framerate", GST_TYPE_FRACTION_RANGE,
      min_fps_n, min_fps_d, max_fps_n, max_fps_d, NULL);
  if (codec_details->format_id == kCMVideoCodecType_H264) {
    gst_structure_set (gst_caps_get_structure (src_caps, 0),
        "stream-format", G_TYPE_STRING, "avc", NULL);
  }
  src_template = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
      src_caps);
  gst_element_class_add_pad_template (element_class, src_template);
}

static void
gst_vtenc_class_init (GstVTEncClass * klass)
{
  GObjectClass *gobject_class;
  GstVideoEncoderClass *gstvideoencoder_class;

  gobject_class = (GObjectClass *) klass;
  gstvideoencoder_class = (GstVideoEncoderClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->get_property = gst_vtenc_get_property;
  gobject_class->set_property = gst_vtenc_set_property;

  gstvideoencoder_class->start = gst_vtenc_start;
  gstvideoencoder_class->stop = gst_vtenc_stop;
  gstvideoencoder_class->set_format = gst_vtenc_set_format;
  gstvideoencoder_class->handle_frame = gst_vtenc_handle_frame;
  gstvideoencoder_class->finish = gst_vtenc_finish;

  g_object_class_install_property (gobject_class, PROP_BITRATE,
      g_param_spec_uint ("bitrate", "Bitrate",
          "Target video bitrate in kbps",
          1, G_MAXUINT, VTENC_DEFAULT_BITRATE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ALLOW_FRAME_REORDERING,
      g_param_spec_boolean ("allow-frame-reordering", "Allow frame reordering",
          "Whether to allow frame reordering or not",
          VTENC_DEFAULT_FRAME_REORDERING,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_REALTIME,
      g_param_spec_boolean ("realtime", "Realtime",
          "Configure the encoder for realtime output",
          VTENC_DEFAULT_REALTIME,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
}

static void
gst_vtenc_init (GstVTEnc * self)
{
  GstVTEncClass *klass = (GstVTEncClass *) G_OBJECT_GET_CLASS (self);

  self->details = GST_VTENC_CLASS_GET_CODEC_DETAILS (klass);

  /* These could be controlled by properties later */
  self->dump_properties = FALSE;
  self->dump_attributes = FALSE;

  self->session = NULL;
}

static guint
gst_vtenc_get_bitrate (GstVTEnc * self)
{
  guint result;

  GST_OBJECT_LOCK (self);
  result = self->bitrate;
  GST_OBJECT_UNLOCK (self);

  return result;
}

static void
gst_vtenc_set_bitrate (GstVTEnc * self, guint bitrate)
{
  GST_OBJECT_LOCK (self);

  self->bitrate = bitrate;

  if (self->session != NULL)
    gst_vtenc_session_configure_bitrate (self, self->session, bitrate);

  GST_OBJECT_UNLOCK (self);
}

static gboolean
gst_vtenc_get_allow_frame_reordering (GstVTEnc * self)
{
  gboolean result;

  GST_OBJECT_LOCK (self);
  result = self->allow_frame_reordering;
  GST_OBJECT_UNLOCK (self);

  return result;
}

static void
gst_vtenc_set_allow_frame_reordering (GstVTEnc * self,
    gboolean allow_frame_reordering)
{
  GST_OBJECT_LOCK (self);
  self->allow_frame_reordering = allow_frame_reordering;
  if (self->session != NULL) {
    gst_vtenc_session_configure_allow_frame_reordering (self,
        self->session, allow_frame_reordering);
  }
  GST_OBJECT_UNLOCK (self);
}

static gboolean
gst_vtenc_get_realtime (GstVTEnc * self)
{
  gboolean result;

  GST_OBJECT_LOCK (self);
  result = self->realtime;
  GST_OBJECT_UNLOCK (self);

  return result;
}

static void
gst_vtenc_set_realtime (GstVTEnc * self, gboolean realtime)
{
  GST_OBJECT_LOCK (self);
  self->realtime = realtime;
  if (self->session != NULL)
    gst_vtenc_session_configure_realtime (self, self->session, realtime);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_vtenc_get_property (GObject * obj, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVTEnc *self = GST_VTENC_CAST (obj);

  switch (prop_id) {
    case PROP_BITRATE:
      g_value_set_uint (value, gst_vtenc_get_bitrate (self) * 8 / 1000);
      break;
    case PROP_ALLOW_FRAME_REORDERING:
      g_value_set_boolean (value, gst_vtenc_get_allow_frame_reordering (self));
      break;
    case PROP_REALTIME:
      g_value_set_boolean (value, gst_vtenc_get_realtime (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
      break;
  }
}

static void
gst_vtenc_set_property (GObject * obj, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstVTEnc *self = GST_VTENC_CAST (obj);

  switch (prop_id) {
    case PROP_BITRATE:
      gst_vtenc_set_bitrate (self, g_value_get_uint (value) * 1000 / 8);
      break;
    case PROP_ALLOW_FRAME_REORDERING:
      gst_vtenc_set_allow_frame_reordering (self, g_value_get_boolean (value));
      break;
    case PROP_REALTIME:
      gst_vtenc_set_realtime (self, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
      break;
  }
}

static gboolean
gst_vtenc_start (GstVideoEncoder * enc)
{
  GstVTEnc *self = GST_VTENC_CAST (enc);

  self->cur_outframes = g_async_queue_new ();

  return TRUE;
}

static gboolean
gst_vtenc_stop (GstVideoEncoder * enc)
{
  GstVTEnc *self = GST_VTENC_CAST (enc);

  GST_OBJECT_LOCK (self);
  gst_vtenc_destroy_session (self, &self->session);
  GST_OBJECT_UNLOCK (self);

  if (self->options != NULL) {
    CFRelease (self->options);
    self->options = NULL;
  }

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  self->negotiated_width = self->negotiated_height = 0;
  self->negotiated_fps_n = self->negotiated_fps_d = 0;

  gst_vtenc_clear_cached_caps_downstream (self);

  g_async_queue_unref (self->cur_outframes);
  self->cur_outframes = NULL;

  return TRUE;
}

static gboolean
gst_vtenc_set_format (GstVideoEncoder * enc, GstVideoCodecState * state)
{
  GstVTEnc *self = GST_VTENC_CAST (enc);
  VTCompressionSessionRef session;

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);

  self->negotiated_width = state->info.width;
  self->negotiated_height = state->info.height;
  self->negotiated_fps_n = state->info.fps_n;
  self->negotiated_fps_d = state->info.fps_d;
  self->video_info = state->info;

  GST_OBJECT_LOCK (self);
  gst_vtenc_destroy_session (self, &self->session);
  GST_OBJECT_UNLOCK (self);

  session = gst_vtenc_create_session (self);
  GST_OBJECT_LOCK (self);
  self->session = session;
  GST_OBJECT_UNLOCK (self);

  if (self->options != NULL)
    CFRelease (self->options);
  self->options = CFDictionaryCreateMutable (NULL, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  return TRUE;
}

static gboolean
gst_vtenc_is_negotiated (GstVTEnc * self)
{
  return self->negotiated_width != 0;
}

static gboolean
gst_vtenc_negotiate_downstream (GstVTEnc * self, CMSampleBufferRef sbuf)
{
  gboolean result;
  GstCaps *caps;
  GstStructure *s;
  GstVideoCodecState *state;

  if (self->caps_width == self->negotiated_width &&
      self->caps_height == self->negotiated_height &&
      self->caps_fps_n == self->negotiated_fps_n &&
      self->caps_fps_d == self->negotiated_fps_d) {
    return TRUE;
  }

  caps = gst_pad_get_pad_template_caps (GST_VIDEO_ENCODER_SRC_PAD (self));
  caps = gst_caps_make_writable (caps);
  s = gst_caps_get_structure (caps, 0);
  gst_structure_set (s,
      "width", G_TYPE_INT, self->negotiated_width,
      "height", G_TYPE_INT, self->negotiated_height,
      "framerate", GST_TYPE_FRACTION,
      self->negotiated_fps_n, self->negotiated_fps_d, NULL);

  if (self->details->format_id == kCMVideoCodecType_H264) {
    CMFormatDescriptionRef fmt;
    CFDictionaryRef atoms;
    CFStringRef avccKey;
    CFDataRef avcc;
    gpointer codec_data;
    gsize codec_data_size;
    GstBuffer *codec_data_buf;

    fmt = CMSampleBufferGetFormatDescription (sbuf);
    atoms = CMFormatDescriptionGetExtension (fmt,
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    avccKey = CFStringCreateWithCString (NULL, "avcC", kCFStringEncodingUTF8);
    avcc = CFDictionaryGetValue (atoms, avccKey);
    CFRelease (avccKey);
    codec_data_size = CFDataGetLength (avcc);
    codec_data = g_malloc (codec_data_size);
    CFDataGetBytes (avcc, CFRangeMake (0, codec_data_size), codec_data);
    codec_data_buf = gst_buffer_new_wrapped (codec_data, codec_data_size);

    gst_structure_set (s, "codec_data", GST_TYPE_BUFFER, codec_data_buf, NULL);

    gst_buffer_unref (codec_data_buf);
  }

  state =
      gst_video_encoder_set_output_state (GST_VIDEO_ENCODER_CAST (self), caps,
      self->input_state);
  gst_video_codec_state_unref (state);
  result = gst_video_encoder_negotiate (GST_VIDEO_ENCODER_CAST (self));

  self->caps_width = self->negotiated_width;
  self->caps_height = self->negotiated_height;
  self->caps_fps_n = self->negotiated_fps_n;
  self->caps_fps_d = self->negotiated_fps_d;

  return result;
}

static void
gst_vtenc_clear_cached_caps_downstream (GstVTEnc * self)
{
  self->caps_width = self->caps_height = 0;
  self->caps_fps_n = self->caps_fps_d = 0;
}

static GstFlowReturn
gst_vtenc_handle_frame (GstVideoEncoder * enc, GstVideoCodecFrame * frame)
{
  GstVTEnc *self = GST_VTENC_CAST (enc);

  if (!gst_vtenc_is_negotiated (self))
    goto not_negotiated;

  return gst_vtenc_encode_frame (self, frame);

not_negotiated:
  gst_video_codec_frame_unref (frame);
  return GST_FLOW_NOT_NEGOTIATED;
}

static GstFlowReturn
gst_vtenc_finish (GstVideoEncoder * enc)
{
  GstVTEnc *self = GST_VTENC_CAST (enc);
  GstFlowReturn ret = GST_FLOW_OK;
  OSStatus vt_status;

  vt_status =
      VTCompressionSessionCompleteFrames (self->session,
      kCMTimePositiveInfinity);
  if (vt_status != noErr) {
    GST_WARNING_OBJECT (self, "VTCompressionSessionCompleteFrames returned %d",
        (int) vt_status);
  }

  while (g_async_queue_length (self->cur_outframes) > 0) {
    GstVideoCodecFrame *outframe = g_async_queue_try_pop (self->cur_outframes);

    ret =
        gst_video_encoder_finish_frame (GST_VIDEO_ENCODER_CAST (self),
        outframe);
  }

  return ret;
}

static VTCompressionSessionRef
gst_vtenc_create_session (GstVTEnc * self)
{
  VTCompressionSessionRef session = NULL;
  CFMutableDictionaryRef pb_attrs;
  OSStatus status;

  pb_attrs = CFDictionaryCreateMutable (NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  gst_vtutil_dict_set_i32 (pb_attrs, kCVPixelBufferWidthKey,
      self->negotiated_width);
  gst_vtutil_dict_set_i32 (pb_attrs, kCVPixelBufferHeightKey,
      self->negotiated_height);

  status = VTCompressionSessionCreate (NULL,
      self->negotiated_width, self->negotiated_height,
      self->details->format_id, NULL, pb_attrs, NULL, gst_vtenc_enqueue_buffer,
      self, &session);
  GST_INFO_OBJECT (self, "VTCompressionSessionCreate for %d x %d => %d",
      self->negotiated_width, self->negotiated_height, (int) status);
  if (status != noErr) {
    GST_ERROR_OBJECT (self, "VTCompressionSessionCreate() returned: %d",
        (int) status);
    goto beach;
  }

  if (self->dump_properties) {
    gst_vtenc_session_dump_properties (self, session);

    self->dump_properties = FALSE;
  }

  gst_vtenc_session_configure_expected_framerate (self, session,
      (gdouble) self->negotiated_fps_n / (gdouble) self->negotiated_fps_d);

  /* FIXME: This is only available since OS X 10.9.6 */
#if HAVE_IOS
  status = VTSessionSetProperty (session,
      kVTCompressionPropertyKey_ProfileLevel,
      kVTProfileLevel_H264_Baseline_AutoLevel);
  GST_DEBUG_OBJECT (self, "kVTCompressionPropertyKey_ProfileLevel => %d",
      (int) status);
#endif

  status = VTSessionSetProperty (session,
      kVTCompressionPropertyKey_AllowTemporalCompression, kCFBooleanTrue);
  GST_DEBUG_OBJECT (self,
      "kVTCompressionPropertyKey_AllowTemporalCompression => %d", (int) status);

  gst_vtenc_session_configure_max_keyframe_interval (self, session, 0);
  gst_vtenc_session_configure_max_keyframe_interval_duration (self, session, 0);

  gst_vtenc_session_configure_bitrate (self, session,
      gst_vtenc_get_bitrate (self));
  gst_vtenc_session_configure_realtime (self, session,
      gst_vtenc_get_realtime (self));
  gst_vtenc_session_configure_allow_frame_reordering (self, session,
      gst_vtenc_get_allow_frame_reordering (self));

#ifdef HAVE_VIDEOTOOLBOX_10_9_6
  if (VTCompressionSessionPrepareToEncodeFrames) {
    status = VTCompressionSessionPrepareToEncodeFrames (session);
    if (status != noErr) {
      GST_ERROR_OBJECT (self,
          "VTCompressionSessionPrepareToEncodeFrames() returned: %d",
          (int) status);
    }
  }
#endif

beach:
  CFRelease (pb_attrs);

  return session;
}

static void
gst_vtenc_destroy_session (GstVTEnc * self, VTCompressionSessionRef * session)
{
  VTCompressionSessionInvalidate (*session);
  if (*session != NULL) {
    CFRelease (*session);
    *session = NULL;
  }
}

typedef struct
{
  GstVTEnc *self;
  VTCompressionSessionRef session;
} GstVTDumpPropCtx;

static void
gst_vtenc_session_dump_property (CFStringRef prop_name,
    CFDictionaryRef prop_attrs, GstVTDumpPropCtx * dpc)
{
  gchar *name_str;
  CFTypeRef prop_value;
  OSStatus status;

  name_str = gst_vtutil_string_to_utf8 (prop_name);
  if (dpc->self->dump_attributes) {
    gchar *attrs_str;

    attrs_str = gst_vtutil_object_to_string (prop_attrs);
    GST_DEBUG_OBJECT (dpc->self, "%s = %s", name_str, attrs_str);
    g_free (attrs_str);
  }

  status = VTSessionCopyProperty (dpc->session, prop_name, NULL, &prop_value);
  if (status == noErr) {
    gchar *value_str;

    value_str = gst_vtutil_object_to_string (prop_value);
    GST_DEBUG_OBJECT (dpc->self, "%s = %s", name_str, value_str);
    g_free (value_str);

    if (prop_value != NULL)
      CFRelease (prop_value);
  } else {
    GST_DEBUG_OBJECT (dpc->self, "%s = <failed to query: %d>",
        name_str, (int) status);
  }

  g_free (name_str);
}

static void
gst_vtenc_session_dump_properties (GstVTEnc * self,
    VTCompressionSessionRef session)
{
  GstVTDumpPropCtx dpc = { self, session };
  CFDictionaryRef dict;
  OSStatus status;

  status = VTSessionCopySupportedPropertyDictionary (session, &dict);
  if (status != noErr)
    goto error;
  CFDictionaryApplyFunction (dict,
      (CFDictionaryApplierFunction) gst_vtenc_session_dump_property, &dpc);
  CFRelease (dict);

  return;

error:
  GST_WARNING_OBJECT (self, "failed to dump properties");
}

static void
gst_vtenc_session_configure_expected_framerate (GstVTEnc * self,
    VTCompressionSessionRef session, gdouble framerate)
{
  gst_vtenc_session_configure_property_double (self, session,
      kVTCompressionPropertyKey_ExpectedFrameRate, framerate);
}

static void
gst_vtenc_session_configure_max_keyframe_interval (GstVTEnc * self,
    VTCompressionSessionRef session, gint interval)
{
  gst_vtenc_session_configure_property_int (self, session,
      kVTCompressionPropertyKey_MaxKeyFrameInterval, interval);
}

static void
gst_vtenc_session_configure_max_keyframe_interval_duration (GstVTEnc * self,
    VTCompressionSessionRef session, gdouble duration)
{
  gst_vtenc_session_configure_property_double (self, session,
      kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, duration);
}

static void
gst_vtenc_session_configure_bitrate (GstVTEnc * self,
    VTCompressionSessionRef session, guint bitrate)
{
  gst_vtenc_session_configure_property_int (self, session,
      kVTCompressionPropertyKey_AverageBitRate, bitrate);
}

static void
gst_vtenc_session_configure_allow_frame_reordering (GstVTEnc * self,
    VTCompressionSessionRef session, gboolean allow_frame_reordering)
{
  VTSessionSetProperty (session, kVTCompressionPropertyKey_AllowFrameReordering,
      allow_frame_reordering ? kCFBooleanTrue : kCFBooleanFalse);
}

static void
gst_vtenc_session_configure_realtime (GstVTEnc * self,
    VTCompressionSessionRef session, gboolean realtime)
{
  VTSessionSetProperty (session, kVTCompressionPropertyKey_RealTime,
      realtime ? kCFBooleanTrue : kCFBooleanFalse);
}

static OSStatus
gst_vtenc_session_configure_property_int (GstVTEnc * self,
    VTCompressionSessionRef session, CFStringRef name, gint value)
{
  CFNumberRef num;
  OSStatus status;
  gchar name_str[128];

  num = CFNumberCreate (NULL, kCFNumberIntType, &value);
  status = VTSessionSetProperty (session, name, num);
  CFRelease (num);

  CFStringGetCString (name, name_str, sizeof (name_str), kCFStringEncodingUTF8);
  GST_DEBUG_OBJECT (self, "%s(%d) => %d", name_str, value, (int) status);

  return status;
}

static OSStatus
gst_vtenc_session_configure_property_double (GstVTEnc * self,
    VTCompressionSessionRef session, CFStringRef name, gdouble value)
{
  CFNumberRef num;
  OSStatus status;
  gchar name_str[128];

  num = CFNumberCreate (NULL, kCFNumberDoubleType, &value);
  status = VTSessionSetProperty (session, name, num);
  CFRelease (num);

  CFStringGetCString (name, name_str, sizeof (name_str), kCFStringEncodingUTF8);
  GST_DEBUG_OBJECT (self, "%s(%f) => %d", name_str, value, (int) status);

  return status;
}

static GstFlowReturn
gst_vtenc_encode_frame (GstVTEnc * self, GstVideoCodecFrame * frame)
{
  CMTime ts, duration;
  GstCoreMediaMeta *meta;
  CVPixelBufferRef pbuf = NULL;
  OSStatus vt_status;
  GstFlowReturn ret = GST_FLOW_OK;
  guint i;
  gboolean forced_keyframe = FALSE;

  if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame)) {
    if (self->options != NULL) {
      GST_INFO_OBJECT (self, "received force-keyframe-event, will force intra");
      CFDictionaryAddValue (self->options,
          kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
      forced_keyframe = TRUE;
    } else {
      GST_INFO_OBJECT (self,
          "received force-keyframe-event but encode not yet started, ignoring");
    }
  }

  ts = CMTimeMake (frame->pts, GST_SECOND);
  if (frame->duration != GST_CLOCK_TIME_NONE)
    duration = CMTimeMake (frame->duration, GST_SECOND);
  else
    duration = kCMTimeInvalid;

  meta = gst_buffer_get_core_media_meta (frame->input_buffer);
  if (meta != NULL) {
    pbuf = gst_core_media_buffer_get_pixel_buffer (frame->input_buffer);
  }
#ifdef HAVE_IOS
  if (pbuf == NULL) {
    GstVideoFrame inframe, outframe;
    GstBuffer *outbuf;
    OSType pixel_format_type;
    CVReturn cv_ret;

    /* FIXME: iOS has special stride requirements that we don't know yet.
     * Copy into a newly allocated pixelbuffer for now. Probably makes
     * sense to create a buffer pool around these at some point.
     */

    switch (GST_VIDEO_INFO_FORMAT (&self->video_info)) {
      case GST_VIDEO_FORMAT_I420:
        pixel_format_type = kCVPixelFormatType_420YpCbCr8Planar;
        break;
      case GST_VIDEO_FORMAT_NV12:
        pixel_format_type = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        break;
      default:
        goto cv_error;
    }

    if (!gst_video_frame_map (&inframe, &self->video_info, frame->input_buffer,
            GST_MAP_READ))
      goto cv_error;

    cv_ret =
        CVPixelBufferCreate (NULL, self->negotiated_width,
        self->negotiated_height, pixel_format_type, NULL, &pbuf);

    if (cv_ret != kCVReturnSuccess) {
      gst_video_frame_unmap (&inframe);
      goto cv_error;
    }

    outbuf = gst_core_video_buffer_new ((CVBufferRef) pbuf, &self->video_info);
    if (!gst_video_frame_map (&outframe, &self->video_info, outbuf,
            GST_MAP_WRITE)) {
      gst_video_frame_unmap (&inframe);
      gst_buffer_unref (outbuf);
      CVPixelBufferRelease (pbuf);
      goto cv_error;
    }

    if (!gst_video_frame_copy (&outframe, &inframe)) {
      gst_video_frame_unmap (&inframe);
      gst_buffer_unref (outbuf);
      CVPixelBufferRelease (pbuf);
      goto cv_error;
    }

    gst_buffer_unref (outbuf);
    gst_video_frame_unmap (&inframe);
    gst_video_frame_unmap (&outframe);
  }
#else
  if (pbuf == NULL) {
    GstVTEncFrame *vframe;
    CVReturn cv_ret;

    vframe = gst_vtenc_frame_new (frame->input_buffer, &self->video_info);
    if (!vframe)
      goto cv_error;

    {
      const size_t num_planes = GST_VIDEO_FRAME_N_PLANES (&vframe->videoframe);
      void *plane_base_addresses[GST_VIDEO_MAX_PLANES];
      size_t plane_widths[GST_VIDEO_MAX_PLANES];
      size_t plane_heights[GST_VIDEO_MAX_PLANES];
      size_t plane_bytes_per_row[GST_VIDEO_MAX_PLANES];
      OSType pixel_format_type;
      size_t i;

      for (i = 0; i < num_planes; i++) {
        plane_base_addresses[i] =
            GST_VIDEO_FRAME_PLANE_DATA (&vframe->videoframe, i);
        plane_widths[i] = GST_VIDEO_FRAME_COMP_WIDTH (&vframe->videoframe, i);
        plane_heights[i] = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe->videoframe, i);
        plane_bytes_per_row[i] =
            GST_VIDEO_FRAME_COMP_STRIDE (&vframe->videoframe, i);
        plane_bytes_per_row[i] =
            GST_VIDEO_FRAME_COMP_STRIDE (&vframe->videoframe, i);
      }

      switch (GST_VIDEO_INFO_FORMAT (&self->video_info)) {
        case GST_VIDEO_FORMAT_I420:
          pixel_format_type = kCVPixelFormatType_420YpCbCr8Planar;
          break;
        case GST_VIDEO_FORMAT_NV12:
          pixel_format_type = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
          break;
        default:
          goto cv_error;
      }

      cv_ret = CVPixelBufferCreateWithPlanarBytes (NULL,
          self->negotiated_width, self->negotiated_height,
          pixel_format_type,
          frame,
          GST_VIDEO_FRAME_SIZE (&vframe->videoframe),
          num_planes,
          plane_base_addresses,
          plane_widths,
          plane_heights,
          plane_bytes_per_row, gst_pixel_buffer_release_cb, vframe, NULL,
          &pbuf);
      if (cv_ret != kCVReturnSuccess) {
        gst_vtenc_frame_free (vframe);
        goto cv_error;
      }
    }
  }
#endif

  /* We need to unlock the stream lock here because
   * it can wait for gst_vtenc_enqueue_buffer() to
   * handle a buffer... which will take the stream
   * lock from another thread and then deadlock */
  GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
  vt_status = VTCompressionSessionEncodeFrame (self->session,
      pbuf, ts, duration, self->options,
      GINT_TO_POINTER (frame->system_frame_number), NULL);
  GST_VIDEO_ENCODER_STREAM_LOCK (self);

  /* Only force one keyframe */
  if (forced_keyframe) {
    CFDictionaryRemoveValue (self->options,
        kVTEncodeFrameOptionKey_ForceKeyFrame);
  }

  if (vt_status != noErr) {
    GST_WARNING_OBJECT (self, "VTCompressionSessionEncodeFrame returned %d",
        (int) vt_status);
  }

  gst_video_codec_frame_unref (frame);

  CVPixelBufferRelease (pbuf);

  i = 0;
  while (g_async_queue_length (self->cur_outframes) > 0) {
    GstVideoCodecFrame *outframe = g_async_queue_try_pop (self->cur_outframes);

    /* Try to renegotiate once */
    if (i == 0) {
      meta = gst_buffer_get_core_media_meta (outframe->output_buffer);
      if (!gst_vtenc_negotiate_downstream (self, meta->sample_buf)) {
        ret = GST_FLOW_NOT_NEGOTIATED;
        gst_video_codec_frame_unref (outframe);
        break;
      }
    }

    ret =
        gst_video_encoder_finish_frame (GST_VIDEO_ENCODER_CAST (self),
        outframe);
    i++;
  }

  return ret;

cv_error:
  {
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
}

static void
gst_vtenc_enqueue_buffer (void *outputCallbackRefCon,
    void *sourceFrameRefCon,
    OSStatus status,
    VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer)
{
  GstVTEnc *self = outputCallbackRefCon;
  gboolean is_keyframe;
  GstVideoCodecFrame *frame;

  if (status != noErr) {
    GST_ELEMENT_ERROR (self, LIBRARY, ENCODE, (NULL), ("Failed to encode: %d",
            (int) status));
    goto beach;
  }

  /* This may happen if we don't have enough bitrate */
  if (sampleBuffer == NULL)
    goto beach;

  is_keyframe = gst_vtenc_buffer_is_keyframe (self, sampleBuffer);

  frame =
      gst_video_encoder_get_frame (GST_VIDEO_ENCODER_CAST (self),
      GPOINTER_TO_INT (sourceFrameRefCon));

  if (is_keyframe) {
    GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
    gst_vtenc_clear_cached_caps_downstream (self);
  }

  /* We are dealing with block buffers here, so we don't need
   * to enable the use of the video meta API on the core media buffer */
  frame->output_buffer = gst_core_media_buffer_new (sampleBuffer, FALSE);

  g_async_queue_push (self->cur_outframes, frame);

beach:
  return;
}

static gboolean
gst_vtenc_buffer_is_keyframe (GstVTEnc * self, CMSampleBufferRef sbuf)
{
  gboolean result = FALSE;
  CFArrayRef attachments_for_sample;

  attachments_for_sample = CMSampleBufferGetSampleAttachmentsArray (sbuf, 0);
  if (attachments_for_sample != NULL) {
    CFDictionaryRef attachments;
    CFBooleanRef depends_on_others;

    attachments = CFArrayGetValueAtIndex (attachments_for_sample, 0);
    depends_on_others = CFDictionaryGetValue (attachments,
        kCMSampleAttachmentKey_DependsOnOthers);
    result = (depends_on_others == kCFBooleanFalse);
  }

  return result;
}

#ifndef HAVE_IOS
static GstVTEncFrame *
gst_vtenc_frame_new (GstBuffer * buf, GstVideoInfo * video_info)
{
  GstVTEncFrame *frame;

  frame = g_slice_new (GstVTEncFrame);
  frame->buf = gst_buffer_ref (buf);
  if (!gst_video_frame_map (&frame->videoframe, video_info, buf, GST_MAP_READ)) {
    gst_buffer_unref (frame->buf);
    g_slice_free (GstVTEncFrame, frame);
    return NULL;
  }

  return frame;
}

static void
gst_vtenc_frame_free (GstVTEncFrame * frame)
{
  gst_video_frame_unmap (&frame->videoframe);
  gst_buffer_unref (frame->buf);
  g_slice_free (GstVTEncFrame, frame);
}

static void
gst_pixel_buffer_release_cb (void *releaseRefCon, const void *dataPtr,
    size_t dataSize, size_t numberOfPlanes, const void *planeAddresses[])
{
  GstVTEncFrame *frame = (GstVTEncFrame *) releaseRefCon;
  gst_vtenc_frame_free (frame);
}
#endif

static void
gst_vtenc_register (GstPlugin * plugin,
    const GstVTEncoderDetails * codec_details)
{
  GTypeInfo type_info = {
    sizeof (GstVTEncClass),
    (GBaseInitFunc) gst_vtenc_base_init,
    NULL,
    (GClassInitFunc) gst_vtenc_class_init,
    NULL,
    NULL,
    sizeof (GstVTEnc),
    0,
    (GInstanceInitFunc) gst_vtenc_init,
  };
  gchar *type_name;
  GType type;
  gboolean result;

  type_name = g_strdup_printf ("vtenc_%s", codec_details->element_name);

  type =
      g_type_register_static (GST_TYPE_VIDEO_ENCODER, type_name, &type_info, 0);

  g_type_set_qdata (type, GST_VTENC_CODEC_DETAILS_QDATA,
      (gpointer) codec_details);

  result = gst_element_register (plugin, type_name, GST_RANK_NONE, type);
  if (!result) {
    GST_ERROR_OBJECT (plugin, "failed to register element %s", type_name);
  }

  g_free (type_name);
}

static const GstVTEncoderDetails gst_vtenc_codecs[] = {
  {"H.264", "h264", "video/x-h264", kCMVideoCodecType_H264},
};

void
gst_vtenc_register_elements (GstPlugin * plugin)
{
  guint i;

  GST_DEBUG_CATEGORY_INIT (gst_vtenc_debug, "vtenc",
      0, "Apple VideoToolbox Encoder Wrapper");

  for (i = 0; i != G_N_ELEMENTS (gst_vtenc_codecs); i++)
    gst_vtenc_register (plugin, &gst_vtenc_codecs[i]);
}
