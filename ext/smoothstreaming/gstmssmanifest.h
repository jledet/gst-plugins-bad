/* GStreamer
 * Copyright (C) 2012 Smart TV Alliance
 *  Author: Thiago Sousa Santos <thiago.sousa.santos@collabora.com>, Collabora Ltd.
 *
 * gstmssmanifest.h:
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_MSS_MANIFEST_H__
#define __GST_MSS_MANIFEST_H__

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstMssManifest GstMssManifest;

GstMssManifest * gst_mss_manifest_new (GstBuffer * data); 
void gst_mss_manifest_free (GstMssManifest * manifest);

G_END_DECLS
#endif /* __GST_MSS_MANIFEST_H__ */