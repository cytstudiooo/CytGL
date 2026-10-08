// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    widget_host_test.c
  * @brief   Widget tree rules that no screenshot can settle: the enforced child
  *          limit, and what hit testing does about clipping and deformation.
  *
  *  Why this file exists
  *  --------------------
  *  Three framework behaviours were true-by-accident rather than true-by-test, and
  *  each of them fails in a way a band renderer makes nearly invisible:
  *
  *  A. `CGL_CFG_WIDGET_MAX_CHILDREN` was defined and read by nothing, so a page
  *     that outgrew it kept running with its last widgets silently missing. Now the
  *     limit is enforced: `cgl_widget_add_child()` returns false, the child stays
  *     unattached, and the rejection is counted. What breaks if A is wrong: a
  *     widget you added is not on screen, and nothing anywhere says so.
  *
  *  B. `CGL_WF_CLIP` was honoured when drawing and **ignored** when hit testing, so
  *     a child that draws outside an ancestor's clip window could still be hit
  *     there -- "what you click is not what you see". Without `CGL_WF_CLIP` the
  *     widget is allowed to paint outside its frame, and its children must stay
  *     reachable there. Both halves are asserted, because fixing one by breaking
  *     the other is the easy mistake.
  *
  *  C. A deformed (`cgl_widget_set_warp`) subtree is painted somewhere else, and the
  *     hit walker has no inverse of the warp. It now declines the subtree instead
  *     of reporting a hit on pixels that are not on screen. During a transition
  *     input is locked anyway, so this is a safety net -- and a safety net that
  *     silently answers "yes" is not one.
  *
  *  Build & run
  *  -----------
  *      cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *      cmake --build build/host
  *      ctest --test-dir build/host -R widget_test --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>

#include <cytgl/cgl_widget.h>
#include <cytgl/cytgl_config.h>

static int s_fails = 0;

#define CHECK(cond, ...)                          \
    do                                            \
    {                                             \
        if (!(cond))                              \
        {                                         \
            s_fails++;                            \
            printf("  FAIL: ");                   \
            printf(__VA_ARGS__);                  \
            printf("   [%s:%d]\n", __FILE__, __LINE__); \
        }                                         \
    } while (0)

/* -------------------------------------------------------------------------- */
/* A. The child limit is enforced                                             */
/* -------------------------------------------------------------------------- */

