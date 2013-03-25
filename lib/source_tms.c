/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Mapserver Mapfile datasource
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
#include "ezxml.h"
#include <apr_tables.h>
#include <apr_strings.h>

//------------------------------------------------------------------------------
// This takes tms tiles as input and stores it in a local cache.
// This simple implementation only works with global datasets in EPSG:3857
// It reads mercator tiles in the format
//     http://myserver.com/1.0.0/layername/z/x/y.png
// Created by Martin Christen, martin.christen@fhnw.ch
//------------------------------------------------------------------------------

#define TMS_MIN(x,y)  ((x<y) ? x : y)
#define TMS_MAX(x,y)  ((x>y) ? x : y)

void _GetTileCoords(mapcache_map *map, int* zoom, int* x, int* y, int flipy)
{  
  double res0 = ceil((map->extent.maxx - map->extent.minx) / (map->width));
  double res1 = ceil((map->extent.maxy - map->extent.miny) / (map->height));
  double res_up = TMS_MAX(res0, res1);

  double res = 1.0;
  int i;
  for (i=0;i<map->grid_link->grid->nlevels;i++)
  {
   res = map->grid_link->grid->levels[i]->resolution;
    if (res < res_up)
    {
       *zoom = i;
       *x = (int)floor((map->extent.minx - map->grid_link->grid->extent.minx) / (res * map->width));
       if (flipy)
       {
          *y = (int)((1 << *zoom) - 1 - floor((map->extent.miny - map->grid_link->grid->extent.miny) / (res * map->height)));
       }
       else
       {
          *y = (int)floor((map->extent.miny - map->grid_link->grid->extent.miny) / (res * map->height));
       }
       return;
    }
  }
  
  *zoom = 0;
  *x = 0;
  *y = 0;
}

//------------------------------------------------------------------------------
void _mapcache_source_tms_render_map_elevation(mapcache_context *ctx, mapcache_map *map)
{
  mapcache_source_tms *tms;
  int elevationblock;
  int zoom, x, y;
  char* url;
  double dx, dy;
  
  tms = (mapcache_source_tms*)map->tileset->source;
  elevationblock = map->grid_link->grid->elevationblock;
  
  _GetTileCoords(map, &zoom, &x, &y, tms->flipy);
  
  url = apr_psprintf(ctx->pool,"%s/1.0.0/%s/%i/%i/%i.%s", tms->url,tms->layer,zoom,x,y,tms->format);
  tms->http->url = apr_pstrdup(ctx->pool,url);
  
  map->encoded_data = mapcache_buffer_create(30000,ctx->pool);
  mapcache_http_do_request(ctx, tms->http, map->encoded_data, NULL, NULL);
  GC_CHECK_ERROR(ctx);
  
  if(!mapcache_imageio_is_valid_format(ctx,map->encoded_data)) {
    char *returned_data = apr_pstrndup(ctx->pool,(char*)map->encoded_data->buf,map->encoded_data->size);
    ctx->set_error(ctx, 502, "tms request for tileset %s returned an unsupported format:\n%s",
                   map->tileset->name, returned_data);
    return;
  }
  
  map->raw_image = mapcache_imageio_decode(ctx, map->encoded_data);
  map->raw_image->is_elevation = MC_ELEVATION_YES;
  GC_CHECK_ERROR(ctx);
  
  //map->raw_image->stride = 4 * elevationblock;
  dx = fabs(map->grid_link->grid->extent.maxx-map->grid_link->grid->extent.minx);
  dy = fabs(map->grid_link->grid->extent.maxx-map->grid_link->grid->extent.minx);
  map->raw_image->x0 = map->extent.minx / dx * 2.0;
  map->raw_image->y0 = map->extent.miny / dy * 2.0;
  map->raw_image->x1 = map->extent.maxx / dx * 2.0;
  map->raw_image->y1 = map->extent.maxy / dy * 2.0;
  
  if (map->raw_image->w != elevationblock || map->raw_image->h != elevationblock)
  {
    ctx->set_error(ctx,500,"Error: size of heightmap from source is not configured propery!");
  }
  
}
//------------------------------------------------------------------------------
void _mapcache_source_tms_render_map_image(mapcache_context *ctx, mapcache_map *map)
{
  int zoom, x, y;
  mapcache_source_tms *tms;
  char* url;
  
  tms = (mapcache_source_tms*)map->tileset->source;
  
  _GetTileCoords(map, &zoom, &x, &y, tms->flipy);
  
  url = apr_psprintf(ctx->pool,"%s/1.0.0/%s/%i/%i/%i.%s", tms->url,tms->layer,zoom,x,y,tms->format);
  tms->http->url = apr_pstrdup(ctx->pool,url);
  
  map->encoded_data = mapcache_buffer_create(30000,ctx->pool);
  mapcache_http_do_request(ctx, tms->http, map->encoded_data, NULL, NULL);
  GC_CHECK_ERROR(ctx);

  if(!mapcache_imageio_is_valid_format(ctx,map->encoded_data)) {
    char *returned_data = apr_pstrndup(ctx->pool,(char*)map->encoded_data->buf,map->encoded_data->size);
    ctx->set_error(ctx, 502, "tms request for tileset %s returned an unsupported format:\n%s",
                   map->tileset->name, returned_data);
  }
}

