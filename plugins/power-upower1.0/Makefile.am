BUILT_SOURCES =

plugin_name = power

plugin_LTLIBRARIES =					\
	libpower.la

libpower_la_SOURCES = 					\
	gpm-common.c					\
	gpm-common.h					\
	csd-backlight-linux.c				\
	csd-backlight-linux.h				\
	csd-power-manager.c				\
	csd-power-manager.h				\
	gsm-inhibitor-flag.h				\
	gsm-presence-flag.h				\
	gsm-manager-logout-mode.h			\
	csd-power-constants.h				\
	csd-power-plugin.c

libpower_la_CPPFLAGS = \
	-I$(top_srcdir)/data/				\
	-I$(top_srcdir)/cinnamon-settings-daemon		\
	-I$(top_builddir)/cinnamon-settings-daemon		\
	-I$(top_srcdir)/plugins/common			\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	-DGTKBUILDERDIR=\""$(pkgdatadir)"\"		\
	-DSBINDIR=\"$(sbindir)\"			\
	-DLIBEXECDIR=\"$(libexecdir)\"			\
	$(AM_CPPFLAGS)

libpower_la_CFLAGS = \
	$(PLUGIN_CFLAGS)				\
	$(POWER_CFLAGS)					\
	$(SETTINGS_PLUGIN_CFLAGS)			\
	$(AM_CFLAGS)

libpower_la_LDFLAGS = 					\
	$(CSD_PLUGIN_LDFLAGS)

libpower_la_LIBADD  = 					\
	$(top_builddir)/plugins/common/libcommon.la	\
	$(top_builddir)/cinnamon-settings-daemon/libcsd.la \
	$(POWER_LIBS)					\
	$(SETTINGS_PLUGIN_LIBS)

plugin_in_files = 					\
	power.cinnamon-settings-plugin.in

plugin_DATA = $(plugin_in_files:.cinnamon-settings-plugin.in=.cinnamon-settings-plugin)

org.cinnamon.settings-daemon.plugins.power.policy.in: org.cinnamon.settings-daemon.plugins.power.policy.in.in Makefile
	$(AM_V_GEN) sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

@INTLTOOL_POLICY_RULE@
polkit_policydir = $(datadir)/polkit-1/actions
polkit_policy_in_files = org.cinnamon.settings-daemon.plugins.power.policy.in
polkit_policy_DATA = $(polkit_policy_in_files:.policy.in=.policy)

# so it always gets included in the tarball
csd_backlight_helper_SOURCES =				\
	csd-backlight-linux.c				\
	csd-backlight-linux.h				\
	csd-backlight-helper.c

noinst_PROGRAMS = csd-test-power
csd_test_power_SOURCES =				\
	gpm-common.c					\
	gpm-common.h					\
	csd-backlight-linux.c				\
	csd-backlight-linux.h				\
	csd-power-manager.c				\
	csd-power-manager.h				\
	gsm-inhibitor-flag.h				\
	gsm-presence-flag.h				\
	gsm-manager-logout-mode.h			\
	test-power.c

csd_test_power_CFLAGS = $(libpower_la_CFLAGS)
csd_test_power_CPPFLAGS = $(libpower_la_CPPFLAGS) -DCSD_MOCK=1
csd_test_power_LDADD =					\
	-lm						\
	$(top_builddir)/plugins/common/libcommon.la	\
	$(top_builddir)/cinnamon-settings-daemon/libcsd.la \
	$(POWER_LIBS)					\
	$(SETTINGS_PLUGIN_LIBS)

EXTRA_DIST = 						\
	$(plugin_in_files)				\
	$(csd_backlight_helper_SOURCES)			\
	$(NULL)

if HAVE_GUDEV
libexec_PROGRAMS = csd-backlight-helper

csd_backlight_helper_LDFLAGS =				\
	$(BACKLIGHT_HELPER_LIBS)			\
	-lm

csd_backlight_helper_CFLAGS =				\
	$(BACKLIGHT_HELPER_CFLAGS)

