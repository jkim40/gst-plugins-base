/* GStreamer
 * Copyright (C) <2003> Julien Moutte <julien@moutte.net>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Our interfaces */
#include <gst/interfaces/navigation.h>
#include <gst/interfaces/xoverlay.h>
#include <gst/interfaces/colorbalance.h>

/* Object header */
#include "xvimagesink.h"

/* Debugging category */
#include <gst/gstinfo.h>
GST_DEBUG_CATEGORY_STATIC (gst_debug_xvimagesink);
#define GST_CAT_DEFAULT gst_debug_xvimagesink

typedef struct
{
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
}
MotifWmHints, MwmHints;

#define MWM_HINTS_DECORATIONS   (1L << 1)

static void gst_xvimage_buffer_finalize (GstXvImageBuffer * xvimage);

//static void gst_xvimagesink_send_pending_navigation (GstXvImageSink * xvimagesink);

/* ElementFactory information */
static GstElementDetails gst_xvimagesink_details =
GST_ELEMENT_DETAILS ("Video sink",
    "Sink/Video",
    "A Xv based videosink",
    "Julien Moutte <julien@moutte.net>");

/* Default template - initiated with class struct to allow gst-register to work
   without X running */
static GstStaticPadTemplate gst_xvimagesink_sink_template_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw-rgb, "
        "framerate = (double) [ 1.0, 100.0 ], "
        "width = (int) [ 1, MAX ], "
        "height = (int) [ 1, MAX ]; "
        "video/x-raw-yuv, "
        "framerate = (double) [ 1.0, 100.0 ], "
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]")
    );

enum
{
  ARG_0,
  ARG_CONTRAST,
  ARG_BRIGHTNESS,
  ARG_HUE,
  ARG_SATURATION,
  ARG_DISPLAY,
  ARG_SYNCHRONOUS,
  ARG_PIXEL_ASPECT_RATIO
      /* FILL ME */
};

static GstVideoSinkClass *parent_class = NULL;

/* ============================================================= */
/*                                                               */
/*                       Private Methods                         */
/*                                                               */
/* ============================================================= */

/* xvimage buffers */

#define GST_TYPE_XVIMAGE_BUFFER (gst_xvimage_buffer_get_type())

#define GST_IS_XVIMAGE_BUFFER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_XVIMAGE_BUFFER))
#define GST_XVIMAGE_BUFFER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_XVIMAGE_BUFFER, GstXvImageBuffer))

/* This function destroys a GstXvImage handling XShm availability */
static void
gst_xvimage_buffer_destroy (GstXvImageBuffer * xvimage)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = xvimage->xvimagesink;
  if (xvimagesink == NULL)
    goto no_sink;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  /* If the destroyed image is the current one we destroy our reference too */
  if (xvimagesink->cur_image == xvimage)
    xvimagesink->cur_image = NULL;

  g_mutex_lock (xvimagesink->x_lock);

#ifdef HAVE_XSHM
  if (xvimagesink->xcontext->use_xshm) {
    if (xvimage->SHMInfo.shmaddr != ((void *) -1)) {
      XShmDetach (xvimagesink->xcontext->disp, &xvimage->SHMInfo);
      XSync (xvimagesink->xcontext->disp, FALSE);
      shmdt (xvimage->SHMInfo.shmaddr);
    }
    if (xvimage->SHMInfo.shmid > 0)
      shmctl (xvimage->SHMInfo.shmid, IPC_RMID, 0);
    if (xvimage->xvimage)
      XFree (xvimage->xvimage);
  } else
#endif /* HAVE_XSHM */
  {
    if (xvimage->xvimage) {
      if (xvimage->xvimage->data) {
        g_free (xvimage->xvimage->data);
      }
      XFree (xvimage->xvimage);
    }
  }

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  xvimage->xvimagesink = NULL;
  gst_object_unref (xvimagesink);

  return;

no_sink:
  {
    GST_WARNING ("no sink found");
    return;
  }
}

static void
gst_xvimage_buffer_finalize (GstXvImageBuffer * xvimage)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = xvimage->xvimagesink;
  if (xvimagesink == NULL)
    goto no_sink;

  /* If our geometry changed we can't reuse that image. */
  if ((xvimage->width != xvimagesink->video_width) ||
      (xvimage->height != xvimagesink->video_height)) {
    GST_DEBUG ("destroy image as its size changed %dx%d vs current %dx%d",
        xvimage->width, xvimage->height,
        xvimagesink->video_width, xvimagesink->video_height);
    gst_xvimage_buffer_destroy (xvimage);
  } else {
    /* In that case we can reuse the image and add it to our image pool. */
    GST_DEBUG ("recycling image in pool");
    /* need to increment the refcount again to recycle */
    gst_buffer_ref (GST_BUFFER (xvimage));
    g_mutex_lock (xvimagesink->pool_lock);
    xvimagesink->image_pool = g_slist_prepend (xvimagesink->image_pool,
        xvimage);
    g_mutex_unlock (xvimagesink->pool_lock);
  }
  return;

no_sink:
  {
    GST_WARNING ("no sink found");
    return;
  }
}

static void
gst_xvimage_buffer_free (GstXvImageBuffer * xvimage)
{
  /* make sure it is not recycled */
  xvimage->width = -1;
  xvimage->height = -1;
  gst_buffer_unref (GST_BUFFER (xvimage));
}

static void
gst_xvimage_buffer_init (GstXvImageBuffer * xvimage, gpointer g_class)
{
#ifdef HAVE_XSHM
  xvimage->SHMInfo.shmaddr = ((void *) -1);
  xvimage->SHMInfo.shmid = -1;
#endif
}

static void
gst_xvimage_buffer_class_init (gpointer g_class, gpointer class_data)
{
  GstMiniObjectClass *mini_object_class = GST_MINI_OBJECT_CLASS (g_class);

  mini_object_class->finalize = (GstMiniObjectFinalizeFunction)
      gst_xvimage_buffer_finalize;
}

GType
gst_xvimage_buffer_get_type (void)
{
  static GType _gst_xvimage_buffer_type;

  if (G_UNLIKELY (_gst_xvimage_buffer_type == 0)) {
    static const GTypeInfo xvimage_buffer_info = {
      sizeof (GstBufferClass),
      NULL,
      NULL,
      gst_xvimage_buffer_class_init,
      NULL,
      NULL,
      sizeof (GstXvImageBuffer),
      0,
      (GInstanceInitFunc) gst_xvimage_buffer_init,
      NULL
    };
    _gst_xvimage_buffer_type = g_type_register_static (GST_TYPE_BUFFER,
        "GstXvImageBuffer", &xvimage_buffer_info, 0);
  }
  return _gst_xvimage_buffer_type;
}

/* X11 stuff */

#ifdef HAVE_XSHM
static gboolean error_caught = FALSE;

static int
gst_xvimagesink_handle_xerror (Display * display, XErrorEvent * xevent)
{
  char error_msg[1024];

  XGetErrorText (display, xevent->error_code, error_msg, 1024);
  GST_DEBUG ("xvimagesink failed to use XShm calls. error: %s", error_msg);
  error_caught = TRUE;
  return 0;
}

/* This function checks that it is actually really possible to create an image
   using XShm */
static gboolean
gst_xvimagesink_check_xshm_calls (GstXContext * xcontext)
{
  XvImage *xvimage;
  XShmSegmentInfo SHMInfo;
  gint size;
  int (*handler) (Display *, XErrorEvent *);
  gboolean result = FALSE;

  g_return_val_if_fail (xcontext != NULL, FALSE);

  /* Setting an error handler to catch failure */
  error_caught = FALSE;
  handler = XSetErrorHandler (gst_xvimagesink_handle_xerror);

  /* Trying to create a 1x1 picture */
  GST_DEBUG ("XvShmCreateImage of 1x1");
  xvimage = XvShmCreateImage (xcontext->disp, xcontext->xv_port_id,
      xcontext->im_format, NULL, 1, 1, &SHMInfo);
  if (!xvimage) {
    GST_WARNING ("could not XvShmCreateImage a 1x1 image");
    goto beach;
  }
  size = xvimage->data_size;

  SHMInfo.shmid = shmget (IPC_PRIVATE, size, IPC_CREAT | 0777);
  if (SHMInfo.shmid == -1) {
    GST_WARNING ("could not get shared memory of %d bytes", size);
    goto beach;
  }

  SHMInfo.shmaddr = shmat (SHMInfo.shmid, 0, 0);
  if (SHMInfo.shmaddr == ((void *) -1)) {
    GST_WARNING ("Failed to shmat: %s", g_strerror (errno));
    goto beach;
  }

  xvimage->data = SHMInfo.shmaddr;
  SHMInfo.readOnly = FALSE;

  if (XShmAttach (xcontext->disp, &SHMInfo) == 0) {
    GST_WARNING ("Failed to XShmAttach");
    goto beach;
  }

  /* store whether we succeeded in result and reset error_caught */
  result = !error_caught;
  error_caught = FALSE;

beach:
  XSetErrorHandler (handler);

  XSync (xcontext->disp, FALSE);
  if (SHMInfo.shmaddr != ((void *) -1)) {
    XShmDetach (xcontext->disp, &SHMInfo);
    XSync (xcontext->disp, FALSE);
    shmdt (SHMInfo.shmaddr);
  }
  if (SHMInfo.shmid > 0)
    shmctl (SHMInfo.shmid, IPC_RMID, 0);
  if (xvimage)
    XFree (xvimage);
  return result;
}
#endif /* HAVE_XSHM */

