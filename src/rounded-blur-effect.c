/* rounded-blur-effect.c
 *
 * Copyright 2025 GNOME Rounded Blur
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rounded-blur-effect.h"

//#include <cogl/cogl.h>

//#include "bsm-enum-types.h"

static const gchar *brightness_glsl_declarations =
"uniform float brightness;                                                 \n";

static const gchar *brightness_glsl =
"  cogl_color_out.rgb *= brightness;                                       \n";

static const gchar *mask_glsl_declarations =
"uniform float u_corner_radius;                                            \n"
"uniform vec2  u_size;                                                     \n";

static const gchar *mask_glsl =
"  vec2 uv = cogl_tex_coord_in[0].st;                                      \n"
"  vec2 size = max(u_size, vec2(1.0));                                     \n"
"  vec2 half_size = max(size * 0.5, vec2(0.5));                            \n"
"  float radius = clamp(u_corner_radius,                                   \n"
"                       0.0,                                               \n"
"                       max(min(half_size.x, half_size.y) - 0.5, 0.0));    \n"
"  float m = 1.0;                                                          \n"
"  if (radius > 0.0)                                                       \n"
"    {                                                                     \n"
"      vec2 p = uv * size;                                                 \n"
"      vec2 q = abs(p - half_size) - max(half_size - radius, vec2(0.0));   \n"
"      float dist = length(max(q, vec2(0.0))) - radius;                    \n"
"      float aa = max(fwidth(dist) * 0.5, 0.5);                            \n"
"      m = 1.0 - smoothstep(-aa, aa, dist);                                \n"
"    }                                                                     \n"
"  cogl_color_out.rgb *= m;                                                \n"
"  cogl_color_out.a   *= m;                                                \n";

#define MIN_DOWNSCALE_SIZE 256.f
#define MAX_RADIUS 12.f

typedef enum
{
  ACTOR_PAINTED = 1 << 0,
  BLUR_APPLIED = 1 << 1,
} CacheFlags;

typedef struct
{
  CoglFramebuffer *framebuffer;
  CoglPipeline *pipeline;
  CoglTexture *texture;
} FramebufferData;

struct _GbBlurEffect
{
  ClutterEffect parent_instance;

  ClutterActor *actor;

  unsigned int tex_width;
  unsigned int tex_height;

  /* The cached contents */
  FramebufferData actor_fb;
  CacheFlags cache_flags;

  FramebufferData background_fb;
  FramebufferData brightness_fb;
  int brightness_uniform;

  FramebufferData mask_fb;
  int corner_radius_uniform;
  int mask_size_uniform;

  GbBlurMode mode;
  float downscale_factor;
  float brightness;
  int radius;

  float corner_radius;

  /* Not owned: borrowed from the backend of the actor's ClutterContext. */
  CoglContext *cogl_context;
};

G_DEFINE_TYPE (GbBlurEffect, gb_blur_effect, CLUTTER_TYPE_EFFECT)

enum {
  PROP_0,
  PROP_RADIUS,
  PROP_BRIGHTNESS,
  PROP_MODE,
  PROP_CORNER_RADIUS,
  N_PROPS
};

static GParamSpec *properties [N_PROPS] = { NULL, };

/* clutter_get_default_backend() was dropped in mutter/clutter 51, so fetch the
 * backend through the ClutterContext owned by the actor instead. The
 * paint-context framebuffer is used as a fallback. */
static CoglContext *
get_cogl_context (GbBlurEffect        *self,
                  ClutterPaintContext *paint_context)
{
  ClutterContext *clutter_context;
  ClutterBackend *backend;
  CoglContext *ctx = NULL;

  if (self->actor)
    {
      clutter_context = clutter_actor_get_context (self->actor);
      if (clutter_context)
        {
          backend = clutter_context_get_backend (clutter_context);
          if (backend)
            ctx = clutter_backend_get_cogl_context (backend);
        }
    }

  if (!ctx && paint_context)
    {
      CoglFramebuffer *framebuffer =
        clutter_paint_context_get_framebuffer (paint_context);

      if (framebuffer)
        ctx = cogl_framebuffer_get_context (framebuffer);
    }

  return ctx;
}