static void test_child_limit(void)
{
    cgl_widget_t root;
    cgl_widget_t kids[CGL_CFG_WIDGET_MAX_CHILDREN + 1u];
    uint16_t i;
    uint16_t n;
    uint32_t dropped_before = cgl_widget_children_dropped();

    cgl_widget_init(&root, NULL, "root", cgl_rect_wh(0, 0, 320, 172));

    /* The default flags are exactly "visible + clips its children". CGL_WF_OPAQUE
       used to sit in here as a third bit that nothing read; if this check fails,
       a flag the rest of the framework does not implement is being advertised
       again. */
    CHECK(root.flags == (uint16_t)(CGL_WF_VISIBLE | CGL_WF_CLIP),
          "default flags are 0x%04X, expected VISIBLE|CLIP (0x%04X)",
          (unsigned)root.flags, (unsigned)(CGL_WF_VISIBLE | CGL_WF_CLIP));

    for (i = 0u; i < (uint16_t)CGL_CFG_WIDGET_MAX_CHILDREN; i++)
    {
        cgl_widget_init(&kids[i], NULL, "kid", cgl_rect_wh(0, 0, 10, 10));
        CHECK(cgl_widget_add_child(&root, &kids[i]) == true,
              "child %u of %u was refused", (unsigned)i,
              (unsigned)CGL_CFG_WIDGET_MAX_CHILDREN);
    }

    /* One past the limit: refused, left unattached, counted. */
    cgl_widget_init(&kids[CGL_CFG_WIDGET_MAX_CHILDREN], NULL, "overflow",
                    cgl_rect_wh(0, 0, 10, 10));
    CHECK(cgl_widget_add_child(&root, &kids[CGL_CFG_WIDGET_MAX_CHILDREN]) == false,
          "the %u-th child was attached: the limit is not enforced",
          (unsigned)(CGL_CFG_WIDGET_MAX_CHILDREN + 1u));
    CHECK(kids[CGL_CFG_WIDGET_MAX_CHILDREN].parent == NULL,
          "a refused child was left pointing at the parent");
    CHECK(cgl_widget_children_dropped() == (dropped_before + 1u),
          "the refusal was not counted (%u -> %u)", (unsigned)dropped_before,
          (unsigned)cgl_widget_children_dropped());

    /* The list really holds exactly the limit -- not "the limit plus the one that
       was silently appended anyway". */
    n = 0u;
    for (const cgl_widget_t *it = root.first_child; it != NULL; it = it->next)
    {
        n++;
    }
    CHECK(n == (uint16_t)CGL_CFG_WIDGET_MAX_CHILDREN,
          "the child list has %u entries, expected %u", (unsigned)n,
          (unsigned)CGL_CFG_WIDGET_MAX_CHILDREN);
    CHECK(root.last_child == &kids[CGL_CFG_WIDGET_MAX_CHILDREN - 1u],
          "last_child does not point at the last attached child");

    /* A full parent must still be able to **raise** a child: raise = remove + add,
       and the removal frees the slot. If someone "simplifies" the limit check into
       add_child without noticing this, raising the bottom-most child silently
       detaches it -- the widget disappears on the next navigation. */
    {
        uint32_t dropped_at_limit = cgl_widget_children_dropped();
        cgl_widget_raise(&kids[0]);
        CHECK(root.last_child == &kids[0], "raise() did not move the child to the top");
        CHECK(cgl_widget_children_dropped() == dropped_at_limit,
              "raise() on a full parent counted a dropped child");
        n = 0u;
        for (const cgl_widget_t *it = root.first_child; it != NULL; it = it->next)
        {
            n++;
        }
        CHECK(n == (uint16_t)CGL_CFG_WIDGET_MAX_CHILDREN,
              "raise() changed the child count to %u", (unsigned)n);
        CHECK(kids[0].parent == &root, "raise() detached the child");
    }

    /* Detach, then the slot is free again: the limit is a high-water mark of the
       live list, not a lifetime counter. */
    cgl_widget_remove_child(&kids[5]);
    CHECK(cgl_widget_add_child(&root, &kids[CGL_CFG_WIDGET_MAX_CHILDREN]) == true,
          "a slot freed by remove_child() was not reusable");
}

/* -------------------------------------------------------------------------- */
/* B/C. Hit testing: clipping, and the deformed subtree                       */
/* -------------------------------------------------------------------------- */

static void test_hit_test_clip(void)
{
    cgl_widget_t parent;
    cgl_widget_t child;

    /* The parent covers 0..99 and clips (the default); the child sticks out to the
       right of it (90..199), which is legal: the clip decides what is visible. */
    cgl_widget_init(&parent, NULL, "parent", cgl_rect(0, 0, 99, 99));
    cgl_widget_init(&child, NULL, "child", cgl_rect(90, 10, 199, 20));
    CHECK(cgl_widget_add_child(&parent, &child) == true, "setup: child not attached");

    /* Inside both -> the child, which is the deepest hit. */
    CHECK(cgl_widget_hit_test(&parent, 95, 15) == &child,
          "a point inside parent and child did not hit the child");

    /* Inside the child but outside the parent's clip window: those pixels are never
       painted, so they must not be hittable. */
    CHECK(cgl_widget_hit_test(&parent, 150, 15) == NULL,
          "a point clipped away by an ancestor was still hit");

    /* Outside the parent's frame, but the parent itself does not clip: the child is
       drawn there (a widget without CGL_WF_CLIP may paint outside its frame), so it
       must be reachable there. This is the half that a "just intersect everything"
       fix would break. */
    parent.flags &= (uint16_t)~CGL_WF_CLIP;
    CHECK(cgl_widget_hit_test(&parent, 150, 15) == &child,
          "a non-clipping parent made its overhanging child unreachable");

    /* Back inside the parent, a point that misses every child is the parent. */
    CHECK(cgl_widget_hit_test(&parent, 5, 5) == &parent,
          "an empty area of the parent did not hit the parent");
    /* Outside everything is nothing, at any level. */
    CHECK(cgl_widget_hit_test(&parent, 300, 160) == NULL,
          "a point outside the whole tree hit something");
}