/* This function handles GstXvImage creation depending on XShm availability */
static GstXvImageBuffer *
gst_xvimagesink_xvimage_new (GstXvImageSink * xvimagesink,
    gint width, gint height)
{
  GstXvImageBuffer *xvimage = NULL;
  gboolean succeeded = FALSE;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);
  GST_LOG_OBJECT (xvimagesink, "creating %dx%d", width, height);

  xvimage = (GstXvImageBuffer *) gst_mini_object_new (GST_TYPE_XVIMAGE_BUFFER);

  xvimage->width = width;
  xvimage->height = height;
  xvimage->im_format = xvimagesink->xcontext->im_format;
  xvimage->xvimagesink = gst_object_ref (xvimagesink);

  g_mutex_lock (xvimagesink->x_lock);

#ifdef HAVE_XSHM
  if (xvimagesink->xcontext->use_xshm) {
    xvimage->xvimage = XvShmCreateImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimage->im_format, NULL,
        xvimage->width, xvimage->height, &xvimage->SHMInfo);
    if (!xvimage->xvimage) {
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE, (NULL),
          ("could not XvShmCreateImage a %dx%d image"));
      goto beach;
    }

    /* we have to use the returned data_size for our shm size */
    xvimage->size = xvimage->xvimage->data_size;
    GST_LOG_OBJECT (xvimagesink, "XShm image size is %d", xvimage->size);

    xvimage->SHMInfo.shmid = shmget (IPC_PRIVATE, xvimage->size,
        IPC_CREAT | 0777);
    if (xvimage->SHMInfo.shmid == -1) {
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE, (NULL),
          ("could not get shared memory of %d bytes", xvimage->size));
      goto beach;
    }

    xvimage->SHMInfo.shmaddr = shmat (xvimage->SHMInfo.shmid, 0, 0);
    if (xvimage->SHMInfo.shmaddr == ((void *) -1)) {
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE, (NULL),
          ("Failed to shmat: %s", g_strerror (errno)));
      goto beach;
    }

    xvimage->xvimage->data = xvimage->SHMInfo.shmaddr;
    xvimage->SHMInfo.readOnly = FALSE;

    if (XShmAttach (xvimagesink->xcontext->disp, &xvimage->SHMInfo) == 0) {
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE, (NULL),
          ("Failed to XShmAttach"));
      goto beach;
    }

    XSync (xvimagesink->xcontext->disp, FALSE);
  } else
#endif /* HAVE_XSHM */
  {
    xvimage->xvimage = XvCreateImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimage->im_format, NULL, xvimage->width, xvimage->height);
    if (!xvimage->xvimage) {
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE, (NULL),
          ("could not XvCreateImage a %dx%d image"));
      goto beach;
    }

    /* we have to use the returned data_size for our image size */
    xvimage->size = xvimage->xvimage->data_size;
    xvimage->xvimage->data = g_malloc (xvimage->size);

    XSync (xvimagesink->xcontext->disp, FALSE);
  }
  succeeded = TRUE;

  GST_BUFFER_DATA (xvimage) = (guchar *) xvimage->xvimage->data;
  GST_BUFFER_SIZE (xvimage) = xvimage->size;

beach:
  g_mutex_unlock (xvimagesink->x_lock);

  if (!succeeded) {
    gst_xvimage_buffer_free (xvimage);
    xvimage = NULL;
  }

  return xvimage;
}

/* This function puts a GstXvImage on a GstXvImageSink's window */
static void
gst_xvimagesink_xvimage_put (GstXvImageSink * xvimagesink,
    GstXvImageBuffer * xvimage)
{
  g_return_if_fail (xvimage != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));
  g_return_if_fail (xvimagesink->xwindow != NULL);

  /* Store a reference to the last image we put */
  if (xvimagesink->cur_image != xvimage)
    xvimagesink->cur_image = xvimage;

  g_mutex_lock (xvimagesink->x_lock);

  /* We scale to the window's geometry */
#ifdef HAVE_XSHM
  if (xvimagesink->xcontext->use_xshm) {
    GST_LOG_OBJECT (xvimagesink,
        "XvShmPutImage with image %dx%d and window %dx%d",
        xvimage->width, xvimage->height,
        xvimagesink->xwindow->width, xvimagesink->xwindow->height);
    XvShmPutImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimagesink->xwindow->win,
        xvimagesink->xwindow->gc, xvimage->xvimage,
        0, 0, xvimage->width, xvimage->height,
        0, 0, xvimagesink->xwindow->width, xvimagesink->xwindow->height, FALSE);
  } else
#endif /* HAVE_XSHM */
  {
    XvPutImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimagesink->xwindow->win,
        xvimagesink->xwindow->gc, xvimage->xvimage,
        0, 0, xvimage->width, xvimage->height,
        0, 0, xvimagesink->xwindow->width, xvimagesink->xwindow->height);
  }

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);
}

static gboolean
gst_xvimagesink_xwindow_decorate (GstXvImageSink * xvimagesink,
    GstXWindow * window)
{
  Atom hints_atom = None;
  MotifWmHints *hints;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), FALSE);
  g_return_val_if_fail (window != NULL, FALSE);

  g_mutex_lock (xvimagesink->x_lock);

  hints_atom = XInternAtom (xvimagesink->xcontext->disp, "_MOTIF_WM_HINTS", 1);
  if (hints_atom == None) {
    g_mutex_unlock (xvimagesink->x_lock);
    return FALSE;
  }

  hints = g_malloc0 (sizeof (MotifWmHints));

  hints->flags |= MWM_HINTS_DECORATIONS;
  hints->decorations = 1 << 0;

  XChangeProperty (xvimagesink->xcontext->disp, window->win,
      hints_atom, hints_atom, 32, PropModeReplace,
      (guchar *) hints, sizeof (MotifWmHints) / sizeof (long));

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  g_free (hints);

  return TRUE;
}

/* This function handles a GstXWindow creation
 * The width and height are the actual pixel size on the display */
static GstXWindow *
gst_xvimagesink_xwindow_new (GstXvImageSink * xvimagesink,
    gint width, gint height)
{
  GstXWindow *xwindow = NULL;
  XGCValues values;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  xwindow = g_new0 (GstXWindow, 1);

  xwindow->width = width;
  xwindow->height = height;
  xwindow->internal = TRUE;

  g_mutex_lock (xvimagesink->x_lock);

  xwindow->win = XCreateSimpleWindow (xvimagesink->xcontext->disp,
      xvimagesink->xcontext->root,
      0, 0, xwindow->width, xwindow->height,
      0, 0, xvimagesink->xcontext->black);

  XSelectInput (xvimagesink->xcontext->disp, xwindow->win, ExposureMask |
      StructureNotifyMask | PointerMotionMask | KeyPressMask |
      KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);

  xwindow->gc = XCreateGC (xvimagesink->xcontext->disp,
      xwindow->win, 0, &values);

  XMapRaised (xvimagesink->xcontext->disp, xwindow->win);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  gst_xvimagesink_xwindow_decorate (xvimagesink, xwindow);

  gst_x_overlay_got_xwindow_id (GST_X_OVERLAY (xvimagesink), xwindow->win);

  return xwindow;
}

/* This function destroys a GstXWindow */
static void
gst_xvimagesink_xwindow_destroy (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow)
{
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->x_lock);

  /* If we did not create that window we just free the GC and let it live */
  if (xwindow->internal)
    XDestroyWindow (xvimagesink->xcontext->disp, xwindow->win);
  else
    XSelectInput (xvimagesink->xcontext->disp, xwindow->win, 0);

  XFreeGC (xvimagesink->xcontext->disp, xwindow->gc);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  g_free (xwindow);
}

/* This function resizes a GstXWindow.
 * The width and height are the actual pixel size on the display. */
static void
gst_xvimagesink_xwindow_resize (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow, guint width, guint height)
{
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->x_lock);

  xwindow->width = width;
  xwindow->height = height;

  XResizeWindow (xvimagesink->xcontext->disp, xwindow->win,
      xwindow->width, xwindow->height);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);
}

static void
gst_xvimagesink_xwindow_clear (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow)
{
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->x_lock);

  XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id,
      xwindow->win);

  XSetForeground (xvimagesink->xcontext->disp, xwindow->gc,
      xvimagesink->xcontext->black);

  XFillRectangle (xvimagesink->xcontext->disp, xwindow->win, xwindow->gc,
      0, 0, xwindow->width, xwindow->height);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);
}

