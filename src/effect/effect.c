/* ASE - Allegro Sprite Editor
 * Copyright (C) 2001-2008  David A. Capello
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "jinete/jmanager.h"
#include "jinete/jrect.h"
#include "jinete/jregion.h"
#include "jinete/jview.h"
#include "jinete/jwidget.h"

#include "console/console.h"
#include "core/cfg.h"
#include "effect/colcurve.h"
#include "effect/convmatr.h"
#include "effect/effect.h"
#include "effect/invrtcol.h"
#include "effect/median.h"
#include "effect/replcol.h"
#include "modules/editors.h"
#include "modules/sprites.h"
#include "raster/cel.h"
#include "raster/image.h"
#include "raster/layer.h"
#include "raster/mask.h"
#include "raster/sprite.h"
#include "raster/stock.h"
#include "raster/undo.h"
#include "util/misc.h"
#include "widgets/editor.h"

typedef struct EffectData
{
  const char *name;
  const char *label;
  void (*apply_4)(Effect *effect);
  void (*apply_2)(Effect *effect);
  void (*apply_1)(Effect *effect);
} EffectData;

#define FXDATA(name, label)						\
  { #name, label, apply_##name##4, apply_##name##2, apply_##name##1 }

static EffectData effects_data[] = {
  FXDATA(color_curve, "Color Curve"),
  FXDATA(convolution_matrix, "Convolution Matrix"),
  FXDATA(invert_color, "Invert Color"),
  FXDATA(median, "Median"),
  FXDATA(replace_color, "Replace Color"),
  { NULL, NULL, NULL, NULL }
};

static EffectData *get_effect_data(const char *name);
static int effect_init(Effect *effect, Layer *layer, Image *image, int offset_x, int offset_y);
static void effect_apply_to_image(Effect *effect, ImageRef *p, int x, int y);
static int effect_update_mask(Effect *effect, Mask *mask, Image *image);

int init_module_effect(void)
{
  init_convolution_matrix();
  return 0;
}

void exit_module_effect(void)
{
  exit_convolution_matrix();
}

Effect *effect_new(Sprite *sprite, const char *name)
{
  int offset_x, offset_y;
  EffectData *effect_data;
  Effect *effect;
  Image *image;
  void *apply;

  effect_data = get_effect_data(name);
  if (!effect_data)
    return NULL;

  apply = NULL;
  switch (sprite->imgtype) {
    case IMAGE_RGB:       apply = effect_data->apply_4; break;
    case IMAGE_GRAYSCALE: apply = effect_data->apply_2; break;
    case IMAGE_INDEXED:   apply = effect_data->apply_1; break;
  }
  if (!apply)
    return NULL;

  effect = jnew(Effect, 1);
  if (!effect)
    return NULL;

  effect->sprite = sprite;
  effect->src = NULL;
  effect->dst = NULL;
  effect->row = 0;
  effect->offset_x = 0;
  effect->offset_y = 0;
  effect->mask = NULL;
  effect->preview_mask = NULL;
  effect->mask_address = NULL;
  effect->effect_data = effect_data;
  effect->apply = apply;
  effect->_target = TARGET_ALL_CHANNELS;
  effect->target = TARGET_ALL_CHANNELS;
  effect->progress_data = NULL;
  effect->progress = NULL;
  effect->is_cancelled = NULL;

  image = GetImage2(sprite, &offset_x, &offset_y, NULL);
  if (image) {
    if (!effect_init(effect, sprite->layer, image, offset_x, offset_y)) {
      effect_free(effect);
      return NULL;
    }
  }

  return effect;
}

void effect_free(Effect *effect)
{
  if (effect->preview_mask)
    mask_free(effect->preview_mask);

  if (effect->dst)
    image_free(effect->dst);

  jfree(effect);
}

void effect_set_target(Effect *effect, int target)
{
  effect->_target = target;
  effect->target = target;

  /* the alpha channel of the background layer can't be modified */
  if (effect->sprite->layer &&
      layer_is_background(effect->sprite->layer))
    effect->target &= ~TARGET_ALPHA_CHANNEL;
}

