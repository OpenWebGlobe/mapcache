/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: JPEG format
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "mapcache.h"
#include <apr_strings.h>


//------------------------------------------------------------------------------
// template for elevation json:
static char* json  =
  "{"
  "   \"Version\": \"1.1\",\n"
  "   \"Triangulation\": \"grid\",\n"
  "   \"GridSize\": %i,\n"
  "   \"VertexSemantic\": \"pt\",\n"
  "   \"IndexSemantic\": \"TRIANGLES\",\n"
  "   \"Vertices\": [%s],\n"
  "   \"Offset\": [%.15lf, %.15lf, %.15lf],\n"
  "   \"BoundingBox\": [[%.15lf, %.15lf, %.15lf],[%.15lf,%.15lf,%.15lf]],\n"
  "   \"HeightMap\": [%s]\n"
  "}";
// Integer: Gridsize  (2,3,5,9,17)
// String:  Comma separated vertex list
// Floats: Offset x, Offset y, Offset z
// Floats: bbxmin, bbymin, bbzmin, bbxmax, bbymax, bbzmax
// String: Comma separated heightmap


//------------------------------------------------------------------------------
static mapcache_buffer* _mapcache_imageio_json_create_empty(mapcache_context *ctx, mapcache_image_format *format,
    size_t width, size_t height, unsigned int color)
{
  return NULL;
}
//------------------------------------------------------------------------------
mapcache_buffer* _mapcache_imageio_json_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  char *vertexlist = "1,2,3,4,5"; // test...
  char *heightmap = "1,2,3,4,5";
  double offsetx, offsety, offsetz;
  double bbminx, bbminy, bbminz, bbmaxx, bbmaxy, bbmaxz;
  int gridsize = img->w;
  mapcache_image_format_json* format_json = (mapcache_image_format_json*)format;
 
  if (img->is_elevation != MC_ELEVATION_YES)
  {
    ctx->set_error(ctx,500,"can't convert non elevation data to json");
    return NULL;
  }
  
  
  offsetx = 0.123456789012345678;
  offsety = 0.123123123123123123;
  offsetz = 0.898989898989898989;
  
  bbminx = 0.1;
  bbminy = 0.2;
  bbminz = 0.15;
  bbmaxx = 0.2;
  bbmaxy = 0.4;
  bbmaxz = 0.3;
  
  char *json = apr_psprintf(ctx->pool, json,
                            gridsize,
                            vertexlist,
                            offsetx, offsety, offsetz,
                            bbminx, bbminy, bbminz, bbmaxx, bbmaxy, bbmaxz,
                            heightmap);
  
  
  size_t size = strlen(json);
  mapcache_buffer *buffer = mapcache_buffer_create(size, ctx->pool);
  mapcache_buffer_append(buffer, size, json);
 
  return buffer;
}
//------------------------------------------------------------------------------
mapcache_image* _mapcache_imageio_json_decode(mapcache_context *ctx, mapcache_buffer *buffer)
{  
 
  
  return NULL;
}
//------------------------------------------------------------------------------
void _mapcache_imageio_json_decode_to_image(mapcache_context *ctx, mapcache_buffer *buffer,
    mapcache_image *image)
{
  
}
//------------------------------------------------------------------------------
mapcache_image_format* mapcache_imageio_create_json_format(apr_pool_t *pool, char *name)
{
  mapcache_image_format_jpeg *format = apr_pcalloc(pool, sizeof(mapcache_image_format_json));
  format->format.name = name;
  format->format.extension = apr_pstrdup(pool,"json");
  format->format.mime_type = apr_pstrdup(pool,"application/json");
  format->format.metadata = apr_table_make(pool,3);
  format->format.create_empty_image = _mapcache_imageio_json_create_empty;
  format->format.write = _mapcache_imageio_json_encode;
  return (mapcache_image_format*)format;
}
//------------------------------------------------------------------------------