/* This function commits our internal colorbalance settings to our grabbed Xv
   port. If the xcontext is not initialized yet it simply returns */
static void
gst_xvimagesink_update_colorbalance (GstXvImageSink * xvimagesink)
{
  GList *channels = NULL;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  /* If we haven't initialized the X context we can't update anything */
  if (xvimagesink->xcontext == NULL)
    return;

  /* For each channel of the colorbalance we calculate the correct value
     doing range conversion and then set the Xv port attribute to match our
     values. */
  channels = xvimagesink->xcontext->channels_list;

  while (channels) {
    if (channels->data && GST_IS_COLOR_BALANCE_CHANNEL (channels->data)) {
      GstColorBalanceChannel *channel = NULL;
      gint value = 0;
      gdouble convert_coef;

      channel = GST_COLOR_BALANCE_CHANNEL (channels->data);
      g_object_ref (channel);

      /* Our range conversion coef */
      convert_coef = (channel->max_value - channel->min_value) / 2000.0;

      if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
        value = (xvimagesink->hue + 1000) * convert_coef + channel->min_value;
      } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
        value = (xvimagesink->saturation + 1000) * convert_coef +
            channel->min_value;
      } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
        value = (xvimagesink->contrast + 1000) * convert_coef +
            channel->min_value;
      } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
        value = (xvimagesink->brightness + 1000) * convert_coef +
            channel->min_value;
      } else {
        g_warning ("got an unknown channel %s", channel->label);
        g_object_unref (channel);
        return;
      }

      /* Committing to Xv port */
      g_mutex_lock (xvimagesink->x_lock);
      XvSetPortAttribute (xvimagesink->xcontext->disp,
          xvimagesink->xcontext->xv_port_id,
          XInternAtom (xvimagesink->xcontext->disp, channel->label, 1), value);
      g_mutex_unlock (xvimagesink->x_lock);

      g_object_unref (channel);
    }
    channels = g_list_next (channels);
  }
}

/* This function handles XEvents that might be in the queue. It generates
   GstEvent that will be sent upstream in the pipeline to handle interactivity
   and navigation. It will also listen for configure events on the window to
   trigger caps renegotiation so on the fly software scaling can work. */
