#include "knvr_zone.h"

#include "kilix_mask.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define MAP "build/test-zones.png"

/* A map painted at `width` x `height`: the left half is "drive", the
 * right half is "road", and the road is preclusive. */
static bool paint(int width, int height, int inertia, int loiter)
{
    kmask *mask = NULL;
    char number[16];
    bool ok;

    if (!kmask_create(&mask, width, height, 4)) {
        return false;
    }
    kmask_fill_rect(mask, 0, 0, width / 2, height, 1u);
    kmask_fill_rect(mask, width / 2, 0, width, height, 2u);
    ok = kmask_region_set_name(mask, 1u, "drive") &&
         kmask_region_set_name(mask, 2u, "road");
    (void)snprintf(number, sizeof(number), "%d", inertia);
    ok = ok && kmask_region_set_attr(mask, 1u, "inertia", number);
    (void)snprintf(number, sizeof(number), "%d", loiter);
    ok = ok && kmask_region_set_attr(mask, 1u, "loiter", number) &&
         kmask_region_set_attr(mask, 2u, "preclusive", "yes") &&
         kmask_save(mask, MAP);
    kmask_free(mask);
    return ok;
}

static knvr_detection_box box(int x, int y, int w, int h)
{
    knvr_detection_box out;

    (void)memset(&out, 0, sizeof(out));
    out.x = x;
    out.y = y;
    out.w = w;
    out.h = h;
    return out;
}

