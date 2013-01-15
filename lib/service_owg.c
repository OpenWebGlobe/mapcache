/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: OpenWebGlobe OWG tile layout
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
#include <math.h>
#include <ezxml.h>
/** \addtogroup services */
/** @{ */

void _create_capabilities_owg(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  
  mapcache_request_get_capabilities_owg *request = (mapcache_request_get_capabilities_owg*)req;
  
  request->request.mime_type = apr_pstrdup(ctx->pool,"application/json");
  
  // todo: use real data
   const char* name = request->layer;
   const char* type = "image";
   const char* format = request->tileset->format->extension;
   int minlod = request->grid_link->minz+1;
   int maxlod = request->grid_link->maxz-1;
   int x0 = request->grid_link->grid_limits[maxlod].minx;
   int y0 = request->grid_link->grid_limits[maxlod].miny;
   int x1 = request->grid_link->grid_limits[maxlod].maxx;
   int y1 = request->grid_link->grid_limits[maxlod].maxy;
   
   char* result;
   
   if (request->tileset->elevation)
   { 
      type = "elevation";
   }
   
   result = apr_psprintf(ctx->pool, "\
{\
   \"name\" : \"%s\",\n \
   \"type\" : \"%s\",\n \
   \"format\" : \"%s\",\n \
   \"minlod\" : %i,\n \
   \"maxlod\" : %i,\n \
   \"extent\" : [%i, %i, %i, %i]\n \
}\n", name, type, format, minlod, maxlod, x0, y0, x1, y1); 

  request->request.capabilities = apr_pstrdup(ctx->pool,result);
}

