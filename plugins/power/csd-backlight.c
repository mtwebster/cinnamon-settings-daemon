#include "config.h"

#include "csd-backlight.h"
#include "gpm-common.h"

#if defined(HAVE_LOGIND) && defined(HAVE_GUDEV)
#define HAVE_SYSFS_BACKLIGHT 1
#include <gudev/gudev.h>
#endif

#define LOGIND_SESSION_PATH      "/org/freedesktop/login1/session/auto"
#define LOGIND_SESSION_INTERFACE "org.freedesktop.login1.Session"

typedef enum {
    CSD_BACKLIGHT_BACKEND_NONE,
    CSD_BACKLIGHT_BACKEND_SYSFS,
    CSD_BACKLIGHT_BACKEND_RANDR
} CsdBacklightBackend;

struct _CsdBacklight {
    GObject parent;

    GSettings *settings;
    CsdBacklightBackend backend;
    GnomeRRScreen *rr_screen;

#ifdef HAVE_SYSFS_BACKLIGHT
    GUdevClient *udev_client;
    gchar *device_name;
    gchar *device_path;
    GDBusProxy *session_proxy;
    GCancellable *cancellable;
    gint max;
    gint target;
    gint written;
    gboolean pending;
#endif
};

enum {
    CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (CsdBacklight, csd_backlight, G_TYPE_OBJECT)

static void
set_not_supported (GError **error, const gchar *message)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, message);
}

#ifdef HAVE_SYSFS_BACKLIGHT

static gint
sysfs_read_int (const gchar *device_path, const gchar *attribute)
{
    gchar *path;
    gchar *contents = NULL;
    gint value = -1;

    path = g_build_filename (device_path, attribute, NULL);
    if (g_file_get_contents (path, &contents, NULL, NULL))
        value = g_ascii_strtoll (contents, NULL, 10);

    g_free (contents);
    g_free (path);
    return value;
}

static GUdevDevice *
sysfs_find_device (GUdevClient *client)
{
    const gchar *preferred_types[] = { "firmware", "platform", "raw" };
    GUdevDevice *found = NULL;
    GList *devices;
    GList *l;
    guint i;

    devices = g_udev_client_query_by_subsystem (client, "backlight");

    for (i = 0; i < G_N_ELEMENTS (preferred_types) && found == NULL; i++) {
        for (l = devices; l != NULL; l = l->next) {
            GUdevDevice *device = l->data;

            if (g_strcmp0 (g_udev_device_get_sysfs_attr (device, "type"), preferred_types[i]) != 0)
                continue;
            if (sysfs_read_int (g_udev_device_get_sysfs_path (device), "max_brightness") <= 0)
                continue;

            found = g_object_ref (device);
            break;
        }
    }

    g_list_free_full (devices, g_object_unref);
    return found;
}

static GDBusProxy *
logind_session_proxy_new (void)
{
    GDBusProxy *proxy;
    GVariant *result;
    GError *error = NULL;

    proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                           G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                           NULL,
                                           "org.freedesktop.login1",
                                           LOGIND_SESSION_PATH,
                                           LOGIND_SESSION_INTERFACE,
                                           NULL,
                                           &error);
    if (proxy == NULL) {
        g_debug ("backlight: no logind session proxy: %s", error->message);
        g_error_free (error);
        return NULL;
    }

    /* Probe for the method: an empty subsystem is rejected by any logind
     * that implements SetBrightness, so only UnknownMethod means "absent". */
    result = g_dbus_proxy_call_sync (proxy,
                                     "SetBrightness",
                                     g_variant_new ("(ssu)", "", "", 0),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &error);
    if (result != NULL) {
        g_variant_unref (result);
        return proxy;
    }

    if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
        g_debug ("backlight: logind has no SetBrightness method");
        g_error_free (error);
        g_object_unref (proxy);
        return NULL;
    }

    g_error_free (error);
    return proxy;
}

static void sysfs_write_target (CsdBacklight *self);

static void
on_set_brightness_done (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
    CsdBacklight *self = user_data;
    GVariant *reply;
    GError *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL) {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning ("backlight: SetBrightness failed: %s", error->message);
            self->pending = FALSE;
        }
        g_error_free (error);
        return;
    }

    g_variant_unref (reply);
    self->pending = FALSE;

    if (self->target != self->written)
        sysfs_write_target (self);
}