EXTRA_DIST += 						\
	org.cinnamon.settings-daemon.plugins.power.policy.in.in
endif

# Enums
CSD_POWER_ENUM_FILES = csd-power-enums.c csd-power-enums.h

csd-power-enums.h: gsm-inhibitor-flag.h gsm-presence-flag.h Makefile
	$(AM_V_GEN)($(GLIB_MKENUMS) \
			--fhead "#ifndef CSD_POWER_ENUMS_H\n#define CSD_POWER_ENUMS_H\n\n#include <glib-object.h>\n\nG_BEGIN_DECLS\n" \
			--fprod "/* enumerations from \"@filename@\" */\n" \
			--vhead "GType @enum_name@_get_type (void) G_GNUC_CONST;\n#define CSD_POWER_TYPE_@ENUMSHORT@ (@enum_name@_get_type())\n" \
			--ftail "G_END_DECLS\n\n#endif /* !CSD_POWER_ENUMS_H */" $(srcdir)/gsm-inhibitor-flag.h $(srcdir)/gsm-presence-flag.h > $@)

csd-power-enums.c: csd-power-constants.h gsm-inhibitor-flag.h gsm-presence-flag.h Makefile csd-power-enums.h
	$(AM_V_GEN)($(GLIB_MKENUMS) \
			--fhead "#include \"gsm-inhibitor-flag.h\"\n#include \"gsm-presence-flag.h\"\n#include \"csd-power-enums.h\"" \
			--fprod "\n/* enumerations from \"@filename@\" */" \
			--vhead "GType\n@enum_name@_get_type (void)\n{\n  static GType etype = 0;\n  if (etype == 0) {\n    static const G@Type@Value values[] = {" \
			--vprod "      { @VALUENAME@, \"@VALUENAME@\", \"@valuenick@\" }," \
			--vtail "      { 0, NULL, NULL }\n    };\n    etype = g_@type@_register_static (\"@EnumName@\", values);\n  }\n  return etype;\n}\n" \
		$(srcdir)/gsm-inhibitor-flag.h $(srcdir)/gsm-presence-flag.h > $@)

BUILT_SOURCES += $(CSD_POWER_ENUM_FILES)

csdpowerconstants.py: csd-power-constants-update.pl csd-power-constants.h
	$(AM_V_GEN) $(srcdir)/csd-power-constants-update.pl

csdpowerenums.py: csd-power-enums-update csd-power-enums.h csd-power-enums.c
	$(AM_V_GEN) $(builddir)/csd-power-enums-update > $@

noinst_PROGRAMS += csd-power-enums-update
csd_power_enums_update_SOURCES =		\
	csd-power-enums-update.c		\
	csd-power-enums.h			\
	csd-power-enums.c
csd_power_enums_update_CFLAGS = $(libpower_la_CFLAGS)
csd_power_enums_update_CPPFLAGS = $(libpower_la_CPPFLAGS) -I$(top_builddir)/plugins/power
csd_power_enums_update_LDFLAGS = $(POWER_LIBS)

EXTRA_DIST += csd-power-constants-update.pl csdpowerconstants.py csdpowerenums.py test.py

check-local: $(top_builddir)/tests/shiftkey csd-test-power test.py csdpowerconstants.py csdpowerenums.py
# This is how you run a single test
#	BUILDDIR=$(builddir) TOP_BUILDDIR=$(top_builddir) ${PYTHON} $(srcdir)/test.py PowerPluginTest.test_sleep_inactive_blank
	BUILDDIR=$(builddir) TOP_BUILDDIR=$(top_builddir) ${PYTHON} $(srcdir)/test.py

clean-local:
	rm -f *~

CLEANFILES = 							\
	$(plugin_DATA)						\
	org.cinnamon.settings-daemon.plugins.power.policy		\
	org.cinnamon.settings-daemon.plugins.power.policy.in	\
	$(CSD_POWER_ENUM_FILES)					\
	csdpowerenums.pyc csdpowerenums.py

@CSD_INTLTOOL_PLUGIN_RULE@
