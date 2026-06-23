#include "bg-source.h"

#define BACKGROUND_SCHEMA "org.cinnamon.desktop.background"

struct _BgSource {
    GObject     parent_instance;
    GSettings  *settings;
    BgPlacement placement;
    BgShading   shading;
    GdkRGBA     primary;
    GdkRGBA     secondary;
    double      opacity;
    char       *uri;
    char      **uri_list;
    gboolean    has_uri_list;
};

G_DEFINE_FINAL_TYPE (BgSource, bg_source, G_TYPE_OBJECT)

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static BgPlacement
nick_to_placement (const char *nick)
{
    if (g_str_equal (nick, "none"))      return BG_PLACEMENT_NONE;
    if (g_str_equal (nick, "wallpaper")) return BG_PLACEMENT_WALLPAPER;
    if (g_str_equal (nick, "centered"))  return BG_PLACEMENT_CENTERED;
    if (g_str_equal (nick, "scaled"))    return BG_PLACEMENT_SCALED;
    if (g_str_equal (nick, "stretched")) return BG_PLACEMENT_STRETCHED;
    if (g_str_equal (nick, "spanned"))   return BG_PLACEMENT_SPANNED;
    return BG_PLACEMENT_ZOOM;
}

static BgShading
nick_to_shading (const char *nick)
{
    if (g_str_equal (nick, "horizontal")) return BG_SHADING_HORIZONTAL;
    if (g_str_equal (nick, "vertical"))   return BG_SHADING_VERTICAL;
    return BG_SHADING_SOLID;
}

static void
read_settings (BgSource *self)
{
    g_autoptr(GVariant) pv = g_settings_get_value (self->settings, "picture-options");
    self->placement = nick_to_placement (g_variant_get_string (pv, NULL));

    g_autoptr(GVariant) sv = g_settings_get_value (self->settings, "color-shading-type");
    self->shading = nick_to_shading (g_variant_get_string (sv, NULL));

    g_autofree char *primary = g_settings_get_string (self->settings, "primary-color");
    gdk_rgba_parse (&self->primary, primary);

    g_autofree char *secondary = g_settings_get_string (self->settings, "secondary-color");
    gdk_rgba_parse (&self->secondary, secondary);

    self->opacity = g_settings_get_int (self->settings, "picture-opacity") / 100.0;

    g_free (self->uri);
    self->uri = g_settings_get_string (self->settings, "picture-uri");

    g_strfreev (self->uri_list);
    self->uri_list = self->has_uri_list
        ? g_settings_get_strv (self->settings, "picture-uri-list")
        : NULL;
}

static void
on_settings_changed (GSettings   *settings G_GNUC_UNUSED,
                     const char  *key,
                     BgSource    *self)
{
    g_debug ("Background setting changed: %s", key);
    read_settings (self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
bg_source_finalize (GObject *object)
{
    BgSource *self = BG_SOURCE (object);

    g_signal_handlers_disconnect_by_func (self->settings, on_settings_changed, self);
    g_clear_object (&self->settings);
    g_free (self->uri);
    g_strfreev (self->uri_list);

    G_OBJECT_CLASS (bg_source_parent_class)->finalize (object);
}

static void
bg_source_class_init (BgSourceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->finalize = bg_source_finalize;

    signals[SIGNAL_CHANGED] =
        g_signal_new ("changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 0);
}

static void
bg_source_init (BgSource *self)
{
    self->settings = g_settings_new (BACKGROUND_SCHEMA);

    g_autoptr(GSettingsSchema) schema = NULL;
    g_object_get (self->settings, "settings-schema", &schema, NULL);
    self->has_uri_list = schema && g_settings_schema_has_key (schema, "picture-uri-list");

    read_settings (self);
    g_signal_connect (self->settings, "changed",
                      G_CALLBACK (on_settings_changed), self);
}

BgSource *
bg_source_new (void)
{
    return g_object_new (BG_TYPE_SOURCE, NULL);
}

void
bg_source_get_input (BgSource *self, BgRenderInput *in)
{
    in->placement  = self->placement;
    in->shading    = self->shading;
    in->primary    = self->primary;
    in->secondary  = self->secondary;
    in->opacity    = self->opacity;
    in->source     = NULL;
}

const char *
bg_source_get_uri (BgSource *self)
{
    return self->uri;
}

/* picture-uri takes priority: when it is set, every monitor shows it. Only when
 * it is empty do we fall back to the per-monitor picture-uri-list, assigning
 * entries to monitors in order and reusing the last entry once we run out. */
const char *
bg_source_get_uri_for_monitor (BgSource *self, guint monitor_index)
{
    if (self->uri && self->uri[0] != '\0')
        return self->uri;

    guint n = self->uri_list ? g_strv_length (self->uri_list) : 0;
    if (n == 0)
        return NULL;

    const char *uri = self->uri_list[MIN (monitor_index, n - 1)];
    return (uri && uri[0] != '\0') ? uri : NULL;
}
