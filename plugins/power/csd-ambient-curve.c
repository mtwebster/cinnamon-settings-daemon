#include <math.h>

#include "csd-ambient-curve.h"

#define LEVEL_STEP (100.0 / (CSD_AMBIENT_CURVE_POINTS - 1))

static gdouble
band_around (gdouble lux)
{
    return MAX (CSD_AMBIENT_CURVE_BAND_LUX, fabs (lux) * CSD_AMBIENT_CURVE_BAND_FRACTION);
}

void
csd_ambient_curve_init (CsdAmbientCurve *curve, const gdouble *points)
{
    guint i;

    for (i = 0; i < CSD_AMBIENT_CURVE_POINTS; i++)
        curve->points[i] = points[i];

    curve->applied_reading = -1.0;
    curve->applied_percentage = -1;
    curve->candidate_since = 0;
}

gint
csd_ambient_curve_lookup (const CsdAmbientCurve *curve, gdouble lux)
{
    guint i;

    if (lux <= curve->points[0])
        return 0;

    for (i = 1; i < CSD_AMBIENT_CURVE_POINTS; i++) {
        gdouble span;
        gdouble fraction;

        if (lux > curve->points[i])
            continue;

        span = curve->points[i] - curve->points[i - 1];
        fraction = span > 0.0 ? (lux - curve->points[i - 1]) / span : 1.0;
        return (gint) (LEVEL_STEP * (i - 1) + LEVEL_STEP * fraction + 0.5);
    }

    return 100;
}

/* Reshape the curve so this light level maps to the chosen percentage:
 * the segment the percentage falls in is narrowed to a small window
 * around the reading, and the remaining points are pushed outward just
 * enough to keep the curve strictly increasing. Points below zero are
 * allowed so any level can be chosen in the dark. */
void
csd_ambient_curve_adjust (CsdAmbientCurve *curve, gdouble lux, gint percentage)
{
    guint last = CSD_AMBIENT_CURVE_POINTS - 1;
    guint j;
    gint i;

    percentage = CLAMP (percentage, 0, 100);
    j = (guint) (percentage / LEVEL_STEP);

    if (j >= last) {
        j = last;
        curve->points[j] = lux;
    } else {
        gdouble fraction = (percentage - LEVEL_STEP * j) / LEVEL_STEP;
        gdouble window = 2.0 * band_around (lux);

        curve->points[j] = lux - fraction * window;
        curve->points[j + 1] = lux + (1.0 - fraction) * window;
    }

    for (i = (gint) j - 1; i >= 0; i--) {
        gdouble limit = curve->points[i + 1] - band_around (curve->points[i + 1]);

        if (curve->points[i] > limit)
            curve->points[i] = limit;
    }

    for (i = j + 2; i < CSD_AMBIENT_CURVE_POINTS; i++) {
        gdouble limit = curve->points[i - 1] + band_around (curve->points[i - 1]);

        if (curve->points[i] < limit)
            curve->points[i] = limit;
    }

    curve->applied_reading = lux;
    curve->applied_percentage = percentage;
    curve->candidate_since = 0;
}

/* Returns the percentage to write, or -1. A reading inside the band
 * around the last applied one is unchanged light. One outside it has to
 * stay outside for the settle time; *wait tells the caller to call again
 * after that time even if the sensor stays quiet. */
gint
csd_ambient_curve_update (CsdAmbientCurve *curve,
                          gdouble          lux,
                          gint64           now,
                          gboolean        *wait)
{
    gint percentage;

    *wait = FALSE;

    if (lux <= 0.0)
        return -1;

    if (curve->applied_reading >= 0.0) {
        if (fabs (lux - curve->applied_reading) <= band_around (curve->applied_reading)) {
            curve->candidate_since = 0;
            return -1;
        }

        if (curve->candidate_since == 0)
            curve->candidate_since = now;

        if (now - curve->candidate_since < CSD_AMBIENT_CURVE_SETTLE_USEC) {
            *wait = TRUE;
            return -1;
        }
    }

    curve->candidate_since = 0;
    curve->applied_reading = lux;

    percentage = csd_ambient_curve_lookup (curve, lux);
    if (percentage == curve->applied_percentage)
        return -1;

    curve->applied_percentage = percentage;
    return percentage;
}