static CoglPipeline*
create_base_pipeline (CoglContext *ctx)
{
  static CoglPipeline *base_pipeline = NULL;

  if (G_UNLIKELY (base_pipeline == NULL))
    {
      g_return_val_if_fail (ctx != NULL, NULL);

      base_pipeline = cogl_pipeline_new (ctx);
      cogl_pipeline_set_layer_null_texture (base_pipeline, 0);
      cogl_pipeline_set_layer_filters (base_pipeline,
                                       0,
                                       COGL_PIPELINE_FILTER_LINEAR,
                                       COGL_PIPELINE_FILTER_LINEAR);
      cogl_pipeline_set_layer_wrap_mode (base_pipeline,
                                         0,
                                         COGL_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE);
    }

  return cogl_pipeline_copy (base_pipeline);
}

static CoglPipeline*
create_brightness_pipeline (CoglContext *ctx)
{
  static CoglPipeline *brightness_pipeline = NULL;

  if (G_UNLIKELY (brightness_pipeline == NULL))
    {
      CoglSnippet *snippet;

      brightness_pipeline = create_base_pipeline (ctx);

      snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                                  brightness_glsl_declarations,
                                  brightness_glsl);
      cogl_pipeline_add_snippet (brightness_pipeline, snippet);
      g_object_unref (snippet);
    }

  return cogl_pipeline_copy (brightness_pipeline);
}

static CoglPipeline*
create_mask_pipeline (CoglContext *ctx)
{
  static CoglPipeline *mask_pipeline = NULL;

  if (G_UNLIKELY (mask_pipeline == NULL))
  {
    CoglSnippet *snippet;
    mask_pipeline = create_base_pipeline (ctx);
    snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                                mask_glsl_declarations,
                                mask_glsl);
    cogl_pipeline_add_snippet (mask_pipeline, snippet);
    g_object_unref (snippet);
  }

  return cogl_pipeline_copy (mask_pipeline);
}

static void
update_brightness (GbBlurEffect *self,
                   uint8_t        paint_opacity)
{
  CoglColor color;

  if (!self->brightness_fb.pipeline)
    return;

  cogl_color_init_from_4f (&color,
                           paint_opacity / 255.0, paint_opacity / 255.0,
                           paint_opacity / 255.0, paint_opacity / 255.0);
  cogl_pipeline_set_color (self->brightness_fb.pipeline, &color);

  if (self->brightness_uniform > -1)
    {
      cogl_pipeline_set_uniform_1f (self->brightness_fb.pipeline,
                                    self->brightness_uniform,
                                    self->brightness);
    }
}

static void
update_mask_uniforms (GbBlurEffect *self,
                      float          width,
                      float          height)
{
  if (!self->mask_fb.pipeline)
    return;

  if (self->corner_radius_uniform > -1)
    cogl_pipeline_set_uniform_1f (self->mask_fb.pipeline,
                                  self->corner_radius_uniform,
                                  self->corner_radius);

  if (self->mask_size_uniform > -1)
  {
    const float size[2] = { width, height };
    cogl_pipeline_set_uniform_float (self->mask_fb.pipeline,
                                     self->mask_size_uniform,
                                     2, 1, size);
  }
}

static void
setup_projection_matrix (CoglFramebuffer *framebuffer,
                         float            width,
                         float            height)
{
  graphene_matrix_t projection;

  graphene_matrix_init_translate (&projection,
                                  &GRAPHENE_POINT3D_INIT (-width / 2.0,
                                                          -height / 2.0,
                                                          0.f));
  graphene_matrix_scale (&projection, 2.0 / width, -2.0 / height, 1.f);

  cogl_framebuffer_set_projection_matrix (framebuffer, &projection);
}

