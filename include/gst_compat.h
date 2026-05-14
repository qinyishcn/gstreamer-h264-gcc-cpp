/**
 * gst_compat.h - GStreamer version compatibility layer
 *
 * Handles API differences between GStreamer 1.16 (Ubuntu 20.04)
 * and GStreamer 1.20+ (Ubuntu 22.04+).
 *
 * Key difference: gst_bus_pop_filtered() lost its 3rd argument (timeout)
 * in GStreamer 1.20. We wrap it for cross-version compatibility.
 */

#ifndef GST_COMPAT_H
#define GST_COMPAT_H

#include <gst/gst.h>

/*
 * GStreamer 1.20 removed the timeout parameter from gst_bus_pop_filtered().
 * GST_CHECK_VERSION macro lets us pick the right signature at compile time.
 */
#if GST_CHECK_VERSION(1, 20, 0)
  /* GStreamer >= 1.20: 2-argument version */
  static inline GstMessage*
  gst_compat_bus_pop_filtered(GstBus* bus, GstMessageType types) {
      return gst_bus_pop_filtered(bus, types);
  }
#else
  /* GStreamer < 1.20 (e.g. 1.16): 3-argument version with timeout */
  static inline GstMessage*
  gst_compat_bus_pop_filtered(GstBus* bus, GstMessageType types) {
      return gst_bus_pop_filtered(bus, types, 0);
  }
#endif

#endif /* GST_COMPAT_H */