static void
gst_xvimagesink_handle_xevents (GstXvImageSink * xvimagesink, GstPad * pad)
{
  XEvent e;
  guint pointer_x = 0, pointer_y = 0;
  gboolean pointer_moved = FALSE;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  /* We get all pointer motion events, only the last position is
     interesting. */
  g_mutex_lock (xvimagesink->x_lock);
  while (XCheckWindowEvent (xvimagesink->xcontext->disp,
          xvimagesink->xwindow->win, PointerMotionMask, &e)) {
    g_mutex_unlock (xvimagesink->x_lock);

    switch (e.type) {
      case MotionNotify:
        pointer_x = e.xmotion.x;
        pointer_y = e.xmotion.y;
        pointer_moved = TRUE;
        break;
      default:
        break;
    }

    g_mutex_lock (xvimagesink->x_lock);
  }
  g_mutex_unlock (xvimagesink->x_lock);

  if (pointer_moved) {
    GST_DEBUG ("xvimagesink pointer moved over window at %d,%d",
        pointer_x, pointer_y);
    gst_navigation_send_mouse_event (GST_NAVIGATION (xvimagesink),
        "mouse-move", 0, e.xbutton.x, e.xbutton.y);
  }

  /* We get all events on our window to throw them upstream */
  g_mutex_lock (xvimagesink->x_lock);
  while (XCheckWindowEvent (xvimagesink->xcontext->disp,
          xvimagesink->xwindow->win,
          StructureNotifyMask | KeyPressMask |
          KeyReleaseMask | ButtonPressMask | ButtonReleaseMask, &e)) {
    KeySym keysym;

    /* We lock only for the X function call */
    g_mutex_unlock (xvimagesink->x_lock);

    switch (e.type) {
      case ConfigureNotify:
        /* Window got resized or moved. We update our data. */
        GST_DEBUG ("xvimagesink window is at %d, %d with geometry : %d,%d",
            e.xconfigure.x, e.xconfigure.y,
            e.xconfigure.width, e.xconfigure.height);
        xvimagesink->xwindow->width = e.xconfigure.width;
        xvimagesink->xwindow->height = e.xconfigure.height;
        break;
      case ButtonPress:
        /* Mouse button pressed over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("xvimagesink button %d pressed over window at %d,%d",
            e.xbutton.button, e.xbutton.x, e.xbutton.y);
        gst_navigation_send_mouse_event (GST_NAVIGATION (xvimagesink),
            "mouse-button-press", e.xbutton.button, e.xbutton.x, e.xbutton.y);
        break;
      case ButtonRelease:
        /* Mouse button released over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("xvimagesink button %d released over window at %d,%d",
            e.xbutton.button, e.xbutton.x, e.xbutton.y);
        gst_navigation_send_mouse_event (GST_NAVIGATION (xvimagesink),
            "mouse-button-release", e.xbutton.button, e.xbutton.x, e.xbutton.y);
        break;
      case KeyPress:
      case KeyRelease:
        /* Key pressed/released over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("xvimagesink key %d pressed over window at %d,%d",
            e.xkey.keycode, e.xkey.x, e.xkey.y);
        keysym = XKeycodeToKeysym (xvimagesink->xcontext->disp,
            e.xkey.keycode, 0);
        if (keysym != NoSymbol) {
          gst_navigation_send_key_event (GST_NAVIGATION (xvimagesink),
              e.type == KeyPress ?
              "key-press" : "key-release", XKeysymToString (keysym));
        } else {
          gst_navigation_send_key_event (GST_NAVIGATION (xvimagesink),
              e.type == KeyPress ? "key-press" : "key-release", "unknown");
        }
        break;
      default:
        GST_DEBUG ("xvimagesink unhandled X event (%d)", e.type);
    }

    g_mutex_lock (xvimagesink->x_lock);
  }
  g_mutex_unlock (xvimagesink->x_lock);
}

/* This function generates a caps with all supported format by the first
   Xv grabable port we find. We store each one of the supported formats in a
   format list and append the format to a newly created caps that we return
   If this function does not return NULL because of an error, it also grabs
   the port via XvGrabPort */
static GstCaps *
gst_xvimagesink_get_xv_support (GstXvImageSink * xvimagesink,
    GstXContext * xcontext)
{
  gint i;
  guint nb_adaptors;
  XvAdaptorInfo *adaptors;
  gint nb_formats;
  XvImageFormatValues *formats = NULL;
  GstCaps *caps = NULL;

  g_return_val_if_fail (xcontext != NULL, NULL);

  /* First let's check that XVideo extension is available */
  if (!XQueryExtension (xcontext->disp, "XVideo", &i, &i, &i)) {
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, SETTINGS, (NULL),
        ("XVideo extension is not available"));
    return NULL;
  }

  /* Then we get adaptors list */
  if (Success != XvQueryAdaptors (xcontext->disp, xcontext->root,
          &nb_adaptors, &adaptors)) {
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, SETTINGS, (NULL),
        ("Failed getting XV adaptors list"));
    return NULL;
  }

  xcontext->xv_port_id = 0;

  GST_DEBUG ("Found %u XV adaptor(s)", nb_adaptors);

  /* Now search for an adaptor that supports XvImageMask */
  for (i = 0; i < nb_adaptors && !xcontext->xv_port_id; i++) {
    if (adaptors[i].type & XvImageMask) {
      gint j;

      /* We found such an adaptor, looking for an available port */
      for (j = 0; j < adaptors[i].num_ports && !xcontext->xv_port_id; j++) {
        /* We try to grab the port */
        if (Success == XvGrabPort (xcontext->disp, adaptors[i].base_id + j, 0)) {
          xcontext->xv_port_id = adaptors[i].base_id + j;
        }
      }
    }

    GST_DEBUG ("XV Adaptor %s with %ld ports", adaptors[i].name,
        adaptors[i].num_ports);

  }
  XvFreeAdaptorInfo (adaptors);

  if (!xcontext->xv_port_id) {
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, BUSY, (NULL),
        ("No port available"));
    return NULL;
  }

  /* Set XV_AUTOPAINT_COLORKEY */
  {
    int count;
    XvAttribute *const attr = XvQueryPortAttributes (xcontext->disp,
        xcontext->xv_port_id, &count);
    static const char autopaint[] = "XV_AUTOPAINT_COLORKEY";

    for (i = 0; i < count; i++)
      if (!strcmp (attr[i].name, autopaint)) {
        const Atom atom = XInternAtom (xcontext->disp, autopaint, False);

        XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom, 1);
        break;
      }

    XFree (attr);
  }

  /* We get all image formats supported by our port */
  formats = XvListImageFormats (xcontext->disp,
      xcontext->xv_port_id, &nb_formats);
  caps = gst_caps_new_empty ();
  for (i = 0; i < nb_formats; i++) {
    GstCaps *format_caps = NULL;

    /* We set the image format of the xcontext to an existing one. Sink
       connect method will override that but we need to have at least a
       valid image format so that we can make our xshm calls check before
       caps negotiation really happens. */
    xcontext->im_format = formats[i].id;

    switch (formats[i].type) {
      case XvRGB:
      {
        format_caps = gst_caps_new_simple ("video/x-raw-rgb",
            "endianness", G_TYPE_INT, xcontext->endianness,
            "depth", G_TYPE_INT, xcontext->depth,
            "bpp", G_TYPE_INT, xcontext->bpp,
            "blue_mask", G_TYPE_INT, formats[i].red_mask,
            "green_mask", G_TYPE_INT, formats[i].green_mask,
            "red_mask", G_TYPE_INT, formats[i].blue_mask,
            "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "framerate", GST_TYPE_DOUBLE_RANGE, 1.0, 100.0, NULL);

        /* For RGB caps we store them and the image
           format so that we can get back the format
           when sinkconnect will give us a caps without
           format property */
        if (format_caps) {
          GstXvImageFormat *format = NULL;

          format = g_new0 (GstXvImageFormat, 1);
          if (format) {
            format->format = formats[i].id;
            format->caps = gst_caps_copy (format_caps);
            xcontext->formats_list =
                g_list_append (xcontext->formats_list, format);
          }
        }
        break;
      }
      case XvYUV:
        format_caps = gst_caps_new_simple ("video/x-raw-yuv",
            "format", GST_TYPE_FOURCC, formats[i].id,
            "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "framerate", GST_TYPE_DOUBLE_RANGE, 1.0, 100.0, NULL);
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    gst_caps_append (caps, format_caps);
  }

  if (formats)
    XFree (formats);

  GST_DEBUG ("Generated the following caps: %" GST_PTR_FORMAT, caps);

  if (gst_caps_is_empty (caps)) {
    gst_caps_unref (caps);
    XvUngrabPort (xcontext->disp, xcontext->xv_port_id, 0);
    GST_ELEMENT_ERROR (xvimagesink, STREAM, WRONG_TYPE, (NULL),
        ("No supported format found"));
    return NULL;
  }

  return caps;
}

/* This function calculates the pixel aspect ratio based on the properties
 * in the xcontext structure and stores it there. */
static void
gst_xvimagesink_calculate_pixel_aspect_ratio (GstXContext * xcontext)
{
  gint par[][2] = {
    {1, 1},                     /* regular screen */
    {16, 15},                   /* PAL TV */
    {11, 10},                   /* 525 line Rec.601 video */
    {54, 59}                    /* 625 line Rec.601 video */
  };
  gint i;
  gint index;
  gdouble ratio;
  gdouble delta;

#define DELTA(idx) (ABS (ratio - ((gdouble) par[idx][0] / par[idx][1])))

  /* first calculate the "real" ratio based on the X values;
   * which is the "physical" w/h divided by the w/h in pixels of the display */
  ratio = (gdouble) (xcontext->widthmm * xcontext->height)
      / (xcontext->heightmm * xcontext->width);

  /* DirectFB's X in 720x576 reports the physical dimensions wrong, so
   * override here */
  if (xcontext->width == 720 && xcontext->height == 576) {
    ratio = 4.0 * 576 / (3.0 * 720);
  }
  GST_DEBUG ("calculated pixel aspect ratio: %f", ratio);
  /* now find the one from par[][2] with the lowest delta to the real one */
  delta = DELTA (0);
  index = 0;

  for (i = 1; i < sizeof (par) / (sizeof (gint) * 2); ++i) {
    gdouble this_delta = DELTA (i);

    if (this_delta < delta) {
      index = i;
      delta = this_delta;
    }
  }

  GST_DEBUG ("Decided on index %d (%d/%d)", index,
      par[index][0], par[index][1]);

  g_free (xcontext->par);
  xcontext->par = g_new0 (GValue, 1);
  g_value_init (xcontext->par, GST_TYPE_FRACTION);
  gst_value_set_fraction (xcontext->par, par[index][0], par[index][1]);
  GST_DEBUG ("set xcontext PAR to %d/%d",
      gst_value_get_fraction_numerator (xcontext->par),
      gst_value_get_fraction_denominator (xcontext->par));
}

/* This function gets the X Display and global info about it. Everything is
   stored in our object and will be cleaned when the object is disposed. Note
   here that caps for supported format are generated without any window or
   image creation */
static GstXContext *
gst_xvimagesink_xcontext_get (GstXvImageSink * xvimagesink)
{
  GstXContext *xcontext = NULL;
  XPixmapFormatValues *px_formats = NULL;
  gint nb_formats = 0, i, j, N_attr;
  XvAttribute *xv_attr;
  char *channels[4] = { "XV_HUE", "XV_SATURATION",
    "XV_BRIGHTNESS", "XV_CONTRAST"
  };

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  xcontext = g_new0 (GstXContext, 1);
  xcontext->im_format = 0;

  g_mutex_lock (xvimagesink->x_lock);

  xcontext->disp = XOpenDisplay (xvimagesink->display_name);

  if (!xcontext->disp) {
    g_mutex_unlock (xvimagesink->x_lock);
    g_free (xcontext);
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE, (NULL),
        ("Could not open display"));
    return NULL;
  }

  xcontext->screen = DefaultScreenOfDisplay (xcontext->disp);
  xcontext->screen_num = DefaultScreen (xcontext->disp);
  xcontext->visual = DefaultVisual (xcontext->disp, xcontext->screen_num);
  xcontext->root = DefaultRootWindow (xcontext->disp);
  xcontext->white = XWhitePixel (xcontext->disp, xcontext->screen_num);
  xcontext->black = XBlackPixel (xcontext->disp, xcontext->screen_num);
  xcontext->depth = DefaultDepthOfScreen (xcontext->screen);

  xcontext->width = DisplayWidth (xcontext->disp, xcontext->screen_num);
  xcontext->height = DisplayHeight (xcontext->disp, xcontext->screen_num);
  xcontext->widthmm = DisplayWidthMM (xcontext->disp, xcontext->screen_num);
  xcontext->heightmm = DisplayHeightMM (xcontext->disp, xcontext->screen_num);

  GST_DEBUG_OBJECT (xvimagesink, "X reports %dx%d pixels and %d mm x %d mm",
      xcontext->width, xcontext->height, xcontext->widthmm, xcontext->heightmm);


  gst_xvimagesink_calculate_pixel_aspect_ratio (xcontext);
  /* We get supported pixmap formats at supported depth */
  px_formats = XListPixmapFormats (xcontext->disp, &nb_formats);

  if (!px_formats) {
    XCloseDisplay (xcontext->disp);
    g_mutex_unlock (xvimagesink->x_lock);
    g_free (xcontext);
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, SETTINGS, (NULL),
        ("Could not get pixel formats"));
    return NULL;
  }

  /* We get bpp value corresponding to our running depth */
  for (i = 0; i < nb_formats; i++) {
    if (px_formats[i].depth == xcontext->depth)
      xcontext->bpp = px_formats[i].bits_per_pixel;
  }

  XFree (px_formats);

  xcontext->endianness =
      (ImageByteOrder (xcontext->disp) ==
      LSBFirst) ? G_LITTLE_ENDIAN : G_BIG_ENDIAN;

  /* our caps system handles 24/32bpp RGB as big-endian. */
  if ((xcontext->bpp == 24 || xcontext->bpp == 32) &&
      xcontext->endianness == G_LITTLE_ENDIAN) {
    xcontext->endianness = G_BIG_ENDIAN;
    xcontext->visual->red_mask = GUINT32_TO_BE (xcontext->visual->red_mask);
    xcontext->visual->green_mask = GUINT32_TO_BE (xcontext->visual->green_mask);
    xcontext->visual->blue_mask = GUINT32_TO_BE (xcontext->visual->blue_mask);
    if (xcontext->bpp == 24) {
      xcontext->visual->red_mask >>= 8;
      xcontext->visual->green_mask >>= 8;
      xcontext->visual->blue_mask >>= 8;
    }
  }

  xcontext->caps = gst_xvimagesink_get_xv_support (xvimagesink, xcontext);

  if (!xcontext->caps) {
    XCloseDisplay (xcontext->disp);
    g_mutex_unlock (xvimagesink->x_lock);
    g_free (xcontext);
    /* GST_ELEMENT_ERROR is thrown by gst_xvimagesink_get_xv_support */
    return NULL;
  }
#ifdef HAVE_XSHM
  /* Search for XShm extension support */
  if (XShmQueryExtension (xcontext->disp) &&
      gst_xvimagesink_check_xshm_calls (xcontext)) {
    xcontext->use_xshm = TRUE;
    GST_DEBUG ("xvimagesink is using XShm extension");
  } else
#endif /* HAVE_XSHM */
  {
    xcontext->use_xshm = FALSE;
    GST_DEBUG ("xvimagesink is not using XShm extension");
  }

  xv_attr = XvQueryPortAttributes (xcontext->disp,
      xcontext->xv_port_id, &N_attr);


  /* Generate the channels list */
  for (i = 0; i < (sizeof (channels) / sizeof (char *)); i++) {
    XvAttribute *matching_attr = NULL;

    if (xv_attr != NULL) {
      for (j = 0; j < N_attr && matching_attr == NULL; ++j)
        if (!g_ascii_strcasecmp (channels[i], xv_attr[j].name))
          matching_attr = xv_attr + j;
    }

    if (matching_attr) {
      GstColorBalanceChannel *channel;

      channel = g_object_new (GST_TYPE_COLOR_BALANCE_CHANNEL, NULL);
      channel->label = g_strdup (channels[i]);
      channel->min_value = matching_attr ? matching_attr->min_value : -1000;
      channel->max_value = matching_attr ? matching_attr->max_value : 1000;

      xcontext->channels_list = g_list_append (xcontext->channels_list,
          channel);

      /* If the colorbalance settings have not been touched we get Xv values
         as defaults and update our internal variables */
      if (!xvimagesink->cb_changed) {
        gint val;

        XvGetPortAttribute (xcontext->disp, xcontext->xv_port_id,
            XInternAtom (xcontext->disp, channel->label, 1), &val);
        /* Normalize val to [-1000, 1000] */
        val = -1000 + 2000 * (val - channel->min_value) /
            (channel->max_value - channel->min_value);

        if (!g_ascii_strcasecmp (channels[i], "XV_HUE"))
          xvimagesink->hue = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_SATURATION"))
          xvimagesink->saturation = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_BRIGHTNESS"))
          xvimagesink->brightness = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_CONTRAST"))
          xvimagesink->contrast = val;
      }
    }
  }

  if (xv_attr)
    XFree (xv_attr);

  g_mutex_unlock (xvimagesink->x_lock);

  return xcontext;
}

