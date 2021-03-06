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
// very simple high performance string handling for JSON strings.
// dynamic strings based on: http://locklessinc.com/articles/dynamic_cstrings/
  
#define STR_FREEABLE (1ULL << 63)
#define NaS ((json_string) {NULL, 0, 0, 0})
#define STRINIT ((json_string) {(char*)malloc(16), 0, (16), (char*)malloc(32)})

  
typedef struct json_string json_string;
struct json_string
{
  char *s;
  size_t size;   // length of string
  size_t b_size; // memory available
  char *tmp;     // 32 bytes temp memory to speed up certain operations
};
//------------------------------------------------------------------------------- 
static size_t json_strsize(json_string *s)
{
  if (!s->s) return 0;
  return s->size;
}
//------------------------------------------------------------------------------- 
static json_string json_strmalloc(size_t size)
{
  if (size < 16) size = 16;
  return (json_string) {(char*)malloc(size), 0, size | STR_FREEABLE, (char*)malloc(32)};
}
//------------------------------------------------------------------------------- 
// resize string-buffer
static void _json_str_resize(json_string *s, size_t size)
{
  char *buf;
  size_t bsize;
  
  if (!(s->s)) return;
 
  if (!(s->b_size & STR_FREEABLE))
  {
    json_string s2;
    if (size <= s->size) return;
    s2 = json_strmalloc(size);
    memcpy(s2.s, s->s, s->size);
    s->s = s2.s;
    s->b_size = s2.b_size;
    
    return;
  }
  
  if (size & STR_FREEABLE)
  {
    free(s->s);
    *s = NaS;
    return;
  }
  
  bsize = s->b_size - STR_FREEABLE;
  if (size < 16) size = 16;
  if ((4 * size > 3 * bsize) && (size <= bsize)) return;
  if ((size > bsize) && (size < bsize * 2)) size = bsize * 2;
  if (size < 16) size = 16;
  buf = (char*)realloc(s->s, size);
  
  if (!buf)
  {
    free(s->s);
    *s = NaS;
  }
  else
  {
    s->s = buf;
    s->b_size = size | STR_FREEABLE;
  }
}
//------------------------------------------------------------------------------
// convert to c-string
static char *json_tocstr(json_string *s)
{
  size_t bsize;
  if (!s->s) return NULL;
  
  bsize = s->b_size & ~STR_FREEABLE;
  
  /* null terminate string: there must be space for appending 0 */
  if (s->size == bsize)
  {
    _json_str_resize(s, bsize + 1);
    if (!s->s) return NULL;
  }
  s->s[s->size] = 0;
  return s->s;
}
//------------------------------------------------------------------------------
static void json_strprintf(json_string *s, const char *fmt, ...)
{
  va_list v;
  size_t len;
  if (!(s->s)) *s = STRINIT;
  if (!fmt) return;
  va_start(v, fmt);
  len = vsnprintf(NULL, 0, fmt, v) + 1;
  va_end(v);
  _json_str_resize(s, len);
  if (!(s->s)) return;
  va_start(v, fmt);
  vsnprintf(s->s, len, fmt, v);
  va_end(v);
  s->size = len - 1;
}
//------------------------------------------------------------------------------
static void json_append_cstr(json_string *s, const char *str)
{
  size_t len;
  size_t bsize;
  len = strlen(str);
  if (!(s->s)) *s = STRINIT;
  if (!str || !len) return;
  bsize = s->b_size & ~STR_FREEABLE;
  if (s->size + len >= bsize)
  {
    _json_str_resize(s, s->size + len);
    if (!(s->s)) return;
  }
  memcpy(&s->s[s->size], str, len);
  s->size += len;
}
//----------------------------------------------------------------------------
static void json_append_int(json_string *s, int value)
{
  snprintf(s->tmp, 32, "%i", value);
  json_append_cstr(s, s->tmp);
}
//----------------------------------------------------------------------------
static void json_append_comma_int(json_string *s, int value)
{
  snprintf(s->tmp, 32, ",%i", value);
  json_append_cstr(s, s->tmp);
}
//----------------------------------------------------------------------------
static void json_append_float(json_string *s, float value)
{
  snprintf(s->tmp, 32, "%.7g", value);
  json_append_cstr(s, s->tmp);
}
//----------------------------------------------------------------------------
static void json_append_comma_float(json_string *s, float value)
{
  snprintf(s->tmp, 32, ",%.7g", value);
  json_append_cstr(s, s->tmp);
}
//----------------------------------------------------------------------------
static void json_append_double(json_string *s, double value)
{
  snprintf(s->tmp, 32, "%.15g", value);
  json_append_cstr(s, s->tmp);
}
//----------------------------------------------------------------------------
static void json_append_comma_double(json_string *s, double value)
{
  snprintf(s->tmp, 32, ",%.15g", value);
  json_append_cstr(s, s->tmp);
}
//------------------------------------------------------------------------------
static void json_append_json(json_string *s, const json_string *s2)
{
  json_append_cstr(s, s2->s);
}
//------------------------------------------------------------------------------
static void json_strfree(json_string *s)
{
  if (s->b_size & STR_FREEABLE) free(s->s);
  if (s->tmp) free(s->tmp);
  *s = NaS;
}
//------------------------------------------------------------------------------
inline void _MercatorToWGS84(double x, double y, double* lng, double* lat)
{
  *lat = M_PI/2.0 - 2.0 * atan(exp(-y*M_PI));
  *lng = M_PI*x / 1.0;
}
//------------------------------------------------------------------------------
inline void _WGS84ToCartesian(double lng, double lat, double elv, double* x, double * y, double *z)
{
  double sinlat = sin(lat);
  double coslat = cos(lat);
  double sinlong = sin(lng);
  double coslong = cos(lng);
  double Rn = 6378137.0 / sqrt(1.0-0.006694379990197*sinlat*sinlat);
  *x = (Rn + elv) * coslat * coslong * 1.1920930376163765926810017443897e-7;
  *y = (Rn + elv) * coslat * sinlong * 1.1920930376163765926810017443897e-7;
  *z = (0.993305620011365*Rn + elv) * sinlat * 1.1920930376163765926810017443897e-7;
}
//------------------------------------------------------------------------------
void _gen_json(json_string* str, float* heightmap, int gridsize, double x0, double y0, double x1, double y1)
{
  
  double offsetx, offsety, offsetz;
  double bbminx = 1e20;
  double bbminy = 1e20; 
  double bbminz = 1e20; 
  double bbmaxx = -1e20; 
  double bbmaxy = -1e20;
  double bbmaxz = -1e20;
  int i,j,x,y;
  
  //-----------
  // START JSON
  //-----------
  json_append_cstr(str, "{\n");
  
  //------------------
  // Generate VERSION
  //------------------
  
  json_append_cstr(str, "  \"Version\": \"1.1\",\n");
  
  //------------------
  // Generate GRIDSIZE
  //------------------
  
  json_append_cstr(str, "  \"GridSize\": ");
  json_append_int(str, gridsize);
  json_append_cstr(str, ",\n");

  //----------------
  // Generate BOUNDS
  //----------------
  
  json_append_cstr(str, "  \"Bounds\": [");
  json_append_double(str, x0);
  json_append_comma_double(str, y0);
  json_append_comma_double(str, x1);
  json_append_comma_double(str, y1);
  json_append_cstr(str, "],\n");
  
  //-------------------------
  // Generate VERTEX SEMANTIC
  //-------------------------
  
  json_append_cstr(str, "  \"VertexSemantic\": \"pt\",\n");

  //------------------
  // Generate VERTICES
  //------------------
  
  json_append_cstr(str, "  \"Vertices\": [");
  
  // x0, y0, x1, y1: Bounding Box in Mercator Coordinates
  // lng0, lat0, lng1, lat1: Bounding box in WGS84 Coordinates (radiant)
  double lng0, lat0, lng1, lat1;
  double x_cart, y_cart, z_cart;
  double lng, lat;
  double x_coord;
  double y_coord;
  double elevation;
  double dH = (y1-y0)/(gridsize-1); // for x positions
  double dW = (x1-x0)/(gridsize-1); // for y positions
  float fdX = 1.0 / (gridsize-1);   // for texture coordinates (u,v)
    
  for (y=0;y<gridsize;y++)
  {
    for (x=0;x<gridsize;x++)
    {
      x_coord = x0 + x*dW;
      y_coord = y0 + y*dH;
     
      elevation = heightmap[(gridsize-y-1)*gridsize+x];
         
      _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
      _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
      
      bbminx = MIN(bbminx, x_cart);
      bbminy = MIN(bbminy, y_cart);
      bbminz = MIN(bbminz, z_cart);
      bbmaxx = MAX(bbmaxx, x_cart);
      bbmaxy = MAX(bbmaxy, y_cart);
      bbmaxz = MAX(bbmaxz, z_cart);
         
      if (x==0 && y==0)
      {
        offsetx = x_cart;
        offsety = y_cart;
        offsetz = z_cart;
            
        // POSITION:
        json_append_float(str,(float)(x_cart - offsetx));
        json_append_comma_float(str,(float)(y_cart - offsety));
        json_append_comma_float(str,(float)(z_cart - offsetz));
      }
      else
      {
        // POSITION
        json_append_comma_float(str,(float)(x_cart - offsetx));
        json_append_comma_float(str,(float)(y_cart - offsety));
        json_append_comma_float(str,(float)(z_cart - offsetz));
      }    
      
      // TEXCOORD:
      json_append_comma_float(str,(float)(x*fdX));
      json_append_comma_float(str,(float)(y*fdX));
      
    }
  }
  
  // add curtain vertices
  double curtainheight = 500;
  
  // NW-corner:
  x=0; y=gridsize-1;
  x_coord = x0 + x*dW; 
  y_coord = y0 + y*dH;
  elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
  _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
  _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
  json_append_comma_float(str,(float)(x_cart - offsetx));
  json_append_comma_float(str,(float)(y_cart - offsety));
  json_append_comma_float(str,(float)(z_cart - offsetz));
  json_append_comma_float(str,(float)(x*fdX));
  json_append_comma_float(str,(float)(y*fdX));
  // SW-corner:
  x=0; y=0;
  x_coord = x0 + x*dW; 
  y_coord = y0 + y*dH;
  elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
  _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
  _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
  json_append_comma_float(str,(float)(x_cart - offsetx));
  json_append_comma_float(str,(float)(y_cart - offsety));
  json_append_comma_float(str,(float)(z_cart - offsetz));
  json_append_comma_float(str,(float)(x*fdX));
  json_append_comma_float(str,(float)(y*fdX));
  // SE-corner:
  x=gridsize-1; y=0;
  x_coord = x0 + x*dW; 
  y_coord = y0 + y*dH;
  elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
  _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
  _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
  json_append_comma_float(str,(float)(x_cart - offsetx));
  json_append_comma_float(str,(float)(y_cart - offsety));
  json_append_comma_float(str,(float)(z_cart - offsetz));
  json_append_comma_float(str,(float)(x*fdX));
  json_append_comma_float(str,(float)(y*fdX));
  // NE-corner:
  x=gridsize-1; y=gridsize-1;
  x_coord = x0 + x*dW; 
  y_coord = y0 + y*dH;
  elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
  _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
  _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
  json_append_comma_float(str,(float)(x_cart - offsetx));
  json_append_comma_float(str,(float)(y_cart - offsety));
  json_append_comma_float(str,(float)(z_cart - offsetz));
  json_append_comma_float(str,(float)(x*fdX));
  json_append_comma_float(str,(float)(y*fdX));
    
  
  // point for west-border
  for (i=1;i<gridsize-1;i++)
  {
    x = 0;
    y = gridsize-1-i;    
    x_coord = x0 + x*dW; 
    y_coord = y0 + y*dH;
    elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
    _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
    _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
    json_append_comma_float(str,(float)(x_cart - offsetx));
    json_append_comma_float(str,(float)(y_cart - offsety));
    json_append_comma_float(str,(float)(z_cart - offsetz));
    json_append_comma_float(str,(float)(x*fdX));
    json_append_comma_float(str,(float)(y*fdX));
  }
  
  // vertices for south-border
  for (i=1;i<gridsize-1;i++)
  {
    x = i;
    y = 0;
    x_coord = x0 + x*dW; 
    y_coord = y0 + y*dH;
    elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
    _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
    _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
    json_append_comma_float(str,(float)(x_cart - offsetx));
    json_append_comma_float(str,(float)(y_cart - offsety));
    json_append_comma_float(str,(float)(z_cart - offsetz));
    json_append_comma_float(str,(float)(x*fdX));
    json_append_comma_float(str,(float)(y*fdX));
  }
  
  // vertices for east-border
  for (i=1;i<gridsize-1;i++)
  {
    x = gridsize-1;
    y = i;
    x_coord = x0 + x*dW; 
    y_coord = y0 + y*dH;
    elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
    _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
    _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
    json_append_comma_float(str,(float)(x_cart - offsetx));
    json_append_comma_float(str,(float)(y_cart - offsety));
    json_append_comma_float(str,(float)(z_cart - offsetz));
    json_append_comma_float(str,(float)(x*fdX));
    json_append_comma_float(str,(float)(y*fdX));
  }
  
  // vertices for north-border
  for (i=1;i<gridsize-1;i++)
  {
    x = gridsize-1-i;
    y = gridsize-1;
    x_coord = x0 + x*dW; 
    y_coord = y0 + y*dH;
    elevation = heightmap[(gridsize-y-1)*gridsize+x]-curtainheight;
    _MercatorToWGS84(x_coord,y_coord,&lng,&lat);
    _WGS84ToCartesian(lng, lat, elevation, &x_cart, &y_cart, &z_cart);
    json_append_comma_float(str,(float)(x_cart - offsetx));
    json_append_comma_float(str,(float)(y_cart - offsety));
    json_append_comma_float(str,(float)(z_cart - offsetz));
    json_append_comma_float(str,(float)(x*fdX));
    json_append_comma_float(str,(float)(y*fdX));
  }
  
  
  json_append_cstr(str, "],\n"); // end Vertices
  
  //-----------------
  // Generate INDICES
  //------------------
  
  json_append_cstr(str, "  \"Indices\": [");
  

  for (j=0;j<gridsize-1;j++)
  {
    for (i=0;i<gridsize-1;i++)
    {
      /*  d    c
          +-- -+
          |  / |    Triangles: acb, bcd
          |/   |
          +----+
          a    b
      */ 
      int a,b,c,d;
      
      a = i+j*gridsize;
      b = a+1;
      d = a+gridsize;
      c = d+1;

      
      if (i==0 && j==0)
      {
        json_append_int(str,a);
      }
      else
      {
        json_append_comma_int(str,a);
      }
      
      json_append_comma_int(str,c);
      json_append_comma_int(str,d);
      json_append_comma_int(str,a);
      json_append_comma_int(str,b);
      json_append_comma_int(str,c); 
    }
  }
  
  // Generate Indices for curtain
  
  int NW = gridsize*gridsize;
  int SW = NW+1;
  int SE = NW+2;
  int NE = NW+3;
  
  // left curtain:
  for (i=0;i<gridsize-1;i++)
  {
    int s,t,v,u;
    s = (gridsize-i-1)*gridsize;
    t = (gridsize-i-2)*gridsize;
    if (i==0) { u = NW; } else { u=gridsize*gridsize+3+i; }
    if (i==gridsize-2) { v = SW;} else { v=gridsize*gridsize+4+i; }
    
    json_append_comma_int(str,s);
    json_append_comma_int(str,t);
    json_append_comma_int(str,v);
    json_append_comma_int(str,s);
    json_append_comma_int(str,v);
    json_append_comma_int(str,u);
  }
   
  
  // bottom curtain
  for (i=0;i<gridsize-1;i++)
  {
    int s,t,v,u;
    s = i;
    t = i+1;
    if (i==0) { v = SW; } else { v=gridsize*gridsize+gridsize+1+i; }
    if (i==gridsize-2) { u = SE;} else { u=gridsize*gridsize+gridsize+2+i; }
    
    json_append_comma_int(str,t);
    json_append_comma_int(str,s);
    json_append_comma_int(str,v);
    json_append_comma_int(str,t);
    json_append_comma_int(str,v);
    json_append_comma_int(str,u);
  }
  
  // right curtain
  for (i=0;i<gridsize-1;i++)
  {
    int s,t,v,u;
    s = (i+1)*gridsize-1;
    t = (i+2)*gridsize-1;
    if (i==0) { u = SE; } else { u=gridsize*gridsize+2*gridsize-1+i; }
    if (i==gridsize-2) { v = NE;} else { v=gridsize*gridsize+2*gridsize+i; }
    
    json_append_comma_int(str,t);
    json_append_comma_int(str,s);
    json_append_comma_int(str,u);
    json_append_comma_int(str,t);
    json_append_comma_int(str,u);
    json_append_comma_int(str,v);
  }
   
  // top cutrain
  for (i=0;i<gridsize-1;i++)
  {
    int s,t,v,u;
    s = gridsize*gridsize-1-i;
    t = s-1;
    if (i==0) { u = NE; } else { u=gridsize*gridsize+3*gridsize-3+i; }
    if (i==gridsize-2) { v = NW;} else { v=gridsize*gridsize+3*gridsize-2+i; }
    
    json_append_comma_int(str,t);
    json_append_comma_int(str,s);
    json_append_comma_int(str,u);
    json_append_comma_int(str,t);
    json_append_comma_int(str,u);
    json_append_comma_int(str,v);
  }
  
  
  json_append_cstr(str, "],\n"); // end Indices
  
  
  //------------------------
  // Generate INDEX SEMANTIC
  //------------------------
  
  json_append_cstr(str, "  \"IndexSemantic\": \"TRIANGLES\",\n");
  
  //----------------
  // Generate OFFSET
  //----------------
  json_append_cstr(str, "  \"Offset\": [");
  json_append_double(str, offsetx);
  json_append_comma_double(str, offsety);
  json_append_comma_double(str, offsetz);
  json_append_cstr(str, "],\n"); // end Offset
  
  //----------------------
  // Generate BOUNDING BOX
  //----------------------
  
  json_append_cstr(str, "  \"BoundingBox\": [[");
  json_append_double(str, bbminx);
  json_append_comma_double(str, bbminy);
  json_append_comma_double(str, bbminz);
  json_append_cstr(str, "],["); // end BoundingBox
  json_append_double(str, bbmaxx);
  json_append_comma_double(str, bbmaxy);
  json_append_comma_double(str, bbmaxz);
  json_append_cstr(str, "]],\n"); // end BoundingBox
  
  //----------------------
  // Generate CURTAININDEX
  //----------------------
  
  json_append_cstr(str, "  \"CurtainIndex\": ");
  json_append_int(str, gridsize*gridsize);
  json_append_cstr(str, ",\n");
  
  
  //-------------------
  // Generate HEIGHTMAP
  //-------------------
  
  json_append_cstr(str, "  \"HeightMap\": [");
  
  for (i=0;i<gridsize*gridsize;i++)
  {
    if (i==0)
    {
      json_append_float(str, heightmap[0]);
    }
    else 
    {
      json_append_comma_float(str, heightmap[i]);
    }
    
  }
  
  json_append_cstr(str, "]\n"); // end HeightMap
 
  //---------------
  // TERMINATE JSON
  //---------------
  
  json_append_cstr(str, "}\n");
}
//------------------------------------------------------------------------------
static mapcache_buffer* _mapcache_imageio_json_create_empty(mapcache_context *ctx, mapcache_image_format *format,
    size_t width, size_t height, unsigned int color)
{
  return NULL;
}
//------------------------------------------------------------------------------
mapcache_buffer* _mapcache_imageio_json_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  mapcache_buffer *buffer = NULL;
  int gridsize = img->w;
  mapcache_image_format_json* format_json = (mapcache_image_format_json*)format;
 
  if (img->is_elevation != MC_ELEVATION_YES)
  {
    ctx->set_error(ctx,500,"can't convert non elevation data to json");
    return NULL;
  }
 
  json_string json = json_strmalloc(512);
  _gen_json(&json, img->data, gridsize, img->x0, img->y0, img->x1, img->y1);
  
  if (json.s)
  {
    buffer = mapcache_buffer_create(json.size, ctx->pool);
    mapcache_buffer_append(buffer, json.size, json.s);
  }
  json_strfree(&json);
  
 
  return buffer;
}
//------------------------------------------------------------------------------
mapcache_image* _mapcache_imageio_json_decode(mapcache_context *ctx, mapcache_buffer *buffer)
{  
  mapcache_image *img = mapcache_image_create(ctx);
  
  _mapcache_imageio_json_decode_to_image(ctx, buffer,img);
  if(GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  return img;
}
//------------------------------------------------------------------------------
void _mapcache_imageio_json_decode_to_image(mapcache_context *ctx, mapcache_buffer *buffer,
    mapcache_image *img)
{
  img->is_elevation = MC_ELEVATION_YES;
  
  // todo: parse buffer and create image...
  
  /*
  if(!img->data) {
    img->data = malloc(...);
    apr_pool_cleanup_register(r->pool, img->data, (void*)free, apr_pool_cleanup_null) ;
    img->stride = img->w * 4;
  }
  */
  
  /*image: 
  unsigned char *data; 
  size_t w; 
  size_t h;
  size_t stride; 
  double x0, y0, x1, y1;*/
  
 
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
