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
static mapcache_buffer* _mapcache_imageio_json_create_empty(mapcache_context *ctx, mapcache_image_format *format,
    size_t width, size_t height, unsigned int color)
{
  return NULL;
}
//------------------------------------------------------------------------------
mapcache_buffer* _mapcache_imageio_json_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  return NULL;
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