/**
 * \private \memberof mapcache_source_tms
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_tms_render_map(mapcache_context *ctx, mapcache_map *map)
{
  // example: myserver.com/path/1.0.0/LAYER/lod/x/y.FORMAT
  //          myserver.com/render/1.0.0/osm_traffic/1/1/0.png
  //              -> <url>myserver.com/render</url>
  //                 <layer>osm_traffic</layer>
  //                 <format>png</format>
  mapcache_source_tms *tms;
  
  tms = (mapcache_source_tms*)map->tileset->source;
  
  int is_elevation = map->tileset->elevation; // is this an elevation source ?
  
  if (is_elevation)
  {
    // RGBA encoded elevation data from TMS source
    _mapcache_source_tms_render_map_elevation(ctx, map);
  }
  else
  {
    // Standard image formt
    _mapcache_source_tms_render_map_image(ctx, map);
  }
  
}

void _mapcache_source_tms_query(mapcache_context *ctx, mapcache_feature_info *fi)
{
  ctx->set_error(ctx,500,"tms source does not support queries");
}

/**
 * \private \memberof mapcache_source_tms
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_tms_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_source *source)
{
  ezxml_t cur_node;
  mapcache_source_tms *src = (mapcache_source_tms*)source;

  if ((cur_node = ezxml_child(node,"url")) != NULL) {
    src->url = apr_pstrdup(ctx->pool,cur_node->txt);
    src->http = (mapcache_http*)apr_pcalloc(ctx->pool,sizeof(mapcache_http));
    src->http->connection_timeout = 30;
  }
  
  if ((cur_node = ezxml_child(node,"layer")) != NULL) {
    src->layer = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
    if ((cur_node = ezxml_child(node,"format")) != NULL) {
    src->format = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"flipy")) != NULL) {
    src->flipy = TRUE;
  }
}

/**
 * \private \memberof mapcache_source_tms
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_tms_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
}

mapcache_source* mapcache_source_tms_create(mapcache_context *ctx)
{
  mapcache_source_tms *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_tms));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate tms source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->http = NULL;
  source->url = NULL;
  source->layer = NULL;
  source->flipy = FALSE;
  source->source.type = MAPCACHE_SOURCE_TMS;
  source->source.render_map = _mapcache_source_tms_render_map;
  source->source.configuration_check = _mapcache_source_tms_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_tms_configuration_parse_xml;
  source->source.query_info = _mapcache_source_tms_query;
  
  return (mapcache_source*)&source->source;
}


/* vim: ts=2 sts=2 et sw=2
*/