static gboolean
update_fbo (CoglContext     *ctx,
            FramebufferData *data,
            unsigned int     width,
            unsigned int     height,
            float            downscale_factor)
{
  if (!ctx)
    return FALSE;



  g_clear_object (&data->texture);
  g_clear_object (&data->framebuffer);

  float new_width = floorf (width / downscale_factor);
  float new_height = floorf (height / downscale_factor);

  data->texture = cogl_texture_2d_new_with_size (ctx, new_width, new_height);
  if (!data->texture)
    return FALSE;

  cogl_pipeline_set_layer_texture (data->pipeline, 0, data->texture);

  data->framebuffer =
    COGL_FRAMEBUFFER (cogl_offscreen_new_with_texture (data->texture));
  if (!data->framebuffer)
    {
      g_warning ("%s: Unable to create an Offscreen buffer", G_STRLOC);
      return FALSE;
    }

  setup_projection_matrix (data->framebuffer, new_width, new_height);

  return TRUE;
}

static gboolean
update_actor_fbo (GbBlurEffect *self,
                  unsigned int   width,
                  unsigned int   height,
                  float          downscale_factor)
{
  if (self->tex_width == width &&
      self->tex_height == height &&
      self->downscale_factor == downscale_factor &&
      self->actor_fb.framebuffer)
    {
      return TRUE;
    }

  self->cache_flags &= ~ACTOR_PAINTED;

  return update_fbo (self->cogl_context,
                     &self->actor_fb, width, height, downscale_factor);
}

static gboolean
update_brightness_fbo (GbBlurEffect *self,
                       unsigned int   width,
                       unsigned int   height,
                       float          downscale_factor)
{
  if (self->tex_width == width &&
      self->tex_height == height &&
      self->downscale_factor == downscale_factor &&
      self->brightness_fb.framebuffer)
    {
      return TRUE;
    }

  return update_fbo (self->cogl_context,
                     &self->brightness_fb,
                     width, height,
                     downscale_factor);
}

static gboolean
update_background_fbo (GbBlurEffect *self,
                       unsigned int   width,
                       unsigned int   height)
{
  if (self->tex_width == width &&
      self->tex_height == height &&
      self->background_fb.framebuffer)
    {
      return TRUE;
    }

  return update_fbo (self->cogl_context,
                     &self->background_fb, width, height, 1.0);
}

static gboolean
update_mask_fbo (GbBlurEffect *self,
                 unsigned int   width,
                 unsigned int   height,
                 float          downscale_factor)
{
  if (self->tex_width == width &&
      self->tex_height == height &&
      self->downscale_factor == downscale_factor &&
      self->mask_fb.framebuffer)
  {
    return TRUE;
  }

  return update_fbo (self->cogl_context,
                     &self->mask_fb, width, height, downscale_factor);
}

static void
clear_framebuffer_data (FramebufferData *fb_data)
{
  g_clear_object (&fb_data->texture);
  g_clear_object (&fb_data->framebuffer);
}

static float
calculate_downscale_factor (float width,
                            float height,
                            float radius)
{
  float downscale_factor = 1.0;
  float scaled_width = width;
  float scaled_height = height;
  float scaled_radius = radius;

  /* This is the algorithm used by Firefox; keep downscaling until either the
   * blur radius is lower than the threshold, or the downscaled texture is too
   * small.
   */
  while (scaled_radius > MAX_RADIUS &&
         scaled_width > MIN_DOWNSCALE_SIZE &&
         scaled_height > MIN_DOWNSCALE_SIZE)
    {
      downscale_factor *= 2.f;

      scaled_width = width / downscale_factor;
      scaled_height = height / downscale_factor;
      scaled_radius = radius / downscale_factor;
    }

  return downscale_factor;
}

static void
gb_blur_effect_set_actor (ClutterActorMeta *meta,
                           ClutterActor     *actor)
{
  GbBlurEffect *self = GB_BLUR_EFFECT (meta);
  ClutterActorMetaClass *meta_class;

  meta_class = CLUTTER_ACTOR_META_CLASS (gb_blur_effect_parent_class);
  meta_class->set_actor (meta, actor);

  /* clear out the previous state */
  clear_framebuffer_data (&self->actor_fb);
  clear_framebuffer_data (&self->background_fb);
  clear_framebuffer_data (&self->brightness_fb);
  clear_framebuffer_data (&self->mask_fb);

  /* we keep a back pointer here, to avoid going through the ActorMeta */
  self->actor = clutter_actor_meta_get_actor (meta);
}

