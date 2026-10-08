// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_fx_config.h
  * @brief   Which animation effects this build contains -- the application's side
  *          of the module's off switch.
  *
  *  Why this file exists
  *  --------------------
  *  The module has its own switch (`CGL_CFG_PARTICLES`, see
  *  `include/ext/cgl_particles_config.h`), and it removes the **code**. It cannot remove the
  *  **storage**: a page that declares a particle system as a static member still
  *  pays for those bytes when the engine is compiled out, because the bytes are the
  *  application's, not the module's. This header is where that decision lives.
  *
  *  What each switch actually buys, in this application
  *  ---------------------------------------------------
  *  | Switch | Effect | RAM back | Flash back |
  *  |---|---|---|---|
  *  | `APP_FX_PARTICLES 0` | no bullet time page: its widget tree, its HUD labels and its `cgl_particles_t` pointer all go | ~1.2 KB (`app_page_bullet_t`) | ~2.6 KB |
  *
  *  The particle pool itself (5.9 KB) is **borrowed** from the smash page's storage
  *  (see `smash_storage_t`) rather than owned by the bullet page, so switching the
  *  bullet page off does not return it: the smash page still needs those bytes, and
  *  they are the bigger of the two arms (a 12.7 KB `cgl_phys2d` world).
  *
  *  Combine it with the module switch when the effect is not wanted at all:
  *
  *      -DAPP_FX_PARTICLES=0 -DCGL_CFG_PARTICLES=0
  *
  *  `CGL_CFG_PARTICLES=0` alone would leave `app_page_bullet.c` calling functions
  *  that no longer exist, which is why this file also gates that translation unit:
  *  a build that collects its sources per directory (this repository's firmware
  *  build does) compiles `app_page_bullet.c` whether or not the page is registered.
  *
  *  The menu copes on its own: item "Bullet Time" is built from `ctx->app`
  *  (`menu__target()`), so a page that this build leaves out is simply an item that
  *  shows but does nothing -- the same state a host test that does not build the
  *  page is already in.
  ******************************************************************************
  */
#ifndef APP_FX_CONFIG_H
#define APP_FX_CONFIG_H

/* The module's own switches, so that the consistency checks below are possible here
   (the includes resolve through `include/`, which is on the application's include path). */
#include <ext/cgl_particles_config.h>
#include <ext/cgl_debris_config.h>
#include <ext/cgl_bounce_config.h>

/** 1 = the smash page (the shatter effect) is part of this build. **This is the
    switch that returns RAM**, because the smash page is the one that declares the
    shared storage (a 12.7 KB `cgl_phys2d` world plus its camera/sorter). */
#ifndef APP_FX_SMASH
#define APP_FX_SMASH 1
#endif

/** 1 = the bullet time page is part of this build; 0 = it is removed (page, table
    row, links entry and translation unit). See the cost table above. */
#ifndef APP_FX_PARTICLES
#define APP_FX_PARTICLES 1
#endif

/* --- the switches have to agree with each other and with the modules --------
 *
 * These are `#error`s rather than silent fallbacks on purpose: every combination
 * below produces a link error or, worse, a page reading bytes that belong to another
 * engine, and "the build stopped with a sentence" beats "the screen showed garbage".
 */
#if APP_FX_PARTICLES && !APP_FX_SMASH
#error "the bullet time page borrows the smash page's storage (smash_storage_t): APP_FX_PARTICLES=1 needs APP_FX_SMASH=1"
#endif
#if APP_FX_PARTICLES && !CGL_CFG_PARTICLES
#error "APP_FX_PARTICLES=1 needs the particle engine: drop the page (APP_FX_PARTICLES=0) or keep the engine (CGL_CFG_PARTICLES=1)"
#endif
#if APP_FX_SMASH && !CGL_CFG_DEBRIS
#error "APP_FX_SMASH=1 needs the shatter module: drop the page (APP_FX_SMASH=0) or keep the module (CGL_CFG_DEBRIS=1)"
#endif
#if APP_FX_SMASH && !CGL_CFG_BOUNCE
#error "APP_FX_SMASH=1 needs the gravity ball: drop the page (APP_FX_SMASH=0) or keep the effect (CGL_CFG_BOUNCE=1)"
#endif

#endif /* APP_FX_CONFIG_H */