static void test_hit_test_nested_clip(void)
{
    cgl_widget_t outer;
    cgl_widget_t mid;
    cgl_widget_t leaf;

    /* outer (0..99, CLIP) > mid (0..49, CLIP) > leaf (40..60).
       The leaf overhangs `mid` on purpose: the effective (painted) region is the
       intersection of every clipping ancestor. */
    cgl_widget_init(&outer, NULL, "outer", cgl_rect(0, 0, 99, 99));
    cgl_widget_init(&mid, NULL, "mid", cgl_rect(0, 0, 49, 49));
    cgl_widget_init(&leaf, NULL, "leaf", cgl_rect(40, 40, 60, 60));
    CHECK(cgl_widget_add_child(&outer, &mid) == true, "setup: mid not attached");
    CHECK(cgl_widget_add_child(&mid, &leaf) == true, "setup: leaf not attached");

    CHECK(cgl_widget_hit_test(&outer, 45, 45) == &leaf,
          "a point inside the whole clip chain did not reach the leaf");

    /* (55, 55) is inside the leaf's frame but outside `mid`'s clip window, so the
       leaf does not own that pixel. The pixel is still real -- it is `outer`'s own
       area, and `mid` clipping its subtree does not clip its parent -- so the hit
       must fall through to `outer`, **not** to the leaf and **not** to nothing. */
    CHECK(cgl_widget_hit_test(&outer, 55, 55) == &outer,
          "a point clipped away from a grandchild did not fall through to the "
          "clipping widget's parent");
}

static void test_hit_test_warped_subtree(void)
{
    cgl_widget_t page;
    cgl_widget_t inner;
    cgl_warp_t warp;

    cgl_widget_init(&page, NULL, "page", cgl_rect(0, 0, 319, 171));
    cgl_widget_init(&inner, NULL, "inner", cgl_rect(10, 10, 40, 40));
    CHECK(cgl_widget_add_child(&page, &inner) == true, "setup: inner not attached");

    /* Undeformed: ordinary hit. */
    CHECK(cgl_widget_hit_test(&page, 20, 20) == &inner, "setup: plain hit failed");

    /* Deformed: the pixels of this subtree are somewhere else now, and the walker
       has no inverse of the warp. It must decline rather than answer from the
       undeformed frame. */
    warp.src_y0 = 0;
    warp.src_h = 172;
    warp.cx = 160;
    warp.tx = 160;
    warp.ty = 172;
    warp.progress_q8 = 128u;
    warp.narrow_q8 = 64u;
    warp.row_step_q8 = (uint16_t)((256u * 256u) / 172u);
    warp.s_amp_q8 = 0u;
    cgl_widget_set_warp(&page, &warp);
    CHECK(cgl_widget_hit_test(&page, 20, 20) == NULL,
          "a warped subtree was still hit from its undeformed frame");

    /* Removing the warp restores ordinary hit testing: the safety net is not
       sticky. */
    cgl_widget_set_warp(&page, NULL);
    CHECK(cgl_widget_hit_test(&page, 20, 20) == &inner,
          "clearing the warp did not restore hit testing");
}

int main(void)
{
    printf("widget tree rules: child limit, clipping, deformation\n");

    test_child_limit();
    test_hit_test_clip();
    test_hit_test_nested_clip();
    test_hit_test_warped_subtree();

    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
