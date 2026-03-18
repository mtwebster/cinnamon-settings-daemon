/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2025 Linux Mint
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <unistd.h>
#include <glib-object.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include <dirent.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#endif

#define EXIT_CODE_SUCCESS           0
#define EXIT_CODE_FAILED            1
#define EXIT_CODE_ARGUMENTS_INVALID 3
#define EXIT_CODE_NO_DEVICES        5

/* VIA protocol over QMK Raw HID (usage page 0xFF60) */
#define VIA_CUSTOM_SET_VALUE  0x07
#define VIA_CUSTOM_GET_VALUE  0x08

#define VIA_BACKLIGHT_CHANNEL    1
#define VIA_BACKLIGHT_BRIGHTNESS 1

#define HID_BUF_SIZE 33  /* report ID (1 byte) + 32 bytes payload */
#define HID_RAW_PAYLOAD 32

/* ========================================================================
 * Backend: sysfs LEDs (/sys/class/leds/ kbd_backlight)
 * ======================================================================== */

static gchar *
find_sysfs_kbd_backlight (void)
{
    const gchar *leds_dir = "/sys/class/leds";
    GDir *dir;
    const gchar *name;
    gchar *path = NULL;

    dir = g_dir_open (leds_dir, 0, NULL);
    if (dir == NULL)
        return NULL;

    while ((name = g_dir_read_name (dir)) != NULL) {
        if (strstr (name, "kbd_backlight") != NULL) {
            gchar *brightness_path = g_build_filename (leds_dir, name, "brightness", NULL);
            if (g_file_test (brightness_path, G_FILE_TEST_EXISTS)) {
                path = g_build_filename (leds_dir, name, NULL);
                g_free (brightness_path);
                break;
            }
            g_free (brightness_path);
        }
    }

    g_dir_close (dir);
    return path;
}

static gint
sysfs_get_value (const gchar *led_path, const gchar *attr)
{
    gchar *path;
    gchar *contents = NULL;
    gint value = -1;

    path = g_build_filename (led_path, attr, NULL);
    if (g_file_get_contents (path, &contents, NULL, NULL)) {
        value = atoi (g_strstrip (contents));
        g_free (contents);
    }
    g_free (path);
    return value;
}

static gboolean
sysfs_set_value (const gchar *led_path, gint value)
{
    gchar *path;
    gchar *text;
    gint fd, length, retval;
    gboolean ret = FALSE;

    path = g_build_filename (led_path, "brightness", NULL);
    fd = open (path, O_WRONLY);
    if (fd < 0) {
        g_free (path);
        return FALSE;
    }

    text = g_strdup_printf ("%i", value);
    length = strlen (text);
    retval = write (fd, text, length);
    ret = (retval == length);

    close (fd);
    g_free (text);
    g_free (path);
    return ret;
}

/* ========================================================================
 * Backend: QMK/VIA over HID Raw
 * ======================================================================== */

#ifdef __linux__

static gint
find_via_hidraw (void)
{
    DIR *dir;
    struct dirent *ent;
    char devpath[270];
    struct hidraw_report_descriptor rdesc;
    int fd;

    dir = opendir ("/dev");
    if (dir == NULL)
        return -1;

    while ((ent = readdir (dir)) != NULL) {
        if (strncmp (ent->d_name, "hidraw", 6) != 0)
            continue;

        snprintf (devpath, sizeof (devpath), "/dev/%s", ent->d_name);
        fd = open (devpath, O_RDWR);
        if (fd < 0)
            continue;

        if (ioctl (fd, HIDIOCGRDESCSIZE, &rdesc.size) < 0 ||
            rdesc.size > sizeof (rdesc.value) ||
            ioctl (fd, HIDIOCGRDESC, &rdesc) < 0) {
            close (fd);
            continue;
        }

        /* Look for QMK Raw HID usage page: 0x06 0x60 0xFF */
        gboolean found = FALSE;
        for (guint i = 0; i + 2 < rdesc.size; i++) {
            if (rdesc.value[i] == 0x06 &&
                rdesc.value[i+1] == 0x60 &&
                rdesc.value[i+2] == 0xFF) {
                found = TRUE;
                break;
            }
        }

        if (!found) {
            close (fd);
            continue;
        }

        /* Verify this device actually responds to VIA backlight commands */
        guint8 buf[HID_BUF_SIZE];
        memset (buf, 0, sizeof (buf));
        buf[0] = 0x00;
        buf[1] = VIA_CUSTOM_GET_VALUE;
        buf[2] = VIA_BACKLIGHT_CHANNEL;
        buf[3] = VIA_BACKLIGHT_BRIGHTNESS;

        if (write (fd, buf, HID_BUF_SIZE) < 0) {
            close (fd);
            continue;
        }

        guint8 recvbuf[HID_RAW_PAYLOAD];
        memset (recvbuf, 0, sizeof (recvbuf));
        if (read (fd, recvbuf, HID_RAW_PAYLOAD) < 0) {
            close (fd);
            continue;
        }

        /* A valid VIA response echoes back the command ID */
        if (recvbuf[0] != VIA_CUSTOM_GET_VALUE) {
            close (fd);
            continue;
        }

        closedir (dir);
        return fd;
    }

    closedir (dir);
    return -1;
}