static void
update_actor_box (GbBlurEffect       *self,
                  ClutterPaintContext *paint_context,
                  ClutterActorBox     *source_actor_box)
{
  ClutterStageView *stage_view;
  float box_scale_factor = 1.0f;
  float origin_x, origin_y;
  float width, height;

  switch (self->mode)
    {
    case GB_BLUR_MODE_ACTOR:
      clutter_actor_get_allocation_box (self->actor, source_actor_box);
      break;

    case GB_BLUR_MODE_BACKGROUND:
      stage_view = clutter_paint_context_get_stage_view (paint_context);

      clutter_actor_get_transformed_position (self->actor, &origin_x, &origin_y);
      clutter_actor_get_transformed_size (self->actor, &width, &height);

      if (stage_view)
        {
          MtkRectangle stage_view_layout;

          box_scale_factor = clutter_stage_view_get_scale (stage_view);
          clutter_stage_view_get_layout (stage_view, &stage_view_layout);

          origin_x -= stage_view_layout.x;
          origin_y -= stage_view_layout.y;
        }
      else
        {
          /* If we're drawing off stage, just assume scale = 1, this won't work
           * with stage-view scaling though.
           */
        }

      clutter_actor_box_set_origin (source_actor_box, origin_x, origin_y);
      clutter_actor_box_set_size (source_actor_box, width, height);

      clutter_actor_box_scale (source_actor_box, box_scale_factor);
      break;
    }

  clutter_actor_box_clamp_to_pixel (source_actor_box);
}

static void
add_blurred_pipeline (GbBlurEffect    *self,
                      ClutterPaintNode *node,
                      uint8_t           paint_opacity)
{
  g_autoptr (ClutterPaintNode) pipeline_node = NULL;
  float width, height;

  /* Use the untransformed actor size here, since the framebuffer itself already
   * has the actor transform matrix applied.
   */
  clutter_actor_get_size (self->actor, &width, &height);

  update_brightness (self, paint_opacity);
  update_mask_uniforms (self, width, height);

  pipeline_node = clutter_pipeline_node_new (self->mask_fb.pipeline);
  clutter_paint_node_set_static_name (pipeline_node, "GbBlurEffect (final)");
  clutter_paint_node_add_child (node, pipeline_node);

  clutter_paint_node_add_rectangle (pipeline_node,
                                    &(ClutterActorBox) {
                                      0.f, 0.f,
                                      width,
                                      height,
                                    });
}

static ClutterPaintNode *
create_blur_nodes (GbBlurEffect    *self,
                   ClutterPaintNode *node,
                   uint8_t           paint_opacity)
{
  g_autoptr (ClutterPaintNode) brightness_node = NULL;
  g_autoptr (ClutterPaintNode) blur_node = NULL;
  g_autoptr (ClutterPaintNode) mask_node = NULL;
  float width;
  float height;

  clutter_actor_get_size (self->actor, &width, &height);

  update_mask_uniforms (self, width, height);
  mask_node = clutter_layer_node_new_to_framebuffer (self->mask_fb.framebuffer,
                                                     self->mask_fb.pipeline);
  clutter_paint_node_set_static_name (mask_node, "ShellBlurEffect (mask)");
  clutter_paint_node_add_child (node, mask_node);
  clutter_paint_node_add_rectangle (mask_node,
                                    &(ClutterActorBox) {
                                      0.f, 0.f,
                                      width, height,
                                    });

  update_brightness (self, paint_opacity);
  brightness_node = clutter_layer_node_new_to_framebuffer (self->brightness_fb.framebuffer,
                                                           self->brightness_fb.pipeline);
  clutter_paint_node_set_static_name (brightness_node, "ShellBlurEffect (brightness)");
  clutter_paint_node_add_child (mask_node, brightness_node);
  clutter_paint_node_add_rectangle (brightness_node,
                                    &(ClutterActorBox) {
                                      0.f, 0.f,
                                      cogl_texture_get_width (self->mask_fb.texture),
                                      cogl_texture_get_height (self->mask_fb.texture),
                                    });

  blur_node = clutter_blur_node_new (self->tex_width / self->downscale_factor,
                                     self->tex_height / self->downscale_factor,
                                     self->radius / self->downscale_factor);
  clutter_paint_node_set_static_name (blur_node, "ShellBlurEffect (blur)");
  clutter_paint_node_add_child (brightness_node, blur_node);
  clutter_paint_node_add_rectangle (blur_node,
                                    &(ClutterActorBox) {
                                      0.f, 0.f,
                                      cogl_texture_get_width (self->mask_fb.texture),
                                      cogl_texture_get_height (self->mask_fb.texture),
                                    });

  self->cache_flags |= BLUR_APPLIED;

  return g_steal_pointer (&blur_node);
}