void effect_begin(Effect *effect)
{
  effect->row = 0;
  effect->mask = effect->sprite->mask;

  effect_update_mask(effect, effect->mask, effect->src);
}

void effect_begin_for_preview(Effect *effect)
{
  if (effect->preview_mask) {
    mask_free(effect->preview_mask);
    effect->preview_mask = NULL;
  }

  if ((effect->sprite->mask) && (effect->sprite->mask->bitmap))
    effect->preview_mask = mask_new_copy(effect->sprite->mask);
  else {
    effect->preview_mask = mask_new();
    mask_replace(effect->preview_mask,
		 effect->offset_x, effect->offset_y,
		 effect->src->w, effect->src->h);
  }

  effect->row = 0;
  effect->mask = effect->preview_mask;

  {
    JWidget editor = current_editor;
    JRect vp = jview_get_viewport_position(jwidget_get_view(editor));
    int x1, y1, x2, y2;
    int x, y, w, h;

    screen_to_editor(editor, vp->x1, vp->y1, &x1, &y1);
    screen_to_editor(editor, vp->x2-1, vp->y2-1, &x2, &y2);

    jrect_free(vp);

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= effect->sprite->w) x2 = effect->sprite->w-1;
    if (y2 >= effect->sprite->h) y2 = effect->sprite->h-1;

    x = x1;
    y = y1;
    w = x2 - x1 + 1;
    h = y2 - y1 + 1;

    if ((w < 1) || (h < 1)) {
      mask_free(effect->preview_mask);
      effect->preview_mask = NULL;
      effect->row = -1;
      return;
    }

    mask_intersect(effect->preview_mask, x, y, w, h);
  }

  if (!effect_update_mask(effect, effect->mask, effect->src)) {
    mask_free(effect->preview_mask);
    effect->preview_mask = NULL;
    effect->row = -1;
    return;
  }
}

bool effect_apply_step(Effect *effect)
{
  if ((effect->row >= 0) && (effect->row < effect->h)) {
    if ((effect->mask) && (effect->mask->bitmap)) {
      effect->d = div(effect->x-effect->mask->x+effect->offset_x, 8);
      effect->mask_address =
	((ase_uint8 **)effect->mask->bitmap->line)
	[effect->row+effect->y-effect->mask->y+effect->offset_y]+effect->d.quot;
    }
    else
      effect->mask_address = NULL;

    (*effect->apply)(effect);
    effect->row++;
    return TRUE;
  }
  else {
    return FALSE;
  }
}

void effect_apply(Effect *effect)
{
  bool cancelled = FALSE;

  effect_begin(effect);
  while (!cancelled && effect_apply_step(effect)) {
    if (effect->progress != NULL)
      (effect->progress)(effect->progress_data,
			 effect->progress_base
			 + effect->progress_width * (effect->row+1) / effect->h);

    if (effect->is_cancelled != NULL)
      cancelled = (effect->is_cancelled)(effect->progress_data);
  }

  if (!cancelled) {
    /* undo stuff */
    if (undo_is_enabled(effect->sprite->undo)) {
      undo_set_label(effect->sprite->undo,
		     effect->effect_data->label);
      undo_image(effect->sprite->undo, effect->src,
		 effect->x, effect->y, effect->w, effect->h);
    }

    /* copy "dst" to "src" */
    image_copy(effect->src, effect->dst, 0, 0);
  }
}


/**
 * Updates the current editor to show the progress of the preview.
 */
void effect_flush(Effect *effect)
{
  if (effect->row >= 0) {
    JRegion reg1, reg2;
    struct jrect rect;
    JWidget editor;

    editor = current_editor;
    reg1 = jregion_new(NULL, 0);

    editor_to_screen(editor,
		     effect->x+effect->offset_x,
		     effect->y+effect->offset_y+effect->row-1,
		     &rect.x1, &rect.y1);
    rect.x2 = rect.x1 + (effect->w << editor_data(editor)->zoom);
    rect.y2 = rect.y1 + (1 << editor_data(editor)->zoom);

    reg2 = jregion_new(&rect, 1);
    jregion_union(reg1, reg1, reg2);
    jregion_free(reg2);

    reg2 = jwidget_get_drawable_region(editor, JI_GDR_CUTTOPWINDOWS);
    jregion_intersect(reg1, reg1, reg2);
    jregion_free(reg2);

    jwidget_redraw_region(editor, reg1);
    jregion_free(reg1);
  }
}