static void
sysfs_write_target (CsdBacklight *self)
{
    if (self->pending || self->target == self->written)
        return;

    self->pending = TRUE;
    self->written = self->target;

    g_debug ("backlight: SetBrightness %s %d", self->device_name, self->written);
    g_dbus_proxy_call (self->session_proxy,
                       "SetBrightness",
                       g_variant_new ("(ssu)", "backlight", self->device_name, (guint32) self->written),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       self->cancellable,
                       on_set_brightness_done,
                       self);
}

static gint
sysfs_min_abs (CsdBacklight *self)
{
    guint min_percent = g_settings_get_uint (self->settings, "minimum-display-brightness");

    return self->max / 100 * min_percent;
}

static void
sysfs_set_raw (CsdBacklight *self, gint value)
{
    self->target = CLAMP (value, sysfs_min_abs (self), self->max);
    sysfs_write_target (self);
}

static gint
sysfs_get_percentage (CsdBacklight *self)
{
    gint min_abs = sysfs_min_abs (self);

    return ABS_TO_PERCENTAGE (min_abs, self->max, CLAMP (self->target, min_abs, self->max));
}

static gint
sysfs_step (CsdBacklight *self, gint direction)
{
    gint step = BRIGHTNESS_STEP_AMOUNT (self->max - sysfs_min_abs (self));

    sysfs_set_raw (self, self->target + direction * step);
    return sysfs_get_percentage (self);
}

static void
on_uevent (GUdevClient *client,
           const gchar *action,
           GUdevDevice *device,
           gpointer     user_data)
{
    CsdBacklight *self = user_data;
    gint value;

    if (g_strcmp0 (action, "change") != 0)
        return;
    if (g_strcmp0 (g_udev_device_get_sysfs_path (device), self->device_path) != 0)
        return;

    value = sysfs_read_int (self->device_path, "brightness");
    if (value < 0 || self->pending || value == self->target)
        return;

    g_debug ("backlight: foreign change %d -> %d", self->target, value);
    self->target = value;
    self->written = value;
    g_signal_emit (self, signals[CHANGED], 0);
}

static gboolean
sysfs_backend_init (CsdBacklight *self)
{
    const gchar *subsystems[] = { "backlight", NULL };
    GUdevDevice *device;

    self->udev_client = g_udev_client_new (subsystems);
    device = sysfs_find_device (self->udev_client);
    if (device == NULL) {
        g_debug ("backlight: no usable sysfs backlight device");
        g_clear_object (&self->udev_client);
        return FALSE;
    }

    self->session_proxy = logind_session_proxy_new ();
    if (self->session_proxy == NULL) {
        g_object_unref (device);
        g_clear_object (&self->udev_client);
        return FALSE;
    }

    self->device_name = g_strdup (g_udev_device_get_name (device));
    self->device_path = g_strdup (g_udev_device_get_sysfs_path (device));
    self->max = sysfs_read_int (self->device_path, "max_brightness");
    self->target = sysfs_read_int (self->device_path, "brightness");
    self->written = self->target;
    self->cancellable = g_cancellable_new ();
    g_object_unref (device);

    g_signal_connect (self->udev_client, "uevent", G_CALLBACK (on_uevent), self);
    return TRUE;
}

static void
sysfs_backend_dispose (CsdBacklight *self)
{
    if (self->cancellable != NULL)
        g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
    g_clear_object (&self->session_proxy);
    if (self->udev_client != NULL)
        g_signal_handlers_disconnect_by_data (self->udev_client, self);
    g_clear_object (&self->udev_client);
    g_clear_pointer (&self->device_name, g_free);
    g_clear_pointer (&self->device_path, g_free);
}

#else

static gboolean
sysfs_backend_init (CsdBacklight *self)
{
    return FALSE;
}

static void
sysfs_backend_dispose (CsdBacklight *self)
{
}

#endif /* HAVE_SYSFS_BACKLIGHT */

GnomeRROutput *
csd_backlight_find_builtin_output (GnomeRRScreen *screen)
{
    GnomeRROutput **outputs;
    guint i;

    if (screen == NULL)
        return NULL;

    outputs = gnome_rr_screen_list_outputs (screen);
    if (outputs == NULL)
        return NULL;

    for (i = 0; outputs[i] != NULL; i++) {
        if (gnome_rr_output_is_connected (outputs[i]) &&
            gnome_rr_output_is_builtin_display (outputs[i]))
            return outputs[i];
    }

    return NULL;
}