static void
paint_background (GbBlurEffect      *self,
                  ClutterPaintNode    *node,
                  ClutterPaintContext *paint_context,
                  ClutterActorBox     *source_actor_box)
{
  g_autoptr (ClutterPaintNode) background_node = NULL;
  g_autoptr (ClutterPaintNode) blit_node = NULL;
  CoglFramebuffer *src;
  float transformed_x;
  float transformed_y;
  float transformed_width;
  float transformed_height;

  clutter_actor_box_get_origin (source_actor_box,
                                &transformed_x,
                                &transformed_y);
  clutter_actor_box_get_size (source_actor_box,
                              &transformed_width,
                              &transformed_height);

  /* Background layer node */
  background_node =
    clutter_layer_node_new_to_framebuffer (self->background_fb.framebuffer,
                                           self->background_fb.pipeline);
  clutter_paint_node_set_static_name (background_node, "GbBlurEffect (background)");
  clutter_paint_node_add_child (node, background_node);
  clutter_paint_node_add_rectangle (background_node,
                                    &(ClutterActorBox) {
                                      0.f, 0.f,
                                      self->tex_width / self->downscale_factor,
                                      self->tex_height / self->downscale_factor,
                                    });

  /* Blit node */
  src = clutter_paint_context_get_framebuffer (paint_context);
  blit_node = clutter_blit_node_new (src);
  clutter_paint_node_set_static_name (blit_node, "GbBlurEffect (blit)");
  clutter_paint_node_add_child (background_node, blit_node);
  clutter_blit_node_add_blit_rectangle (CLUTTER_BLIT_NODE (blit_node),
                                        transformed_x,
                                        transformed_y,
                                        0, 0,
                                        transformed_width,
                                        transformed_height);
}

/* Pipelines used to be built in init(), but the CoglContext is only reachable
 * once the effect is attached to an actor, so build them on first paint. */
static gboolean
ensure_pipelines (GbBlurEffect        *self,
                  ClutterPaintContext *paint_context)
{
  if (G_LIKELY (self->actor_fb.pipeline &&
                self->background_fb.pipeline &&
                self->brightness_fb.pipeline &&
                self->mask_fb.pipeline))
    return TRUE;

  if (!self->cogl_context)
    self->cogl_context = get_cogl_context (self, paint_context);

  if (!self->cogl_context)
    return FALSE;

  g_clear_object (&self->actor_fb.pipeline);
  g_clear_object (&self->background_fb.pipeline);
  g_clear_object (&self->brightness_fb.pipeline);
  g_clear_object (&self->mask_fb.pipeline);

  self->actor_fb.pipeline = create_base_pipeline (self->cogl_context);
  self->background_fb.pipeline = create_base_pipeline (self->cogl_context);
  self->brightness_fb.pipeline = create_brightness_pipeline (self->cogl_context);
  self->mask_fb.pipeline = create_mask_pipeline (self->cogl_context);

  if (!self->actor_fb.pipeline || !self->background_fb.pipeline ||
      !self->brightness_fb.pipeline || !self->mask_fb.pipeline)
    return FALSE;

  self->brightness_uniform =
    cogl_pipeline_get_uniform_location (self->brightness_fb.pipeline, "brightness");
  self->corner_radius_uniform =
    cogl_pipeline_get_uniform_location (self->mask_fb.pipeline, "u_corner_radius");
  self->mask_size_uniform =
    cogl_pipeline_get_uniform_location (self->mask_fb.pipeline, "u_size");

  return TRUE;
}