/* This function cleans the X context. Closing the Display, releasing the XV
   port and unrefing the caps for supported formats. */
static void
gst_xvimagesink_xcontext_clear (GstXvImageSink * xvimagesink)
{
  GList *formats_list, *channels_list;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));
  g_return_if_fail (xvimagesink->xcontext != NULL);

  formats_list = xvimagesink->xcontext->formats_list;

  while (formats_list) {
    GstXvImageFormat *format = formats_list->data;

    gst_caps_unref (format->caps);
    g_free (format);
    formats_list = g_list_next (formats_list);
  }

  if (xvimagesink->xcontext->formats_list)
    g_list_free (xvimagesink->xcontext->formats_list);

  channels_list = xvimagesink->xcontext->channels_list;

  while (channels_list) {
    GstColorBalanceChannel *channel = channels_list->data;

    g_object_unref (channel);
    channels_list = g_list_next (channels_list);
  }

  if (xvimagesink->xcontext->channels_list)
    g_list_free (xvimagesink->xcontext->channels_list);

  gst_caps_unref (xvimagesink->xcontext->caps);
  g_free (xvimagesink->xcontext->par);

  g_mutex_lock (xvimagesink->x_lock);

  XvUngrabPort (xvimagesink->xcontext->disp,
      xvimagesink->xcontext->xv_port_id, 0);

  XCloseDisplay (xvimagesink->xcontext->disp);

  g_mutex_unlock (xvimagesink->x_lock);

  g_free (xvimagesink->xcontext);
  xvimagesink->xcontext = NULL;
}

static void
gst_xvimagesink_imagepool_clear (GstXvImageSink * xvimagesink)
{
  g_mutex_lock (xvimagesink->pool_lock);

  while (xvimagesink->image_pool) {
    GstXvImageBuffer *xvimage = xvimagesink->image_pool->data;

    xvimagesink->image_pool = g_slist_delete_link (xvimagesink->image_pool,
        xvimagesink->image_pool);
    gst_xvimage_buffer_free (xvimage);
  }

  g_mutex_unlock (xvimagesink->pool_lock);
}

/* Element stuff */

#if 0
static GstCaps *
gst_xvimagesink_fixate (GstPad * pad, const GstCaps * caps)
{
  GstStructure *structure;
  GstCaps *newcaps;

  if (gst_caps_get_size (caps) > 1)
    return NULL;

  newcaps = gst_caps_copy (caps);
  structure = gst_caps_get_structure (newcaps, 0);

  if (gst_caps_structure_fixate_field_nearest_int (structure, "width", 320)) {
    return newcaps;
  }
  if (gst_caps_structure_fixate_field_nearest_int (structure, "height", 240)) {
    return newcaps;
  }
  if (gst_caps_structure_fixate_field_nearest_double (structure, "framerate",
          30.0)) {
    return newcaps;
  }

  gst_caps_unref (newcaps);
  return NULL;
}
#endif

/* This function tries to get a format matching with a given caps in the
   supported list of formats we generated in gst_xvimagesink_get_xv_support */
static gint
gst_xvimagesink_get_fourcc_from_caps (GstXvImageSink * xvimagesink,
    GstCaps * caps)
{
  GList *list = NULL;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), 0);

  list = xvimagesink->xcontext->formats_list;

  while (list) {
    GstXvImageFormat *format = list->data;

    if (format) {
      GstCaps *icaps = NULL;

      icaps = gst_caps_intersect (caps, format->caps);
      if (!gst_caps_is_empty (icaps))
        return format->format;
    }
    list = g_list_next (list);
  }

  return 0;
}

static GstCaps *
gst_xvimagesink_getcaps (GstBaseSink * bsink)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (bsink);

  if (xvimagesink->xcontext)
    return gst_caps_ref (xvimagesink->xcontext->caps);

  return
      gst_caps_copy (gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD
          (xvimagesink)));
}

static gboolean
gst_xvimagesink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstXvImageSink *xvimagesink;
  GstStructure *structure;
  guint32 im_format = 0;
  gboolean ret;
  gint video_width, video_height;
  gint video_par_n, video_par_d;        /* video's PAR */
  gint display_par_n, display_par_d;    /* display's PAR */
  GValue display_ratio = { 0, };        /* display w/h ratio */
  const GValue *caps_par;
  gint num, den;

  xvimagesink = GST_XVIMAGESINK (bsink);

  GST_DEBUG_OBJECT (xvimagesink,
      "In setcaps. Possible caps %" GST_PTR_FORMAT ", setting caps %"
      GST_PTR_FORMAT, xvimagesink->xcontext->caps, caps);

  structure = gst_caps_get_structure (caps, 0);
  ret = gst_structure_get_int (structure, "width", &video_width);
  ret &= gst_structure_get_int (structure, "height", &video_height);
  ret &= gst_structure_get_double (structure, "framerate",
      &xvimagesink->framerate);
  if (!ret)
    return FALSE;

  xvimagesink->video_width = video_width;
  xvimagesink->video_height = video_height;
  if (!gst_structure_get_fourcc (structure, "format", &im_format)) {
    im_format =
        gst_xvimagesink_get_fourcc_from_caps (xvimagesink,
        gst_caps_copy (caps));
  }
  if (im_format == 0) {
    return FALSE;
  }

  /* get aspect ratio from caps if it's present, and
   * convert video width and height to a display width and height
   * using wd / hd = wv / hv * PARv / PARd
   * the ratio wd / hd will be stored in display_ratio */
  g_value_init (&display_ratio, GST_TYPE_FRACTION);

  /* get video's PAR */
  caps_par = gst_structure_get_value (structure, "pixel-aspect-ratio");
  if (caps_par) {
    video_par_n = gst_value_get_fraction_numerator (caps_par);
    video_par_d = gst_value_get_fraction_denominator (caps_par);
  } else {
    video_par_n = 1;
    video_par_d = 1;
  }
  /* get display's PAR */
  if (xvimagesink->par) {
    display_par_n = gst_value_get_fraction_numerator (xvimagesink->par);
    display_par_d = gst_value_get_fraction_denominator (xvimagesink->par);
  } else {
    display_par_n = 1;
    display_par_d = 1;
  }

  gst_value_set_fraction (&display_ratio,
      video_width * video_par_n * display_par_d,
      video_height * video_par_d * display_par_n);

  num = gst_value_get_fraction_numerator (&display_ratio);
  den = gst_value_get_fraction_denominator (&display_ratio);
  GST_DEBUG_OBJECT (xvimagesink,
      "video width/height: %dx%d, calculated display ratio: %d/%d",
      video_width, video_height, num, den);

  /* now find a width x height that respects this display ratio.
   * prefer those that have one of w/h the same as the incoming video
   * using wd / hd = num / den */

  /* start with same height, because of interlaced video */
  /* check hd / den is an integer scale factor, and scale wd with the PAR */
  if (video_height % den == 0) {
    GST_DEBUG_OBJECT (xvimagesink, "keeping video height");
    GST_VIDEO_SINK_WIDTH (xvimagesink) = video_height * num / den;
    GST_VIDEO_SINK_HEIGHT (xvimagesink) = video_height;
  } else if (video_width % num == 0) {
    GST_DEBUG_OBJECT (xvimagesink, "keeping video width");
    GST_VIDEO_SINK_WIDTH (xvimagesink) = video_width;
    GST_VIDEO_SINK_HEIGHT (xvimagesink) = video_width * den / num;
  } else {
    GST_DEBUG_OBJECT (xvimagesink, "approximating while keeping video height");
    GST_VIDEO_SINK_WIDTH (xvimagesink) = video_height * num / den;
    GST_VIDEO_SINK_HEIGHT (xvimagesink) = video_height;
  }
  GST_DEBUG_OBJECT (xvimagesink, "scaling to %dx%d",
      GST_VIDEO_SINK_WIDTH (xvimagesink), GST_VIDEO_SINK_HEIGHT (xvimagesink));

  /* Creating our window and our image with the display size in pixels */
  g_assert (GST_VIDEO_SINK_WIDTH (xvimagesink) > 0);
  g_assert (GST_VIDEO_SINK_HEIGHT (xvimagesink) > 0);
  if (!xvimagesink->xwindow)
    xvimagesink->xwindow = gst_xvimagesink_xwindow_new (xvimagesink,
        GST_VIDEO_SINK_WIDTH (xvimagesink),
        GST_VIDEO_SINK_HEIGHT (xvimagesink));
  else {
    if (xvimagesink->xwindow->internal)
      gst_xvimagesink_xwindow_resize (xvimagesink, xvimagesink->xwindow,
          GST_VIDEO_SINK_WIDTH (xvimagesink),
          GST_VIDEO_SINK_HEIGHT (xvimagesink));
  }

  /* We renew our xvimage only if size or format changed;
   * the xvimage is the same size as the video pixel size */
  if ((xvimagesink->xvimage) &&
      ((im_format != xvimagesink->xvimage->im_format) ||
          (video_width != xvimagesink->xvimage->width) ||
          (video_height != xvimagesink->xvimage->height))) {
    GST_DEBUG_OBJECT (xvimagesink,
        "old format " GST_FOURCC_FORMAT ", new format " GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (xvimagesink->xcontext->im_format),
        GST_FOURCC_ARGS (im_format));
    GST_DEBUG_OBJECT (xvimagesink, "renewing xvimage");
    gst_xvimage_buffer_free (xvimagesink->xvimage);
    xvimagesink->xvimage = NULL;
  }

  xvimagesink->xcontext->im_format = im_format;

  gst_x_overlay_got_desired_size (GST_X_OVERLAY (xvimagesink),
      GST_VIDEO_SINK_WIDTH (xvimagesink), GST_VIDEO_SINK_HEIGHT (xvimagesink));

  return TRUE;
}

