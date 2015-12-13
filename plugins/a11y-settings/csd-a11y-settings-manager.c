/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street - Suite 500, Boston, MA 02110-1335, USA.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include "cinnamon-settings-profile.h"
#include "csd-a11y-settings-manager.h"

#define CSD_A11Y_SETTINGS_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CSD_TYPE_A11Y_SETTINGS_MANAGER, CsdA11ySettingsManagerPrivate))

struct CsdA11ySettingsManagerPrivate
{
        GSettings *interface_settings;
        GSettings *a11y_apps_settings;

        GHashTable       *bind_table;
};

enum {
        PROP_0,
};

static void     csd_a11y_settings_manager_class_init  (CsdA11ySettingsManagerClass *klass);
static void     csd_a11y_settings_manager_init        (CsdA11ySettingsManager      *a11y_settings_manager);
static void     csd_a11y_settings_manager_finalize    (GObject                     *object);

G_DEFINE_TYPE (CsdA11ySettingsManager, csd_a11y_settings_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

#define CINNAMON_A11Y_APP_SCHEMA "org.cinnamon.desktop.a11y.applications"
#define CINNAMON_DESKTOP_SCHEMA "org.cinnamon.desktop.interface"

#define GNOME_A11Y_APP_SCHEMA "org.gnome.desktop.a11y.applications"
#define GNOME_DESKTOP_SCHEMA "org.gnome.desktop.interface"

static void
bound_key_changed (GSettings *settings,
                   gchar     *key,
                   gpointer   user_data)
{
    GSettings *other_settings = G_SETTINGS (user_data);

    g_signal_handlers_block_by_func (settings, bound_key_changed, other_settings);
    g_signal_handlers_block_by_func (other_settings, bound_key_changed, settings);

    g_settings_set_boolean (other_settings, key, g_settings_get_boolean (settings, key));

    g_signal_handlers_unblock_by_func (settings, bound_key_changed, other_settings);
    g_signal_handlers_unblock_by_func (other_settings, bound_key_changed, settings);
}

static void
bind_keys (CsdA11ySettingsManager *manager,
           const gchar            *schema_id_a,
           const gchar            *schema_id_b,
           const gchar            *key)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();

    /* We assume any schema_id_a will exist (since it's our's) */

    GSettingsSchema *schema = g_settings_schema_source_lookup (source, schema_id_b, FALSE);

    if (!schema)
        return;

    if (!g_settings_schema_has_key (schema, key))
        return;

    g_settings_schema_unref (schema);

    if (!manager->priv->bind_table) {
        manager->priv->bind_table = g_hash_table_new_full (g_direct_hash, g_str_equal, g_free, g_object_unref);
    }

    GSettings *a_settings = NULL;
    GSettings *b_settings = NULL;

    a_settings = g_hash_table_lookup (manager->priv->bind_table, schema_id_a);

    if (!a_settings) {
        a_settings = g_settings_new (schema_id_a);
        g_hash_table_insert (manager->priv->bind_table, g_strdup (schema_id_a), a_settings);
    }

    b_settings = g_hash_table_lookup (manager->priv->bind_table, schema_id_b);

    if (!b_settings) {
        b_settings = g_settings_new (schema_id_b);
        g_hash_table_insert (manager->priv->bind_table, g_strdup (schema_id_b), b_settings);
    }

    /* initial sync */
    g_settings_set_boolean (b_settings,
                            key,
                            g_settings_get_boolean (a_settings,
                                                    key));

    gchar *detailed_signal = g_strdup_printf ("changed::%s", key);

    g_signal_connect (a_settings, detailed_signal, G_CALLBACK (bound_key_changed), b_settings);
    g_signal_connect (b_settings, detailed_signal, G_CALLBACK (bound_key_changed), a_settings);

    g_free (detailed_signal);
}

static void
bind_cinnamon_gnome_a11y_settings (CsdA11ySettingsManager *manager)
{
    bind_keys (manager, CINNAMON_A11Y_APP_SCHEMA, GNOME_A11Y_APP_SCHEMA, "screen-keyboard-enabled");
    bind_keys (manager, CINNAMON_A11Y_APP_SCHEMA, GNOME_A11Y_APP_SCHEMA, "screen-reader-enabled");
    bind_keys (manager, CINNAMON_DESKTOP_SCHEMA, GNOME_DESKTOP_SCHEMA, "toolkit-accessibility");
}

static void
hash_table_foreach_disconnect (gpointer key,
                               gpointer value,
                               gpointer user_data)
{
    g_signal_handlers_disconnect_matched (G_SETTINGS (value),
                                          G_SIGNAL_MATCH_FUNC,
                                          0,
                                          0,
                                          NULL,
                                          bound_key_changed,
                                          NULL);
}

static void
unbind_cinnamon_gnome_a11y_settings (CsdA11ySettingsManager *manager)
{
    g_hash_table_foreach (manager->priv->bind_table, (GHFunc) hash_table_foreach_disconnect, manager);
    g_clear_pointer (&manager->priv->bind_table, g_hash_table_destroy);
}