/**
 * \brief parse a OWG request
 * \private \memberof mapcache_service_owg
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_owg_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *cpathinfo, apr_table_t *params, mapcache_cfg *config)
{
  int index = 0;
  char *last, *key, *endptr;
  char *sTileset = NULL;
  mapcache_tileset *tileset = NULL;
  mapcache_grid_link *grid_link = NULL;
  char *pathinfo = NULL;
  int x=-1,y=-1,z=-1;
  
  // Example:
  // http://domain.com/mapcache/owg/tileset@grid/tiles/1/0/0.jpg
  // http://domain.com/mapcache/owg/bluemarble;landsat@world/layersettings.json
  // http://localhost/mapcache/owg/landsat@world/layersettings.json
  char* path = apr_pstrdup(ctx->pool, cpathinfo);
 
  char* next;
  char* layer = apr_strtok(path,"/",&next);
  char* json = apr_strtok(NULL,"/",&next);
  
  if (layer && json)
  {    
    if (0 == strcmp(json,"layersettings.json"))
    {
      //--------------------------------------------------------------------------
      char *gridname;
      mapcache_request_get_tile *req = (mapcache_request_get_tile*)apr_pcalloc(ctx->pool,sizeof(mapcache_request_get_tile));
      req->request.type = MAPCACHE_REQUEST_GET_TILE;
      gridname = layer;  /*hijack the char* pointer while counting the number of commas */
      while(*gridname) 
      {
        if(*gridname == ';') req->ntiles++;
        gridname++;
      }
      req->tiles = (mapcache_tile**)apr_pcalloc(ctx->pool,(req->ntiles+1) * sizeof(mapcache_tile*));

      /* reset the hijacked variables back to default value */
      gridname = NULL;
      req->ntiles = 0;

      for (key = apr_strtok(layer, ";", &last); key != NULL;
           key = apr_strtok(NULL, ";", &last)) 
      {
        tileset = mapcache_configuration_get_tileset(config,key);
        if(!tileset) 
        {
          /*tileset not found directly, test if it was given as "name@grid" notation*/
          char *tname = apr_pstrdup(ctx->pool,key);
          char *gname = tname;
          int i;
          while(*gname) 
          {
            if(*gname == '@') 
            {
              *gname = '\0';
              gname++;
              break;
            }
            gname++;
          }
          if(!gname) 
          {
            ctx->set_error(ctx,404, "received owg request with invalid layer %s", key);
            return;
          }
          tileset = mapcache_configuration_get_tileset(config,tname);
          if(!tileset) 
          {
            ctx->set_error(ctx,404, "received owg request with invalid layer %s", tname);
            return;
          }
          for(i=0; i<tileset->grid_links->nelts; i++) 
          {
            mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
            if(!strcmp(sgrid->grid->name,gname)) 
            {
              grid_link = sgrid;
              break;
            }
          }
          if(!grid_link) 
          {
            ctx->set_error(ctx,404, "received owg request with invalid grid %s", gname);
            return;
          }
        } 
        else 
        {
          grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
        }
        if(!gridname)
        {
          gridname = grid_link->grid->name;
        } 
        else 
        {
          if(strcmp(gridname,grid_link->grid->name)) 
          {
            ctx->set_error(ctx,400,"received owg request with conflicting grids %s and %s",
                           gridname,grid_link->grid->name);
            return;
          }
        }
      }
      //--------------------------------------------------------------------------
    
      mapcache_request_get_capabilities_owg *capreq = (mapcache_request_get_capabilities_owg*)apr_pcalloc(
          ctx->pool,sizeof(mapcache_request_get_capabilities_owg));
      capreq->request.request.type = MAPCACHE_REQUEST_GET_CAPABILITIES;
      capreq->tileset = tileset;
      capreq->grid_link = grid_link;
      capreq->layer = apr_pstrdup(ctx->pool,layer);
             
      *request = (mapcache_request*)capreq;
      return;
    }
  }

  if(cpathinfo) 
  {
    pathinfo = apr_pstrdup(ctx->pool,cpathinfo);
    /* parse a path_info like /tileset@grid/tiles/1/0/0.jpg */
    for (key = apr_strtok(pathinfo, "/", &last); key != NULL; key = apr_strtok(NULL, "/", &last)) 
    {
      if(!*key) continue; /* skip an empty string, could happen if the url contains // */
      switch(++index) {
        case 1: /* layer name */
          sTileset = apr_pstrdup(ctx->pool,key);
          break;
        case 2: /* tiles */
          if(strcmp("tiles",key)) 
          {
            ctx->set_error(ctx,404, "received owg request with invalid tile path %s", key);
            return;
          }
          break; 
        case 3:
          z = (int)strtol(key,&endptr,10);
          if(*endptr != 0) 
          {
            ctx->set_error(ctx,404, "failed to parse z");
            return;
          }
          break;
        case 4:
          x = (int)strtol(key,&endptr,10);
          if(*endptr != 0) 
          {
            ctx->set_error(ctx,404, "failed to parse x");
            return;
          }
          break;
        case 5:
          y = (int)strtol(key,&endptr,10);
          if(*endptr != '.') 
          {
            ctx->set_error(ctx,404, "failed to parse y");
            return;
          }
          break;
        default:
          ctx->set_error(ctx,404, "received owg request %s with invalid parameter %s", pathinfo, key);
          return;
      }
    }
  }
  
  if(index == 5) 
  {
    char *gridname;
    mapcache_request_get_tile *req = (mapcache_request_get_tile*)apr_pcalloc(ctx->pool,sizeof(mapcache_request_get_tile));
    req->request.type = MAPCACHE_REQUEST_GET_TILE;
    gridname = sTileset;  /*hijack the char* pointer while counting the number of commas */
    while(*gridname) 
    {
      if(*gridname == ';') req->ntiles++;
      gridname++;
    }
    req->tiles = (mapcache_tile**)apr_pcalloc(ctx->pool,(req->ntiles+1) * sizeof(mapcache_tile*));

    /* reset the hijacked variables back to default value */
    gridname = NULL;
    req->ntiles = 0;

    for (key = apr_strtok(sTileset, ";", &last); key != NULL;
         key = apr_strtok(NULL, ";", &last)) {
      tileset = mapcache_configuration_get_tileset(config,key);
      if(!tileset) {
        /*tileset not found directly, test if it was given as "name@grid" notation*/
        char *tname = apr_pstrdup(ctx->pool,key);
        char *gname = tname;
        int i;
        while(*gname) 
        {
          if(*gname == '@') 
          {
            *gname = '\0';
            gname++;
            break;
          }
          gname++;
        }
        if(!gname) 
        {
          ctx->set_error(ctx,404, "received owg request with invalid layer %s", key);
          return;
        }
        tileset = mapcache_configuration_get_tileset(config,tname);
        if(!tileset) {
          ctx->set_error(ctx,404, "received owg request with invalid layer %s", tname);
          return;
        }
        for(i=0; i<tileset->grid_links->nelts; i++) 
        {
          mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
          if(!strcmp(sgrid->grid->name,gname)) 
          {
            grid_link = sgrid;
            break;
          }
        }
        if(!grid_link) 
        {
          ctx->set_error(ctx,404, "received owg request with invalid grid %s", gname);
          return;
        }

      } 
      else 
      {
        grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
      }
      if(!gridname) 
      {
        gridname = grid_link->grid->name;
      } 
      else 
      {
        if(strcmp(gridname,grid_link->grid->name)) 
        {
          ctx->set_error(ctx,400,"received owg request with conflicting grids %s and %s",
                         gridname,grid_link->grid->name);
          return;
        }
      }

      int owg_y = grid_link->grid->levels[z]->maxy - y - 1;
      
      req->tiles[req->ntiles] = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
      switch(grid_link->grid->origin) {
        case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
          req->tiles[req->ntiles]->x = x;
          req->tiles[req->ntiles]->y = owg_y;
          break;
        case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
          req->tiles[req->ntiles]->x = x;
          req->tiles[req->ntiles]->y = grid_link->grid->levels[z]->maxy - owg_y - 1;
          break;
        case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
          req->tiles[req->ntiles]->x = grid_link->grid->levels[z]->maxx - x - 1;
          req->tiles[req->ntiles]->y = owg_y;
          break;
        case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
          req->tiles[req->ntiles]->x = grid_link->grid->levels[z]->maxx - x - 1;
          req->tiles[req->ntiles]->y = grid_link->grid->levels[z]->maxy - owg_y - 1;
          break;
      }
      req->tiles[req->ntiles]->z = z;
      mapcache_tileset_tile_validate(ctx,req->tiles[req->ntiles]);
      req->ntiles++;
      GC_CHECK_ERROR(ctx);
    }
    *request = (mapcache_request*)req;
    return;
  }
  else 
  {
    ctx->set_error(ctx,404, "received request with wrong number of arguments");
    return;
  }
}

mapcache_service* mapcache_service_owg_create(mapcache_context *ctx)
{
  mapcache_service_owg* service = (mapcache_service_owg*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_owg));
  if(!service) 
  {
    ctx->set_error(ctx, 500, "failed to allocate owg service");
    return NULL;
  }
  
  service->service.url_prefix = apr_pstrdup(ctx->pool,"owg");
  service->service.name = apr_pstrdup(ctx->pool,"owg");
  service->service.type = MAPCACHE_SERVICE_OWG;
  service->service.parse_request = _mapcache_service_owg_parse_request;
  service->service.create_capabilities_response = _create_capabilities_owg;
  return (mapcache_service*)service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