static GstStateChangeReturn
gst_xvimagesink_change_state (GstElement * element, GstStateChange transition)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      /* Initializing the XContext */
      if (!xvimagesink->xcontext &&
          !(xvimagesink->xcontext = gst_xvimagesink_xcontext_get (xvimagesink)))
        return GST_STATE_CHANGE_FAILURE;
      /* update object's par with calculated one if not set yet */
      if (!xvimagesink->par) {
        xvimagesink->par = g_new0 (GValue, 1);
        gst_value_init_and_copy (xvimagesink->par, xvimagesink->xcontext->par);
        GST_DEBUG_OBJECT (xvimagesink, "set calculated PAR on object's PAR");
      }
      /* call XSynchronize with the current value of synchronous */
      GST_DEBUG_OBJECT (xvimagesink, "XSynchronize called with %s",
          xvimagesink->synchronous ? "TRUE" : "FALSE");
      XSynchronize (xvimagesink->xcontext->disp, xvimagesink->synchronous);
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (xvimagesink->xwindow)
        gst_xvimagesink_xwindow_clear (xvimagesink, xvimagesink->xwindow);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      xvimagesink->framerate = 0;
      GST_VIDEO_SINK_WIDTH (xvimagesink) = 0;
      GST_VIDEO_SINK_HEIGHT (xvimagesink) = 0;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (xvimagesink->xvimage) {
        gst_xvimage_buffer_free (xvimagesink->xvimage);
        xvimagesink->xvimage = NULL;
      }

      if (xvimagesink->image_pool)
        gst_xvimagesink_imagepool_clear (xvimagesink);

      if (xvimagesink->xwindow) {
        gst_xvimagesink_xwindow_destroy (xvimagesink, xvimagesink->xwindow);
        xvimagesink->xwindow = NULL;
      }

      if (xvimagesink->xcontext) {
        gst_xvimagesink_xcontext_clear (xvimagesink);
        xvimagesink->xcontext = NULL;
      }
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_xvimagesink_get_times (GstBaseSink * bsink, GstBuffer * buf,
    GstClockTime * start, GstClockTime * end)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (bsink);

  if (GST_BUFFER_TIMESTAMP_IS_VALID (buf)) {
    *start = GST_BUFFER_TIMESTAMP (buf);
    if (GST_BUFFER_DURATION_IS_VALID (buf)) {
      *end = *start + GST_BUFFER_DURATION (buf);
    } else {
      if (xvimagesink->framerate > 0) {
        *end = *start + GST_SECOND / xvimagesink->framerate;
      }
    }
  }
}

static GstFlowReturn
gst_xvimagesink_show_frame (GstBaseSink * bsink, GstBuffer * buf)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (bsink);

  /* If this buffer has been allocated using our buffer management we simply
     put the ximage which is in the PRIVATE pointer */
  if (GST_IS_XVIMAGE_BUFFER (buf)) {
    GST_DEBUG ("fast put of bufferpool buffer");
    gst_xvimagesink_xvimage_put (xvimagesink, GST_XVIMAGE_BUFFER (buf));
  } else {
    GST_DEBUG ("slow copy into bufferpool buffer");
    /* Else we have to copy the data into our private image, */
    /* if we have one... */
    if (!xvimagesink->xvimage) {
      GST_DEBUG_OBJECT (xvimagesink, "creating our xvimage");

      xvimagesink->xvimage = gst_xvimagesink_xvimage_new (xvimagesink,
          xvimagesink->video_width, xvimagesink->video_height);

      if (!xvimagesink->xvimage)
        goto no_image;
    }

    memcpy (xvimagesink->xvimage->xvimage->data,
        GST_BUFFER_DATA (buf),
        MIN (GST_BUFFER_SIZE (buf), xvimagesink->xvimage->size));

    gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->xvimage);
  }

  gst_xvimagesink_handle_xevents (xvimagesink,
      GST_VIDEO_SINK_PAD (xvimagesink));

  return GST_FLOW_OK;

  /* ERRORS */
no_image:
  {
    /* No image available. That's very bad ! */
    GST_DEBUG ("could not create image");
    GST_ELEMENT_ERROR (xvimagesink, CORE, NEGOTIATION, (NULL),
        ("Failed creating an XvImage in xvimagesink chain function."));
    return GST_FLOW_ERROR;
  }
}

/* Buffer management */

#if 0
static void
gst_xvimagesink_buffer_free (GstBuffer * buffer)
{
}
#endif

static GstFlowReturn
gst_xvimagesink_buffer_alloc (GstBaseSink * bsink, guint64 offset, guint size,
    GstCaps * caps, GstBuffer ** buf)
{
  GstXvImageSink *xvimagesink;
  GstXvImageBuffer *xvimage = NULL;

  xvimagesink = GST_XVIMAGESINK (bsink);

  /* FIXME, we should just parse the caps, and provide a buffer in this format,
   * we should not just reconfigure ourselves yet */
  if (caps && caps != GST_PAD_CAPS (GST_VIDEO_SINK_PAD (xvimagesink))) {
    if (!gst_xvimagesink_setcaps (bsink, caps)) {
      return GST_FLOW_NOT_NEGOTIATED;
    }
  }

  g_mutex_lock (xvimagesink->pool_lock);

  /* Walking through the pool cleaning unusable images and searching for a
     suitable one */
  while (xvimagesink->image_pool) {
    xvimage = xvimagesink->image_pool->data;

    if (xvimage) {
      /* Removing from the pool */
      xvimagesink->image_pool = g_slist_delete_link (xvimagesink->image_pool,
          xvimagesink->image_pool);

      /* We check for geometry or image format changes */
      if ((xvimage->width != xvimagesink->video_width) ||
          (xvimage->height != xvimagesink->video_height) ||
          (xvimage->im_format != xvimagesink->xcontext->im_format)) {
        /* This image is unusable. Destroying... */
        gst_xvimage_buffer_free (xvimage);
        xvimage = NULL;
      } else {
        /* We found a suitable image */
        GST_DEBUG_OBJECT (xvimagesink, "found usable image in pool");
        break;
      }
    }
  }

  g_mutex_unlock (xvimagesink->pool_lock);

  if (!xvimage) {
    /* We found no suitable image in the pool. Creating... */
    GST_DEBUG_OBJECT (xvimagesink, "no usable image in pool, creating xvimage");
    xvimage = gst_xvimagesink_xvimage_new (xvimagesink,
        xvimagesink->video_width, xvimagesink->video_height);
  }
  if (xvimage) {
    gst_buffer_set_caps (GST_BUFFER (xvimage), caps);
  }
  *buf = GST_BUFFER (xvimage);

  return GST_FLOW_OK;
}