static bool test_a_point_lands_in_its_zone(void)
{
    knvr_zones *zones = NULL;

    CHECK(paint(640, 360, 1, 0));
    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(knvr_zones_count(zones) == 2u);
    CHECK(knvr_zones_at_point(zones, 100, 100) == 1u);
    CHECK(knvr_zones_at_point(zones, 500, 100) == 2u);
    CHECK(strcmp(knvr_zone_name(zones, 1u), "drive") == 0);
    CHECK(strcmp(knvr_zone_name(zones, 2u), "road") == 0);
    CHECK(knvr_zones_find(zones, "drive") != NULL);
    CHECK(knvr_zones_find(zones, "nowhere") == NULL);
    CHECK(knvr_zone_is_preclusive(zones, 2u));
    CHECK(!knvr_zone_is_preclusive(zones, 1u));
    /* Off the edge is nowhere, not a crash and not zone 1. */
    CHECK(knvr_zones_at_point(zones, -5, -5) == 0u);
    CHECK(knvr_zones_at_point(zones, 5000, 5000) == 0u);
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

/*
 * The map is painted over a main-stream still and consulted against
 * substream boxes.  Getting this wrong puts every zone in the wrong place
 * while looking entirely plausible, so it is worth its own test.
 */
static bool test_geometry_is_reconciled(void)
{
    knvr_zones *zones = NULL;

    CHECK(paint(2304, 1296, 1, 0));
    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    /* A quarter of the way across is in the drive at either scale. */
    CHECK(knvr_zones_at_point(zones, 160, 180) == 1u);
    CHECK(knvr_zones_at_point(zones, 480, 180) == 2u);
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

/*
 * Where it stands, not where its head is.
 *
 * The ground point and the centroid differ only in y, so the map that
 * tells them apart is split horizontally: a person standing on the near
 * pavement has their chest against the far verge, and a centroid puts
 * them in the wrong one.
 */
static bool test_the_ground_point_decides(void)
{
    kmask *mask = NULL;
    knvr_zones *zones = NULL;
    knvr_zone_hit hit;
    /* 200 tall, feet at y=299 (near), centroid at y=200 (far). */
    const knvr_detection_box standing = box(300, 100, 40, 200);

    CHECK(kmask_create(&mask, 640, 360, 4));
    kmask_fill_rect(mask, 0, 0, 640, 240, 1u);
    kmask_fill_rect(mask, 0, 240, 640, 360, 2u);
    CHECK(kmask_region_set_name(mask, 1u, "verge"));
    CHECK(kmask_region_set_name(mask, 2u, "pavement"));
    CHECK(kmask_save(mask, MAP));
    kmask_free(mask);

    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(knvr_zones_at_point(zones, 320, 200) == 1u);   /* centroid: verge */
    CHECK(knvr_zones_track(zones, 1, &standing, 1000, &hit));
    CHECK(hit.region == 2u);                             /* feet: pavement */
    CHECK(hit.settled);
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

static bool test_inertia_holds_it_back(void)
{
    knvr_zones *zones = NULL;
    knvr_zone_hit hit;
    const knvr_detection_box standing = box(100, 100, 40, 80);

    CHECK(paint(640, 360, 3, 0));
    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(knvr_zones_at(zones, 0u)->inertia == 3);

    CHECK(knvr_zones_track(zones, 7, &standing, 1000, &hit));
    CHECK(hit.region == 1u && !hit.settled);
    CHECK(knvr_zones_track(zones, 7, &standing, 1200, &hit));
    CHECK(!hit.settled);
    CHECK(knvr_zones_track(zones, 7, &standing, 1400, &hit));
    CHECK(hit.settled);

    /* Another object starts its own count. */
    CHECK(knvr_zones_track(zones, 8, &standing, 1400, &hit));
    CHECK(!hit.settled);

    /* Stepping out and back starts again. */
    {
        const knvr_detection_box away = box(500, 100, 40, 80);

        CHECK(knvr_zones_track(zones, 7, &away, 1600, &hit));
        CHECK(hit.region == 2u);
        CHECK(knvr_zones_track(zones, 7, &standing, 1800, &hit));
        CHECK(hit.region == 1u && !hit.settled);
    }
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

static bool test_loitering_needs_time(void)
{
    knvr_zones *zones = NULL;
    knvr_zone_hit hit;
    const knvr_detection_box standing = box(100, 100, 40, 80);

    CHECK(paint(640, 360, 1, 30));
    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(knvr_zones_track(zones, 3, &standing, 1000, &hit));
    CHECK(hit.settled && !hit.loitering);
    CHECK(knvr_zones_track(zones, 3, &standing, 20000, &hit));
    CHECK(!hit.loitering);
    CHECK(knvr_zones_track(zones, 3, &standing, 31000, &hit));
    CHECK(hit.loitering);

    /* Forgetting the track forgets how long it had been there. */
    knvr_zones_forget(zones, 3);
    CHECK(knvr_zones_track(zones, 3, &standing, 32000, &hit));
    CHECK(!hit.loitering);
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

static bool test_defining_and_removing(void)
{
    const char *reason = NULL;
    knvr_zones *zones = NULL;
    uint8_t first = 0u;
    uint8_t second = 0u;

    (void)remove(MAP);
    CHECK(knvr_zones_define(MAP, "porch", 640, 360, 2, false, 0, &first,
                            &reason));
    CHECK(first == 1u);
    CHECK(knvr_zones_define(MAP, "lawn", 0, 0, 1, true, 15, &second,
                            &reason));
    CHECK(second == 2u);
    /* The same name twice is refused, with a reason. */
    CHECK(!knvr_zones_define(MAP, "porch", 0, 0, 1, false, 0, &first,
                             &reason));
    CHECK(reason != NULL);

    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(knvr_zones_count(zones) == 2u);
    CHECK(knvr_zones_find(zones, "porch")->inertia == 2);
    CHECK(!knvr_zones_find(zones, "porch")->preclusive);
    CHECK(knvr_zones_find(zones, "lawn")->preclusive);
    CHECK(knvr_zones_find(zones, "lawn")->loiter_seconds == 15);
    /* Nothing painted yet, which is a state the operator has to be able
     * to see rather than a reason to hide the zone. */
    CHECK(knvr_zones_find(zones, "porch")->cells == 0u);
    knvr_zones_free(zones);

    CHECK(knvr_zones_undefine(MAP, "porch", &reason));
    CHECK(!knvr_zones_undefine(MAP, "porch", &reason));
    CHECK(reason != NULL);

    /*
     * Region 1 is free again, and the zone that takes it must not inherit
     * the last tenant's policy.  "lawn" was preclusive; "gate" is not.
     */
    CHECK(knvr_zones_define(MAP, "gate", 0, 0, 1, false, 0, &first, &reason));
    CHECK(first == 1u);
    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(!knvr_zones_find(zones, "gate")->preclusive);
    CHECK(knvr_zones_find(zones, "gate")->inertia == 1);
    CHECK(knvr_zones_find(zones, "gate")->loiter_seconds == 0);
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

/* A painted region nobody named is still a zone. */
static bool test_an_unnamed_region_is_still_a_zone(void)
{
    kmask *mask = NULL;
    knvr_zones *zones = NULL;

    CHECK(kmask_create(&mask, 640, 360, 4));
    kmask_fill_rect(mask, 0, 0, 100, 100, 5u);
    CHECK(kmask_save(mask, MAP));
    kmask_free(mask);

    CHECK(knvr_zones_load(&zones, MAP, 640, 360));
    CHECK(knvr_zones_count(zones) == 1u);
    CHECK(strcmp(knvr_zones_at(zones, 0u)->name, "zone5") == 0);
    CHECK(knvr_zones_at(zones, 0u)->inertia == 1);
    knvr_zones_free(zones);
    (void)remove(MAP);
    return true;
}

static bool test_nonsense_is_refused(void)
{
    knvr_zones *zones = NULL;
    const char *reason = NULL;
    uint8_t region = 0u;

    CHECK(!knvr_zones_load(&zones, "build/no-such-map.png", 640, 360));
    CHECK(!knvr_zones_load(&zones, MAP, 0, 0));
    CHECK(!knvr_zones_load(NULL, MAP, 640, 360));
    CHECK(knvr_zones_at_point(NULL, 1, 1) == 0u);
    CHECK(knvr_zone_name(NULL, 1u) == NULL);
    CHECK(!knvr_zone_is_preclusive(NULL, 1u));
    knvr_zones_forget(NULL, 1);
    /* No map and no geometry: nothing to create it from. */
    CHECK(!knvr_zones_define("build/no-such-map.png", "x", 0, 0, 1, false, 0,
                             &region, &reason));
    CHECK(reason != NULL);
    CHECK(!knvr_zones_undefine("build/no-such-map.png", "x", &reason));
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"a point lands in its zone", test_a_point_lands_in_its_zone},
        {"geometry is reconciled", test_geometry_is_reconciled},
        {"the ground point decides", test_the_ground_point_decides},
        {"inertia holds it back", test_inertia_holds_it_back},
        {"loitering needs time", test_loitering_needs_time},
        {"defining and removing", test_defining_and_removing},
        {"an unnamed region is still a zone",
         test_an_unnamed_region_is_still_a_zone},
        {"nonsense is refused", test_nonsense_is_refused}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