static GnomeRROutput *
randr_get_output (CsdBacklight *self, GError **error)
{
    GnomeRROutput *output = csd_backlight_find_builtin_output (self->rr_screen);

    if (output == NULL || gnome_rr_output_get_min_backlight_step (output) <= 0) {
        set_not_supported (error, "RandR backlight not available");
        return NULL;
    }

    return output;
}

static gint
randr_step (CsdBacklight *self, gint direction, GError **error)
{
    GnomeRROutput *output = randr_get_output (self, error);
    gint value;

    if (output == NULL)
        return -1;

    value = gnome_rr_output_get_backlight (output);
    value = CLAMP (value + direction * gnome_rr_output_get_min_backlight_step (output), 0, 100);
    if (!gnome_rr_output_set_backlight (output, value, error))
        return -1;

    return value;
}

void
csd_backlight_set_rr_screen (CsdBacklight *self, GnomeRRScreen *screen)
{
    g_set_object (&self->rr_screen, screen);
}

gint
csd_backlight_get_percentage (CsdBacklight *self, GError **error)
{
    GnomeRROutput *output;

#ifdef HAVE_SYSFS_BACKLIGHT
    if (self->backend == CSD_BACKLIGHT_BACKEND_SYSFS)
        return sysfs_get_percentage (self);
#endif

    output = randr_get_output (self, error);
    if (output == NULL)
        return -1;

    return gnome_rr_output_get_backlight (output);
}

gboolean
csd_backlight_set_percentage (CsdBacklight *self, guint percentage, GError **error)
{
    GnomeRROutput *output;

#ifdef HAVE_SYSFS_BACKLIGHT
    if (self->backend == CSD_BACKLIGHT_BACKEND_SYSFS) {
        gint min_abs = sysfs_min_abs (self);

        sysfs_set_raw (self, PERCENTAGE_TO_ABS (min_abs, self->max, (gint) CLAMP (percentage, 0, 100)));
        return TRUE;
    }
#endif

    output = randr_get_output (self, error);
    if (output == NULL)
        return FALSE;

    return gnome_rr_output_set_backlight (output, CLAMP (percentage, 0, 100), error);
}

gint
csd_backlight_step_up (CsdBacklight *self, GError **error)
{
#ifdef HAVE_SYSFS_BACKLIGHT
    if (self->backend == CSD_BACKLIGHT_BACKEND_SYSFS)
        return sysfs_step (self, 1);
#endif
    return randr_step (self, 1, error);
}

gint
csd_backlight_step_down (CsdBacklight *self, GError **error)
{
#ifdef HAVE_SYSFS_BACKLIGHT
    if (self->backend == CSD_BACKLIGHT_BACKEND_SYSFS)
        return sysfs_step (self, -1);
#endif
    return randr_step (self, -1, error);
}

CsdBacklight *
csd_backlight_new (GSettings *settings)
{
    CsdBacklight *self = g_object_new (CSD_TYPE_BACKLIGHT, NULL);

    self->settings = g_object_ref (settings);

    if (sysfs_backend_init (self)) {
        self->backend = CSD_BACKLIGHT_BACKEND_SYSFS;
#ifdef HAVE_SYSFS_BACKLIGHT
        g_debug ("backlight: using sysfs device %s via logind", self->device_name);
#endif
    } else {
        self->backend = CSD_BACKLIGHT_BACKEND_RANDR;
        g_debug ("backlight: using RandR via muffin");
    }

    return self;
}

static void
csd_backlight_dispose (GObject *object)
{
    CsdBacklight *self = CSD_BACKLIGHT (object);

    sysfs_backend_dispose (self);
    g_clear_object (&self->rr_screen);
    g_clear_object (&self->settings);

    G_OBJECT_CLASS (csd_backlight_parent_class)->dispose (object);
}

static void
csd_backlight_class_init (CsdBacklightClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = csd_backlight_dispose;

    signals[CHANGED] = g_signal_new ("changed",
                                     G_TYPE_FROM_CLASS (klass),
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}

static void
csd_backlight_init (CsdBacklight *self)
{
    self->backend = CSD_BACKLIGHT_BACKEND_NONE;
}