static gboolean
update_framebuffers (GbBlurEffect       *self,
                     ClutterPaintContext *paint_context,
                     ClutterActorBox     *source_actor_box)
{
  gboolean updated = FALSE;
  float downscale_factor;
  float height = -1;
  float width = -1;

  clutter_actor_box_get_size (source_actor_box, &width, &height);

  downscale_factor = calculate_downscale_factor (width, height, self->radius);

  updated = update_actor_fbo (self, width, height, downscale_factor) &&
            update_brightness_fbo (self, width, height, downscale_factor) &&
            update_mask_fbo (self, width, height, downscale_factor);

  if (self->mode == GB_BLUR_MODE_BACKGROUND)
    updated = updated && update_background_fbo (self, width, height);

  self->tex_width = width;
  self->tex_height = height;
  self->downscale_factor = downscale_factor;

  return updated;
}

static void
add_actor_node (GbBlurEffect    *self,
                ClutterPaintNode *node,
                int               opacity)
{
  g_autoptr (ClutterPaintNode) actor_node = NULL;

  actor_node = clutter_actor_node_new (self->actor, opacity);
  clutter_paint_node_add_child (node, actor_node);
}

static void
paint_actor_offscreen (GbBlurEffect           *self,
                       ClutterPaintNode        *node,
                       ClutterEffectPaintFlags  flags)
{
  gboolean actor_dirty;

  actor_dirty = (flags & CLUTTER_EFFECT_PAINT_ACTOR_DIRTY) != 0;

  /* The actor offscreen framebuffer is updated already */
  if (actor_dirty || !(self->cache_flags & ACTOR_PAINTED))
    {
      g_autoptr (ClutterPaintNode) transform_node = NULL;
      g_autoptr (ClutterPaintNode) layer_node = NULL;
      graphene_matrix_t transform;

      /* Layer node */
      layer_node = clutter_layer_node_new_to_framebuffer (self->actor_fb.framebuffer,
                                                          self->actor_fb.pipeline);
      clutter_paint_node_set_static_name (layer_node, "GbBlurEffect (actor offscreen)");
      clutter_paint_node_add_child (node, layer_node);
      clutter_paint_node_add_rectangle (layer_node,
                                        &(ClutterActorBox) {
                                          0.f, 0.f,
                                          self->tex_width / self->downscale_factor,
                                          self->tex_height / self->downscale_factor,
                                        });

      /* Transform node */
      graphene_matrix_init_scale (&transform,
                                  1.f / self->downscale_factor,
                                  1.f / self->downscale_factor,
                                  1.f);
      transform_node = clutter_transform_node_new (&transform);
      clutter_paint_node_set_static_name (transform_node, "GbBlurEffect (downscale)");
      clutter_paint_node_add_child (layer_node, transform_node);

      /* Actor node */
      add_actor_node (self, transform_node, 255);

      self->cache_flags |= ACTOR_PAINTED;
    }
  else
    {
      g_autoptr (ClutterPaintNode) pipeline_node = NULL;

      pipeline_node = clutter_pipeline_node_new (self->actor_fb.pipeline);
      clutter_paint_node_set_static_name (pipeline_node,
                                          "GbBlurEffect (actor texture)");
      clutter_paint_node_add_child (node, pipeline_node);
      clutter_paint_node_add_rectangle (pipeline_node,
                                        &(ClutterActorBox) {
                                          0.f, 0.f,
                                          self->tex_width / self->downscale_factor,
                                          self->tex_height / self->downscale_factor,
                                        });
    }
}

static gboolean
needs_repaint (GbBlurEffect         *self,
               ClutterEffectPaintFlags  flags)
{
  gboolean actor_cached;
  gboolean blur_cached;
  gboolean actor_dirty;

  actor_dirty = (flags & CLUTTER_EFFECT_PAINT_ACTOR_DIRTY) != 0;
  blur_cached = (self->cache_flags & BLUR_APPLIED) != 0;
  actor_cached = (self->cache_flags & ACTOR_PAINTED) != 0;

  switch (self->mode)
    {
    case GB_BLUR_MODE_ACTOR:
      return actor_dirty || !blur_cached || !actor_cached;

    case GB_BLUR_MODE_BACKGROUND:
      return TRUE;
    }

  return TRUE;
}