/* Interfaces stuff */

static gboolean
gst_xvimagesink_interface_supported (GstImplementsInterface * iface, GType type)
{
  g_assert (type == GST_TYPE_NAVIGATION ||
      type == GST_TYPE_X_OVERLAY || type == GST_TYPE_COLOR_BALANCE);
  return TRUE;
}

static void
gst_xvimagesink_interface_init (GstImplementsInterfaceClass * klass)
{
  klass->supported = gst_xvimagesink_interface_supported;
}

#if 0
/*
 * This function is called with the stream-lock held
 */
static void
gst_xvimagesink_send_pending_navigation (GstXvImageSink * xvimagesink)
{
  GSList *cur;
  GSList *pend_events;

  g_mutex_lock (xvimagesink->nav_lock);
  pend_events = xvimagesink->pend_nav_events;
  xvimagesink->pend_nav_events = NULL;
  g_mutex_unlock (xvimagesink->nav_lock);

  cur = pend_events;
  while (cur) {
    GstEvent *event = cur->data;
    GstStructure *structure;
    double x, y;

    if (event) {
      structure = event->event_data.structure.structure;

      if (!GST_PAD_PEER (GST_VIDEO_SINK_PAD (xvimagesink))) {
        gst_event_unref (event);
        cur = g_slist_next (cur);
        continue;
      }

      /* Converting pointer coordinates to the non scaled geometry */
      if (gst_structure_get_double (structure, "pointer_x", &x)) {
        x *= xvimagesink->video_width;
        x /= xvimagesink->xwindow->width;
        gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE, x, NULL);
      }
      if (gst_structure_get_double (structure, "pointer_y", &y)) {
        y *= xvimagesink->video_height;
        y /= xvimagesink->xwindow->height;
        gst_structure_set (structure, "pointer_y", G_TYPE_DOUBLE, y, NULL);
      }

      gst_pad_send_event (gst_pad_get_peer (GST_VIDEO_SINK_PAD (xvimagesink)),
          event);
    }
    cur = g_slist_next (cur);
  }

  g_slist_free (pend_events);
}
#endif

static void
gst_xvimagesink_navigation_send_event (GstNavigation * navigation,
    GstStructure * structure)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (navigation);
  GstEvent *event;
  double x, y;

  event = gst_event_new_custom (GST_EVENT_NAVIGATION, structure);

  /* Converting pointer coordinates to the non scaled geometry */
  if (gst_structure_get_double (structure, "pointer_x", &x)) {
    x *= GST_VIDEO_SINK_WIDTH (xvimagesink);
    x /= xvimagesink->xwindow->width;
    gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE, x, NULL);
  }
  if (gst_structure_get_double (structure, "pointer_y", &y)) {
    y *= GST_VIDEO_SINK_HEIGHT (xvimagesink);
    y /= xvimagesink->xwindow->height;
    gst_structure_set (structure, "pointer_y", G_TYPE_DOUBLE, y, NULL);
  }

  gst_pad_send_event (gst_pad_get_peer (GST_VIDEO_SINK_PAD (xvimagesink)),
      event);
}

static void
gst_xvimagesink_navigation_init (GstNavigationInterface * iface)
{
  iface->send_event = gst_xvimagesink_navigation_send_event;
}

static void
gst_xvimagesink_set_xwindow_id (GstXOverlay * overlay, XID xwindow_id)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);
  GstXWindow *xwindow = NULL;
  XWindowAttributes attr;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  /* If we already use that window return */
  if (xvimagesink->xwindow && (xwindow_id == xvimagesink->xwindow->win))
    return;

  /* If the element has not initialized the X11 context try to do so */
  if (!xvimagesink->xcontext &&
      !(xvimagesink->xcontext = gst_xvimagesink_xcontext_get (xvimagesink)))
    /* we have thrown a GST_ELEMENT_ERROR now */
    return;

  gst_xvimagesink_update_colorbalance (xvimagesink);

  /* Clear image pool as the images are unusable anyway */
  gst_xvimagesink_imagepool_clear (xvimagesink);

  /* Clear the xvimage */
  if (xvimagesink->xvimage) {
    gst_xvimage_buffer_free (xvimagesink->xvimage);
    xvimagesink->xvimage = NULL;
  }

  /* If a window is there already we destroy it */
  if (xvimagesink->xwindow) {
    gst_xvimagesink_xwindow_destroy (xvimagesink, xvimagesink->xwindow);
    xvimagesink->xwindow = NULL;
  }

  /* If the xid is 0 we go back to an internal window */
  if (xwindow_id == 0) {
    /* If no width/height caps nego did not happen window will be created
       during caps nego then */
    if (GST_VIDEO_SINK_WIDTH (xvimagesink)
        && GST_VIDEO_SINK_HEIGHT (xvimagesink)) {
      xwindow =
          gst_xvimagesink_xwindow_new (xvimagesink,
          GST_VIDEO_SINK_WIDTH (xvimagesink),
          GST_VIDEO_SINK_HEIGHT (xvimagesink));
    }
  } else {
    xwindow = g_new0 (GstXWindow, 1);

    xwindow->win = xwindow_id;

    /* We get window geometry, set the event we want to receive,
       and create a GC */
    g_mutex_lock (xvimagesink->x_lock);
    XGetWindowAttributes (xvimagesink->xcontext->disp, xwindow->win, &attr);
    xwindow->width = attr.width;
    xwindow->height = attr.height;
    xwindow->internal = FALSE;
    XSelectInput (xvimagesink->xcontext->disp, xwindow->win, ExposureMask |
        StructureNotifyMask | PointerMotionMask | KeyPressMask |
        KeyReleaseMask);

    xwindow->gc = XCreateGC (xvimagesink->xcontext->disp,
        xwindow->win, 0, NULL);
    g_mutex_unlock (xvimagesink->x_lock);
  }

  if (xwindow)
    xvimagesink->xwindow = xwindow;
}

static void
gst_xvimagesink_get_desired_size (GstXOverlay * overlay,
    guint * width, guint * height)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);

  *width = GST_VIDEO_SINK_WIDTH (xvimagesink);
  *height = GST_VIDEO_SINK_HEIGHT (xvimagesink);
}

static void
gst_xvimagesink_expose (GstXOverlay * overlay)
{
  XWindowAttributes attr;
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);

  if (!xvimagesink->xwindow)
    return;

  /* Update the window geometry */
  g_mutex_lock (xvimagesink->x_lock);
  XGetWindowAttributes (xvimagesink->xcontext->disp,
      xvimagesink->xwindow->win, &attr);
  g_mutex_unlock (xvimagesink->x_lock);

  xvimagesink->xwindow->width = attr.width;
  xvimagesink->xwindow->height = attr.height;

  if (xvimagesink->cur_image) {
    gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->cur_image);
  }
}

static void
gst_xvimagesink_xoverlay_init (GstXOverlayClass * iface)
{
  iface->set_xwindow_id = gst_xvimagesink_set_xwindow_id;
  iface->get_desired_size = gst_xvimagesink_get_desired_size;
  iface->expose = gst_xvimagesink_expose;
}

static const GList *
gst_xvimagesink_colorbalance_list_channels (GstColorBalance * balance)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (balance);

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  if (xvimagesink->xcontext)
    return xvimagesink->xcontext->channels_list;
  else
    return NULL;
}

static void
gst_xvimagesink_colorbalance_set_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel, gint value)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (balance);

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));
  g_return_if_fail (channel->label != NULL);

  xvimagesink->cb_changed = TRUE;

  /* Normalize val to [-1000, 1000] */
  value = -1000 + 2000 * (value - channel->min_value) /
      (channel->max_value - channel->min_value);

  if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
    xvimagesink->hue = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
    xvimagesink->saturation = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
    xvimagesink->contrast = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
    xvimagesink->brightness = value;
  } else {
    g_warning ("got an unknown channel %s", channel->label);
    return;
  }

  gst_xvimagesink_update_colorbalance (xvimagesink);
}

static gint
gst_xvimagesink_colorbalance_get_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (balance);
  gint value = 0;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), 0);
  g_return_val_if_fail (channel->label != NULL, 0);

  if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
    value = xvimagesink->hue;
  } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
    value = xvimagesink->saturation;
  } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
    value = xvimagesink->contrast;
  } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
    value = xvimagesink->brightness;
  } else {
    g_warning ("got an unknown channel %s", channel->label);
  }

  /* Normalize val to [channel->min_value, channel->max_value] */
  value = channel->min_value + (channel->max_value - channel->min_value) *
      (value + 1000) / 2000;

  return value;
}

static void
gst_xvimagesink_colorbalance_init (GstColorBalanceClass * iface)
{
  GST_COLOR_BALANCE_TYPE (iface) = GST_COLOR_BALANCE_HARDWARE;
  iface->list_channels = gst_xvimagesink_colorbalance_list_channels;
  iface->set_value = gst_xvimagesink_colorbalance_set_value;
  iface->get_value = gst_xvimagesink_colorbalance_get_value;
}