static gint
via_get_brightness (int fd)
{
    guint8 buf[HID_BUF_SIZE];
    guint8 recvbuf[HID_RAW_PAYLOAD];

    memset (buf, 0, sizeof (buf));
    buf[0] = 0x00;
    buf[1] = VIA_CUSTOM_GET_VALUE;
    buf[2] = VIA_BACKLIGHT_CHANNEL;
    buf[3] = VIA_BACKLIGHT_BRIGHTNESS;

    if (write (fd, buf, HID_BUF_SIZE) < 0)
        return -1;

    memset (recvbuf, 0, sizeof (recvbuf));
    if (read (fd, recvbuf, HID_RAW_PAYLOAD) < 0)
        return -1;

    if (recvbuf[0] != VIA_CUSTOM_GET_VALUE)
        return -1;

    /* Response: [cmd, channel, id, value] */
    return recvbuf[3];
}

static gboolean
via_set_brightness (int fd, guint8 brightness)
{
    guint8 buf[HID_BUF_SIZE];
    guint8 recvbuf[HID_RAW_PAYLOAD];

    memset (buf, 0, sizeof (buf));
    buf[0] = 0x00;
    buf[1] = VIA_CUSTOM_SET_VALUE;
    buf[2] = VIA_BACKLIGHT_CHANNEL;
    buf[3] = VIA_BACKLIGHT_BRIGHTNESS;
    buf[4] = brightness;

    if (write (fd, buf, HID_BUF_SIZE) < 0)
        return FALSE;

    memset (recvbuf, 0, sizeof (recvbuf));
    if (read (fd, recvbuf, HID_RAW_PAYLOAD) < 0)
        return FALSE;

    return TRUE;
}

#endif /* __linux__ */

/* ========================================================================
 * Main
 * ======================================================================== */

int
main (int argc, char *argv[])
{
    GOptionContext *context;
    guint retval = EXIT_CODE_FAILED;
    gint set_brightness = -1;
    gboolean get_brightness = FALSE;
    gboolean get_max_brightness = FALSE;

    const GOptionEntry options[] = {
        { "set-brightness", '\0', 0, G_OPTION_ARG_INT, &set_brightness,
          "Set the current keyboard backlight brightness", NULL },
        { "get-brightness", '\0', 0, G_OPTION_ARG_NONE, &get_brightness,
          "Get the current keyboard backlight brightness", NULL },
        { "get-max-brightness", '\0', 0, G_OPTION_ARG_NONE, &get_max_brightness,
          "Get the maximum keyboard backlight brightness", NULL },
        { NULL }
    };

    context = g_option_context_new (NULL);
    g_option_context_set_summary (context, "Cinnamon Settings Daemon Keyboard Backlight Helper");
    g_option_context_add_main_entries (context, options, NULL);
    g_option_context_parse (context, &argc, &argv, NULL);
    g_option_context_free (context);

#ifndef __linux__
    g_critical ("csd-kbd-backlight-helper is only for Linux");
    g_assert_not_reached ();
#endif

    if (set_brightness == -1 && !get_brightness && !get_max_brightness) {
        g_print ("No valid option was specified\n");
        return EXIT_CODE_ARGUMENTS_INVALID;
    }

    /* Try sysfs LED backend first */
    gchar *led_path = find_sysfs_kbd_backlight ();
    if (led_path != NULL) {
        if (get_brightness) {
            gint val = sysfs_get_value (led_path, "brightness");
            if (val >= 0) {
                g_print ("%d", val);
                retval = EXIT_CODE_SUCCESS;
            }
        } else if (get_max_brightness) {
            gint val = sysfs_get_value (led_path, "max_brightness");
            if (val >= 0) {
                g_print ("%d", val);
                retval = EXIT_CODE_SUCCESS;
            }
        } else if (set_brightness >= 0) {
            if (sysfs_set_value (led_path, set_brightness))
                retval = EXIT_CODE_SUCCESS;
            else
                g_print ("Failed to write brightness to %s\n", led_path);
        }

        g_free (led_path);
        return retval;
    }

#ifdef __linux__
    /* Fall back to QMK/VIA HID backend - raw values, no translation */
    int hid_fd = find_via_hidraw ();
    if (hid_fd >= 0) {
        if (get_brightness) {
            gint val = via_get_brightness (hid_fd);
            if (val >= 0) {
                g_print ("%d", val);
                retval = EXIT_CODE_SUCCESS;
            }
        } else if (get_max_brightness) {
            g_print ("255");
            retval = EXIT_CODE_SUCCESS;
        } else if (set_brightness >= 0) {
            guint8 clamped = (guint8) CLAMP (set_brightness, 0, 255);
            if (via_set_brightness (hid_fd, clamped))
                retval = EXIT_CODE_SUCCESS;
            else
                g_print ("Failed to set VIA backlight brightness\n");
        }

        close (hid_fd);
        return retval;
    }
#endif

    g_print ("No keyboard backlight devices found\n");
    return EXIT_CODE_NO_DEVICES;
}