void effect_apply_to_target(Effect *effect)
{
  ImageRef *p, *images;
  bool cancelled = FALSE;
  int nimages;

  images = images_ref_get_from_sprite(effect->sprite, effect->target, TRUE);
  if (images == NULL)
    return;

  nimages = 0;
  for (p=images; p; p=p->next)
    nimages++;

  /* open undo group of operations */
  if (nimages > 1) {
    if (undo_is_enabled(effect->sprite->undo))
      undo_open(effect->sprite->undo);
  }
  
  effect->progress_base = 0.0f;
  effect->progress_width = 1.0f / nimages;

  /* for each target image */
  for (p=images; p && !cancelled; p=p->next) {
    effect_apply_to_image(effect, p, p->cel->x, p->cel->y);

    /* there is a 'is_cancelled' hook? */
    if (effect->is_cancelled != NULL)
      cancelled = (effect->is_cancelled)(effect->progress_data);

    /* progress */
    effect->progress_base += effect->progress_width;
  }

  /* close undo group of operations */
  if (nimages > 1) {
    if (undo_is_enabled(effect->sprite->undo))
      undo_close(effect->sprite->undo);
  }

  /* free all ImageRefs */
  images_ref_free(images);
}

static EffectData *get_effect_data(const char *name)
{
  int c;

  for (c=0; effects_data[c].name; c++) {
    if (strcmp(effects_data[c].name, name) == 0)
      return effects_data+c;
  }

  return NULL;
}

static int effect_init(Effect *effect, Layer *layer, Image *image,
		       int offset_x, int offset_y)
{
  effect->offset_x = offset_x;
  effect->offset_y = offset_y;

  if (!effect_update_mask(effect, effect->sprite->mask, image))
    return FALSE;

  if (effect->preview_mask) {
    mask_free(effect->preview_mask);
    effect->preview_mask = NULL;
  }

  if (effect->dst) {
    image_free(effect->dst);
    effect->dst = NULL;
  }

  effect->src = image;
  effect->dst = image_crop(image, 0, 0, image->w, image->h, 0);
  effect->row = -1;
  effect->mask = NULL;
  effect->preview_mask = NULL;
  effect->mask_address = NULL;

  effect->target = effect->_target;

  /* the alpha channel of the background layer can't be modified */
  if (layer_is_background(layer))
    effect->target &= ~TARGET_ALPHA_CHANNEL;
  
  return TRUE;
}

static void effect_apply_to_image(Effect *effect, ImageRef *p, int x, int y)
{
  if (effect_init(effect, p->layer, p->image, x, y))
    effect_apply(effect);
}

static int effect_update_mask(Effect *effect, Mask *mask, Image *image)
{
  int x, y, w, h;

  if ((mask) && (mask->bitmap)) {
    x = mask->x - effect->offset_x;
    y = mask->y - effect->offset_y;
    w = mask->w;
    h = mask->h;

    if (x < 0) {
      w += x;
      x = 0;
    }

    if (y < 0) {
      h += y;
      y = 0;
    }

    if (x+w-1 >= image->w-1)
      w = image->w-x;

    if (y+h-1 >= image->h-1)
      h = image->h-y;
  }
  else {
    x = 0;
    y = 0;
    w = image->w;
    h = image->h;
  }

  if ((w < 1) || (h < 1)) {
    effect->x = 0;
    effect->y = 0;
    effect->w = 0;
    effect->h = 0;
    return FALSE;
  }
  else {
    effect->x = x;
    effect->y = y;
    effect->w = w;
    effect->h = h;
    return TRUE;
  }
}