/* =========================================== */
/*                                             */
/*              Init & Class init              */
/*                                             */
/* =========================================== */

static void
gst_xvimagesink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink;

  g_return_if_fail (GST_IS_XVIMAGESINK (object));

  xvimagesink = GST_XVIMAGESINK (object);

  switch (prop_id) {
    case ARG_HUE:
      xvimagesink->hue = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case ARG_CONTRAST:
      xvimagesink->contrast = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case ARG_BRIGHTNESS:
      xvimagesink->brightness = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case ARG_SATURATION:
      xvimagesink->saturation = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case ARG_DISPLAY:
      xvimagesink->display_name = g_strdup (g_value_get_string (value));
      break;
    case ARG_SYNCHRONOUS:
      xvimagesink->synchronous = g_value_get_boolean (value);
      if (xvimagesink->xcontext) {
        XSynchronize (xvimagesink->xcontext->disp, xvimagesink->synchronous);
        GST_DEBUG_OBJECT (xvimagesink, "XSynchronize called with %s",
            xvimagesink->synchronous ? "TRUE" : "FALSE");
      }
      break;
    case ARG_PIXEL_ASPECT_RATIO:
      g_free (xvimagesink->par);
      xvimagesink->par = g_new0 (GValue, 1);
      g_value_init (xvimagesink->par, GST_TYPE_FRACTION);
      if (!g_value_transform (value, xvimagesink->par)) {
        g_warning ("Could not transform string to aspect ratio");
        gst_value_set_fraction (xvimagesink->par, 1, 1);
      }
      GST_DEBUG_OBJECT (xvimagesink, "set PAR to %d/%d",
          gst_value_get_fraction_numerator (xvimagesink->par),
          gst_value_get_fraction_denominator (xvimagesink->par));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_xvimagesink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink;

  g_return_if_fail (GST_IS_XVIMAGESINK (object));

  xvimagesink = GST_XVIMAGESINK (object);

  switch (prop_id) {
    case ARG_HUE:
      g_value_set_int (value, xvimagesink->hue);
      break;
    case ARG_CONTRAST:
      g_value_set_int (value, xvimagesink->contrast);
      break;
    case ARG_BRIGHTNESS:
      g_value_set_int (value, xvimagesink->brightness);
      break;
    case ARG_SATURATION:
      g_value_set_int (value, xvimagesink->saturation);
      break;
    case ARG_DISPLAY:
      g_value_set_string (value, g_strdup (xvimagesink->display_name));
      break;
    case ARG_SYNCHRONOUS:
      g_value_set_boolean (value, xvimagesink->synchronous);
      break;
    case ARG_PIXEL_ASPECT_RATIO:
      if (xvimagesink->par)
        g_value_transform (xvimagesink->par, value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Finalize is called only once, dispose can be called multiple times.
 * We use mutexes and don't reset stuff to NULL here so let's register
 * as a finalize. */
static void
gst_xvimagesink_finalize (GObject * object)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (object);

  if (xvimagesink->display_name) {
    g_free (xvimagesink->display_name);
    xvimagesink->display_name = NULL;
  }

  if (xvimagesink->par) {
    g_free (xvimagesink->par);
    xvimagesink->par = NULL;
  }
  if (xvimagesink->x_lock) {
    g_mutex_free (xvimagesink->x_lock);
    xvimagesink->x_lock = NULL;
  }
  if (xvimagesink->pool_lock) {
    g_mutex_free (xvimagesink->pool_lock);
    xvimagesink->pool_lock = NULL;
  }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_xvimagesink_init (GstXvImageSink * xvimagesink)
{
  xvimagesink->display_name = NULL;
  xvimagesink->xcontext = NULL;
  xvimagesink->xwindow = NULL;
  xvimagesink->xvimage = NULL;
  xvimagesink->cur_image = NULL;

  xvimagesink->hue = xvimagesink->saturation = 0;
  xvimagesink->contrast = xvimagesink->brightness = 0;
  xvimagesink->cb_changed = FALSE;

  xvimagesink->framerate = 0;
  xvimagesink->video_width = 0;
  xvimagesink->video_height = 0;

  xvimagesink->x_lock = g_mutex_new ();

  xvimagesink->image_pool = NULL;
  xvimagesink->pool_lock = g_mutex_new ();

  xvimagesink->synchronous = FALSE;
  xvimagesink->par = NULL;
}

static void
gst_xvimagesink_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details (element_class, &gst_xvimagesink_details);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_xvimagesink_sink_template_factory));
}

static void
gst_xvimagesink_class_init (GstXvImageSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  parent_class = g_type_class_ref (GST_TYPE_VIDEO_SINK);

  gobject_class->set_property = gst_xvimagesink_set_property;
  gobject_class->get_property = gst_xvimagesink_get_property;

  g_object_class_install_property (gobject_class, ARG_CONTRAST,
      g_param_spec_int ("contrast", "Contrast", "The contrast of the video",
          -1000, 1000, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_BRIGHTNESS,
      g_param_spec_int ("brightness", "Brightness",
          "The brightness of the video", -1000, 1000, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_HUE,
      g_param_spec_int ("hue", "Hue", "The hue of the video", -1000, 1000, 0,
          G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_SATURATION,
      g_param_spec_int ("saturation", "Saturation",
          "The saturation of the video", -1000, 1000, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_DISPLAY,
      g_param_spec_string ("display", "Display", "X Display name", NULL,
          G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_SYNCHRONOUS,
      g_param_spec_boolean ("synchronous", "Synchronous",
          "When enabled, runs "
          "the X display in synchronous mode. (used only for debugging)", FALSE,
          G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_PIXEL_ASPECT_RATIO,
      g_param_spec_string ("pixel-aspect-ratio", "Pixel Aspect Ratio",
          "The pixel aspect ratio of the device", "1/1", G_PARAM_READWRITE));

  gobject_class->finalize = gst_xvimagesink_finalize;

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_xvimagesink_change_state);

  //gstbasesink_class->get_template = GST_DEBUG_FUNCPTR (gst_xvimagesink_get_template);
  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_xvimagesink_getcaps);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_xvimagesink_setcaps);
  gstbasesink_class->buffer_alloc =
      GST_DEBUG_FUNCPTR (gst_xvimagesink_buffer_alloc);
  gstbasesink_class->get_times = GST_DEBUG_FUNCPTR (gst_xvimagesink_get_times);
  gstbasesink_class->preroll = GST_DEBUG_FUNCPTR (gst_xvimagesink_show_frame);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_xvimagesink_show_frame);
}

/* ============================================================= */
/*                                                               */
/*                       Public Methods                          */
/*                                                               */
/* ============================================================= */

/* =========================================== */
/*                                             */
/*          Object typing & Creation           */
/*                                             */
/* =========================================== */

GType
gst_xvimagesink_get_type (void)
{
  static GType xvimagesink_type = 0;

  if (!xvimagesink_type) {
    static const GTypeInfo xvimagesink_info = {
      sizeof (GstXvImageSinkClass),
      gst_xvimagesink_base_init,
      NULL,
      (GClassInitFunc) gst_xvimagesink_class_init,
      NULL,
      NULL,
      sizeof (GstXvImageSink),
      0,
      (GInstanceInitFunc) gst_xvimagesink_init,
    };
    static const GInterfaceInfo iface_info = {
      (GInterfaceInitFunc) gst_xvimagesink_interface_init,
      NULL,
      NULL,
    };
    static const GInterfaceInfo navigation_info = {
      (GInterfaceInitFunc) gst_xvimagesink_navigation_init,
      NULL,
      NULL,
    };
    static const GInterfaceInfo overlay_info = {
      (GInterfaceInitFunc) gst_xvimagesink_xoverlay_init,
      NULL,
      NULL,
    };
    static const GInterfaceInfo colorbalance_info = {
      (GInterfaceInitFunc) gst_xvimagesink_colorbalance_init,
      NULL,
      NULL,
    };

    xvimagesink_type = g_type_register_static (GST_TYPE_VIDEO_SINK,
        "GstXvImageSink", &xvimagesink_info, 0);

    g_type_add_interface_static (xvimagesink_type,
        GST_TYPE_IMPLEMENTS_INTERFACE, &iface_info);
    g_type_add_interface_static (xvimagesink_type, GST_TYPE_NAVIGATION,
        &navigation_info);
    g_type_add_interface_static (xvimagesink_type, GST_TYPE_X_OVERLAY,
        &overlay_info);
    g_type_add_interface_static (xvimagesink_type, GST_TYPE_COLOR_BALANCE,
        &colorbalance_info);
  }

  return xvimagesink_type;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "xvimagesink",
          GST_RANK_PRIMARY, GST_TYPE_XVIMAGESINK))
    return FALSE;

  GST_DEBUG_CATEGORY_INIT (gst_debug_xvimagesink, "xvimagesink", 0,
      "xvimagesink element");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "xvimagesink",
    "XFree86 video output plugin using Xv extension",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE, GST_ORIGIN)
