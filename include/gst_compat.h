/**
 * gst_compat.h - GStreamer version compatibility layer
 *
 * Provides helpers that work across GStreamer 1.16 (Ubuntu 20.04)
 * and GStreamer 1.20+ (Ubuntu 22.04+).
 */

#ifndef GST_COMPAT_H
#define GST_COMPAT_H

#include <gst/gst.h>

/**
 * Check if a GStreamer element has a given property at runtime.
 * Useful for feature detection (e.g. "stats" on fakesink requires >= 1.18).
 */
static inline bool
gst_element_has_property(GstElement* elem, const char* name) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(elem);
    return g_object_class_find_property(klass, name) != nullptr;
}

#endif /* GST_COMPAT_H */