static void
gb_blur_effect_paint_node (ClutterEffect           *effect,
                              ClutterPaintNode        *node,
                              ClutterPaintContext     *paint_context,
                              ClutterEffectPaintFlags  flags)
{
  GbBlurEffect *self = GB_BLUR_EFFECT (effect);
  uint8_t paint_opacity;

  g_assert (self->actor != NULL);

  if (!ensure_pipelines (self, paint_context))
    goto fail;

  if (self->radius > 0)
    {
      g_autoptr (ClutterPaintNode) blur_node = NULL;

      switch (self->mode)
        {
        case GB_BLUR_MODE_ACTOR:
          paint_opacity = clutter_actor_get_paint_opacity (self->actor);
          break;

        case GB_BLUR_MODE_BACKGROUND:
          paint_opacity = 255;
          break;

        default:
          g_assert_not_reached();
          break;
        }

      if (needs_repaint (self, flags))
        {
          ClutterActorBox source_actor_box;

          update_actor_box (self, paint_context, &source_actor_box);

          /* Failing to create or update the offscreen framebuffers prevents
           * the entire effect to be applied.
           */
          if (!update_framebuffers (self, paint_context, &source_actor_box))
            goto fail;

          blur_node = create_blur_nodes (self, node, paint_opacity);

          switch (self->mode)
            {
            case GB_BLUR_MODE_ACTOR:
              paint_actor_offscreen (self, blur_node, flags);
              break;

            case GB_BLUR_MODE_BACKGROUND:
              paint_background (self, blur_node, paint_context, &source_actor_box);
              break;
            }
        }
      else
        {
          /* Use the cached pipeline if no repaint is needed */
          add_blurred_pipeline (self, node, paint_opacity);
        }

      /* Background blur needs to paint the actor after painting the blurred
       * background.
       */
      switch (self->mode)
        {
        case GB_BLUR_MODE_ACTOR:
          break;

        case GB_BLUR_MODE_BACKGROUND:
          add_actor_node (self, node, -1);
          break;
        }

      return;
    }

fail:
  /* When no blur is applied, or the offscreen framebuffers
   * couldn't be created, fallback to simply painting the actor.
   */
  add_actor_node (self, node, -1);
}

static void
gb_blur_effect_finalize (GObject *object)
{
  GbBlurEffect *self = (GbBlurEffect *)object;

  clear_framebuffer_data (&self->actor_fb);
  clear_framebuffer_data (&self->background_fb);
  clear_framebuffer_data (&self->brightness_fb);
  clear_framebuffer_data (&self->mask_fb);

  g_clear_object (&self->actor_fb.pipeline);
  g_clear_object (&self->background_fb.pipeline);
  g_clear_object (&self->brightness_fb.pipeline);
  g_clear_object (&self->mask_fb.pipeline);

  G_OBJECT_CLASS (gb_blur_effect_parent_class)->finalize (object);
}

