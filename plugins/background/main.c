#include "config.h"

#include <signal.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include "csd-background-manager.h"
#include "csd-exported-background.h"

#define CSD_BACKGROUND_APPLICATION_ID "org.cinnamon.SettingsDaemon.Background"

/* Values published over the org.cinnamon.SettingsDaemon.Background State
 * property; mirrored by watchers. Keep in sync with the interface XML. */
typedef enum {
    CSD_BACKGROUND_STATE_INITIALIZING = 0,
    CSD_BACKGROUND_STATE_READY        = 1,
} CsdBackgroundState;

static gboolean verbose = FALSE;

static const GOptionEntry entries[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Enable verbose logging", NULL },
    { NULL }
};

static void
on_manager_ready (CsdBackgroundManager *manager G_GNUC_UNUSED, gpointer user_data)
{
    CsdExportedBackground *skeleton = user_data;
    g_debug ("Publishing READY state");
    csd_exported_background_set_state (skeleton, CSD_BACKGROUND_STATE_READY);
}

static gboolean
on_sigterm (gpointer data)
{
    g_debug ("Got SIGTERM, quitting");
    g_main_loop_quit ((GMainLoop *) data);
    return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
    g_set_prgname ("csd-background");

    g_autoptr(GOptionContext) context = g_option_context_new (NULL);
    g_option_context_add_main_entries (context, entries, NULL);

    GError *error = NULL;
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("csd-background: %s\n", error->message);
        g_error_free (error);
        return 1;
    }

    if (verbose && !g_getenv ("G_MESSAGES_DEBUG"))
        g_setenv ("G_MESSAGES_DEBUG", G_LOG_DOMAIN, TRUE);

    g_message ("Starting up");

    /* Prefer the GL renderer over Vulkan: for a mostly-static wallpaper with an
     * occasional crossfade it carries a smaller resident footprint (~45MB less
     * per process in testing) with no visible difference. overwrite=FALSE so an
     * explicit GSK_RENDERER from the environment still wins. */
    g_setenv ("GSK_RENDERER", "gl", FALSE);

    /* Needs only the compositor/display, not the portal or the session bus. */
    if (!gtk_init_check ()) {
        g_critical ("csd-background: could not initialize GTK");
        return 1;
    }

    /* Single-instance guard. Owning the application-id bus name is what keeps a
     * second copy from running; it is not a session-client registration. */
    g_autoptr(GApplication) app =
        g_application_new (CSD_BACKGROUND_APPLICATION_ID, G_APPLICATION_DEFAULT_FLAGS);

    /* Exported on the application's own bus connection, alongside
     * org.freedesktop.Application, so a watcher can follow readiness via the
     * State property. NULL when there is no session bus (we still draw). */
    CsdExportedBackground *skeleton = NULL;

    error = NULL;
    if (g_application_register (app, NULL, &error)) {
        if (g_application_get_is_remote (app)) {
            g_message ("Another instance already owns %s, exiting",
                     CSD_BACKGROUND_APPLICATION_ID);
            return 0;
        }
        g_debug ("Registered as the primary instance (%s)", CSD_BACKGROUND_APPLICATION_ID);

        GDBusConnection *connection = g_application_get_dbus_connection (app);
        const char *object_path = g_application_get_dbus_object_path (app);
        if (connection && object_path) {
            skeleton = csd_exported_background_skeleton_new ();
            csd_exported_background_set_state (skeleton, CSD_BACKGROUND_STATE_INITIALIZING);

            GError *export_error = NULL;
            if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton),
                                                   connection, object_path, &export_error)) {
                g_warning ("Could not export the Background interface: %s", export_error->message);
                g_clear_error (&export_error);
                g_clear_object (&skeleton);
            }
        }
    } else {
        g_warning ("Could not register on the session bus (%s); "
                   "continuing so the wallpaper still draws.", error->message);
        g_clear_error (&error);
    }

    /* Draw independently of registration so a bus/portal hiccup can't blank the
     * desktop. The daemon exits with the compositor: GTK4 _exit()s when the
     * display connection drops at logout. */
    CsdBackgroundManager *manager = csd_background_manager_new ();
    if (skeleton)
        g_signal_connect (manager, "ready", G_CALLBACK (on_manager_ready), skeleton);
    csd_background_manager_start (manager, NULL);

    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    g_unix_signal_add (SIGTERM, on_sigterm, loop);

    g_debug ("Entering main loop");
    g_main_loop_run (loop);

    g_message ("Shutting down");
    csd_background_manager_stop (manager);
    if (skeleton) {
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (skeleton));
        g_object_unref (skeleton);
    }
    g_object_unref (manager);
    g_main_loop_unref (loop);

    return 0;
}
