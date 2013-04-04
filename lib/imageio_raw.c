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
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


//------------------------------------------------------------------------------
static mapcache_buffer* _mapcache_imageio_raw_create_empty(mapcache_context *ctx, mapcache_image_format *format,
    size_t width, size_t height, unsigned int color)
{
  int i;
  mapcache_image *empty;
  apr_pool_t *pool = NULL;
  mapcache_buffer *buf;
  if(apr_pool_create(&pool,ctx->pool) != APR_SUCCESS) {
    ctx->set_error(ctx,500,"raw create empty: failed to create temp memory pool");
    return NULL;
  }
  empty = mapcache_image_create(ctx);
  if(GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  empty->data = malloc(width*height*4*sizeof(unsigned char));
  for(i=0; i<width*height; i++) {
    ((unsigned int*)empty->data)[i] = color;
  }
  empty->w = width;
  empty->h = height;
  empty->stride = width * 4;

  buf = format->write(ctx,empty,format);
  apr_pool_destroy(pool);
  free(empty->data);
  return buf;
}
//------------------------------------------------------------------------------
mapcache_buffer* _mapcache_imageio_raw_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  mapcache_buffer *buffer = NULL;
  int gridsize = img->w;
  mapcache_image_format_json* format_json = (mapcache_image_format_json*)format;
  unsigned int i;
 
  if (img->is_elevation != MC_ELEVATION_YES)
  {
    ctx->set_error(ctx,500,"can't convert non elevation data to raw");
    return NULL;
  }
  uint32_t gsize = (uint32_t) gridsize;
  size_t bufsize = gridsize*gridsize*4 + 4 + 4; // header 'RAW0' + 4 bytes size
  buffer = mapcache_buffer_create(sizeof(mapcache_buffer),ctx->pool);
  
  buffer->buf = malloc(bufsize);
  apr_pool_cleanup_register(ctx->pool, buffer->buf,(void*)free, apr_pool_cleanup_null);
  buffer->size = (int)bufsize;
  buffer->avail = (int)bufsize;
  
  buffer->buf[0] = 'R'; buffer->buf[1] = 'A'; buffer->buf[2] = 'W'; buffer->buf[3] = '0';
  buffer->buf[4] = ((unsigned char*)&gsize)[0];
  buffer->buf[5] = ((unsigned char*)&gsize)[1];
  buffer->buf[6] = ((unsigned char*)&gsize)[2];
  buffer->buf[7] = ((unsigned char*)&gsize)[3];
  
  for (i=0;i<4*gridsize*gridsize;i++)
  {
    buffer->buf[8+i] = img->data[i];
  }
 
  return buffer;
}
//------------------------------------------------------------------------------
mapcache_image* _mapcache_imageio_raw_decode(mapcache_context *ctx, mapcache_buffer *buffer)
{  
  mapcache_image *img = mapcache_image_create(ctx);
  
  _mapcache_imageio_raw_decode_to_image(ctx, buffer,img);
  if(GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  return img;
}
//------------------------------------------------------------------------------
void _mapcache_imageio_raw_decode_to_image(mapcache_context *ctx, mapcache_buffer *buffer,
    mapcache_image *img)
{
  int i;
  
  img->is_elevation = MC_ELEVATION_YES;
  
  //parse buffer and create image...
  if(!img->data) {
    size_t gsize;
    ((unsigned char*)&gsize)[0] = buffer->buf[4];
    ((unsigned char*)&gsize)[1] = buffer->buf[5];
    ((unsigned char*)&gsize)[2] = buffer->buf[6];
    ((unsigned char*)&gsize)[3] = buffer->buf[7];
    img->w = (int)gsize;
    img->h = (int)gsize;
    img->stride = 4 * (int)gsize;
    img->data = malloc(4*img->w*img->h);
    apr_pool_cleanup_register(ctx->pool, img->data,(void*)free, apr_pool_cleanup_null);
    
    for (i=0;i<4*img->w*img->h;i++)
    {
      img->data[i] = buffer->buf[8+i];
    }
  }
  
  /*image: 
  unsigned char *data; 
  size_t w; 
  size_t h;
  size_t stride; 
  double x0, y0, x1, y1;*/
}
//------------------------------------------------------------------------------
mapcache_image_format* mapcache_imageio_create_raw_format(apr_pool_t *pool, char *name)
{
  mapcache_image_format_raw *format = apr_pcalloc(pool, sizeof(mapcache_image_format_raw));
  format->format.name = name;
  format->version = 1;
  format->format.extension = apr_pstrdup(pool,"raw");
  format->format.mime_type = apr_pstrdup(pool,"application/octet-stream");
  format->format.metadata = apr_table_make(pool,3);
  format->format.create_empty_image = _mapcache_imageio_raw_create_empty;
  format->format.write = _mapcache_imageio_raw_encode;
  format->format.type = GC_RAW;
  return (mapcache_image_format*)format;
}
//------------------------------------------------------------------------------