static void
gb_blur_effect_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GbBlurEffect *self = GB_BLUR_EFFECT (object);

  switch (prop_id)
    {
    case PROP_RADIUS:
      g_value_set_int (value, self->radius);
      break;

    case PROP_BRIGHTNESS:
      g_value_set_float (value, self->brightness);
      break;

    case PROP_MODE:
      g_value_set_enum (value, self->mode);
      break;

    case PROP_CORNER_RADIUS:
      g_value_set_float (value, self->corner_radius);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_blur_effect_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GbBlurEffect *self = GB_BLUR_EFFECT (object);

  switch (prop_id)
    {
    case PROP_RADIUS:
      gb_blur_effect_set_radius (self, g_value_get_int (value));
      break;

    case PROP_BRIGHTNESS:
      gb_blur_effect_set_brightness (self, g_value_get_float (value));
      break;

    case PROP_MODE:
      gb_blur_effect_set_mode (self, g_value_get_enum (value));
      break;

    case PROP_CORNER_RADIUS:
      gb_blur_effect_set_corner_radius (self, g_value_get_float (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_blur_effect_class_init (GbBlurEffectClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorMetaClass *meta_class = CLUTTER_ACTOR_META_CLASS (klass);
  ClutterEffectClass *effect_class = CLUTTER_EFFECT_CLASS (klass);

  object_class->finalize = gb_blur_effect_finalize;
  object_class->get_property = gb_blur_effect_get_property;
  object_class->set_property = gb_blur_effect_set_property;

  meta_class->set_actor = gb_blur_effect_set_actor;

  effect_class->paint_node = gb_blur_effect_paint_node;

  properties[PROP_RADIUS] =
    g_param_spec_int ("radius", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_BRIGHTNESS] =
    g_param_spec_float ("brightness", NULL, NULL,
                        0.f, 1.f, 1.f,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_MODE] =
    g_param_spec_enum ("mode", NULL, NULL,
                       GB_TYPE_BLUR_MODE,
                       GB_BLUR_MODE_ACTOR,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_CORNER_RADIUS] =
    g_param_spec_float ("corner-radius", NULL, NULL,
                        0.f, G_MAXFLOAT, 0.f,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gb_blur_effect_init (GbBlurEffect *self)
{
  self->mode = GB_BLUR_MODE_ACTOR;
  self->radius = 0;
  self->brightness = 1.f;
  self->corner_radius = 0.f;

  /* The pipelines are created lazily in ensure_pipelines(), once the effect
   * has an actor and the CoglContext can be reached. */
  self->brightness_uniform = -1;
  self->corner_radius_uniform = -1;
  self->mask_size_uniform = -1;
}

GbBlurEffect *
gb_blur_effect_new (void)
{
  return g_object_new (GB_TYPE_BLUR_EFFECT, NULL);
}

int
gb_blur_effect_get_radius (GbBlurEffect *self)
{
  g_return_val_if_fail (GB_IS_BLUR_EFFECT (self), 0);

  return self->radius;
}

void
gb_blur_effect_set_radius (GbBlurEffect *self,
                              int           radius)
{
  g_return_if_fail (GB_IS_BLUR_EFFECT (self));

  if (self->radius == radius)
    return;

  self->radius = radius;
  self->cache_flags &= ~BLUR_APPLIED;

  if (self->actor)
    clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_RADIUS]);
}

float
gb_blur_effect_get_brightness (GbBlurEffect *self)
{
  g_return_val_if_fail (GB_IS_BLUR_EFFECT (self), -1);

  return self->brightness;
}

void
gb_blur_effect_set_brightness (GbBlurEffect *self,
                                  float            brightness)
{
  g_return_if_fail (GB_IS_BLUR_EFFECT (self));

  if (self->brightness == brightness)
    return;

  self->brightness = brightness;
  self->cache_flags &= ~BLUR_APPLIED;

  if (self->actor)
    clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_BRIGHTNESS]);
}

GbBlurMode
gb_blur_effect_get_mode (GbBlurEffect *self)
{
  g_return_val_if_fail (GB_IS_BLUR_EFFECT (self), GB_BLUR_MODE_ACTOR);

  return self->mode;
}

void
gb_blur_effect_set_mode (GbBlurEffect *self,
                            GbBlurMode    mode)
{
  g_return_if_fail (GB_IS_BLUR_EFFECT (self));

  if (self->mode == mode)
    return;

  self->mode = mode;
  self->cache_flags &= ~BLUR_APPLIED;

  switch (mode)
    {
    case GB_BLUR_MODE_ACTOR:
      clear_framebuffer_data (&self->background_fb);
      break;

    case GB_BLUR_MODE_BACKGROUND:
    default:
      /* Do nothing */
      break;
    }

  if (self->actor)
    clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MODE]);
}

float
gb_blur_effect_get_corner_radius (GbBlurEffect *self)
{
  g_return_val_if_fail (GB_IS_BLUR_EFFECT (self), 0.f);
  return self->corner_radius;
}

void
gb_blur_effect_set_corner_radius (GbBlurEffect *self,
                                     float          corner_radius)
{
  g_return_if_fail (GB_IS_BLUR_EFFECT (self));

  if (self->corner_radius == corner_radius)
    return;

  self->corner_radius = MAX (0.f, corner_radius);
  self->cache_flags &= ~BLUR_APPLIED;

  if (self->actor)
    clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CORNER_RADIUS]);
}