static void
apps_settings_changed (GSettings              *settings,
		       const char             *key,
		       CsdA11ySettingsManager *manager)
{
	gboolean screen_reader, keyboard;

	if (g_str_equal (key, "screen-reader-enabled") == FALSE &&
	    g_str_equal (key, "screen-keyboard-enabled") == FALSE)
		return;

	g_debug ("screen reader or OSK enablement changed");

	screen_reader = g_settings_get_boolean (manager->priv->a11y_apps_settings, "screen-reader-enabled");
	keyboard = g_settings_get_boolean (manager->priv->a11y_apps_settings, "screen-keyboard-enabled");

	if (screen_reader || keyboard) {
		g_debug ("Enabling toolkit-accessibility, screen reader or OSK enabled");
		g_settings_set_boolean (manager->priv->interface_settings, "toolkit-accessibility", TRUE);
	} else if (screen_reader == FALSE && keyboard == FALSE) {
		g_debug ("Disabling toolkit-accessibility, screen reader and OSK disabled");
		g_settings_set_boolean (manager->priv->interface_settings, "toolkit-accessibility", FALSE);
	}
}

gboolean
csd_a11y_settings_manager_start (CsdA11ySettingsManager *manager,
                                 GError                **error)
{
        g_debug ("Starting a11y_settings manager");
        cinnamon_settings_profile_start (NULL);

	manager->priv->interface_settings = g_settings_new ("org.cinnamon.desktop.interface");
	manager->priv->a11y_apps_settings = g_settings_new ("org.cinnamon.desktop.a11y.applications");

	g_signal_connect (G_OBJECT (manager->priv->a11y_apps_settings), "changed",
			  G_CALLBACK (apps_settings_changed), manager);

	/* If any of the screen reader or on-screen keyboard are enabled,
	 * make sure a11y is enabled for the toolkits.
	 * We don't do the same thing for the reverse so it's possible to
	 * enable AT-SPI for the toolkits without using an a11y app */
	if (g_settings_get_boolean (manager->priv->a11y_apps_settings, "screen-keyboard-enabled") ||
	    g_settings_get_boolean (manager->priv->a11y_apps_settings, "screen-reader-enabled"))
		g_settings_set_boolean (manager->priv->interface_settings, "toolkit-accessibility", TRUE);

    /* Some accessibility applications (like orca) are hardcoded to look at the gnome
       a11y applications schema - mirror them */
    bind_cinnamon_gnome_a11y_settings (manager);

    cinnamon_settings_profile_end (NULL);
    return TRUE;
}

void
csd_a11y_settings_manager_stop (CsdA11ySettingsManager *manager)
{
    unbind_cinnamon_gnome_a11y_settings (manager);

	if (manager->priv->interface_settings) {
		g_object_unref (manager->priv->interface_settings);
		manager->priv->interface_settings = NULL;
	}
	if (manager->priv->a11y_apps_settings) {
		g_object_unref (manager->priv->a11y_apps_settings);
		manager->priv->a11y_apps_settings = NULL;
	}
        g_debug ("Stopping a11y_settings manager");
}

static GObject *
csd_a11y_settings_manager_constructor (GType                  type,
                                       guint                  n_construct_properties,
                                       GObjectConstructParam *construct_properties)
{
        CsdA11ySettingsManager      *a11y_settings_manager;

        a11y_settings_manager = CSD_A11Y_SETTINGS_MANAGER (G_OBJECT_CLASS (csd_a11y_settings_manager_parent_class)->constructor (type,
                                                                                                                                 n_construct_properties,
                                                                                                                                 construct_properties));

        return G_OBJECT (a11y_settings_manager);
}

static void
csd_a11y_settings_manager_dispose (GObject *object)
{
        G_OBJECT_CLASS (csd_a11y_settings_manager_parent_class)->dispose (object);
}

static void
csd_a11y_settings_manager_class_init (CsdA11ySettingsManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->constructor = csd_a11y_settings_manager_constructor;
        object_class->dispose = csd_a11y_settings_manager_dispose;
        object_class->finalize = csd_a11y_settings_manager_finalize;

        g_type_class_add_private (klass, sizeof (CsdA11ySettingsManagerPrivate));
}

static void
csd_a11y_settings_manager_init (CsdA11ySettingsManager *manager)
{
        manager->priv = CSD_A11Y_SETTINGS_MANAGER_GET_PRIVATE (manager);

        manager->priv->bind_table = NULL;
}

static void
csd_a11y_settings_manager_finalize (GObject *object)
{
        CsdA11ySettingsManager *a11y_settings_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CSD_IS_A11Y_SETTINGS_MANAGER (object));

        a11y_settings_manager = CSD_A11Y_SETTINGS_MANAGER (object);

        g_return_if_fail (a11y_settings_manager->priv != NULL);

        G_OBJECT_CLASS (csd_a11y_settings_manager_parent_class)->finalize (object);
}

CsdA11ySettingsManager *
csd_a11y_settings_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (CSD_TYPE_A11Y_SETTINGS_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return CSD_A11Y_SETTINGS_MANAGER (manager_object);
}
