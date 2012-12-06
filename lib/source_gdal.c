/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: GDAL datasource support (incomplete and disabled)
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

#ifdef USE_GDAL

#include <gdal.h>
#include <cpl_conv.h>

#include "gdal_alg.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"

//------------------------------------------------------------------------------
// This is an optimized dataset transformation based on OpenWebGlobe 
// data processing code (which is much faster than the original implementation.)
// Created by Martin Christen, martin.christen@fhnw.ch
//------------------------------------------------------------------------------

typedef struct datasetinfo datasetinfo;

#define GM_EPSILON 2.2204460492503131e-16  
#define GM_MIN(x,y)  ((x<y) ? x : y)
#define GM_MAX(x,y)  ((x>y) ? x : y)

//------------------------------------------------------------------------------
// Image Buffer Tools
inline double Floor(double x)           
{
  //return (T)((int)x - ((x < 0 && x != (int)(x))));
  return (double)floor((double)x);
}
//------------------------------------------------------------------------------
inline double Fract(double x)          
{
  return x-Floor(x);
}
//------------------------------------------------------------------------------
inline double Clamp(double x, double minval, double maxval)
{
  return (x < minval ? minval : (x > maxval ? maxval : x));
}
//------------------------------------------------------------------------------
inline void _ReadImageDataMemBGR(unsigned char* buffer, int bufferwidth, 
                                 int bufferheight, int x, int y, 
                                 unsigned char* r, unsigned char* g, 
                                 unsigned char* b, unsigned char* a)
{
  if (x<0 || y<0 || x>bufferwidth-1 || y>bufferheight-1) 
  { 
     *b=0; *g=0; *r=0; *a=0;
     return;
  }
  
  *b = buffer[bufferwidth*3*y+3*x+2];
  *g = buffer[bufferwidth*3*y+3*x+1];
  *r = buffer[bufferwidth*3*y+3*x];
  *a = 255;
}
//------------------------------------------------------------------------------
inline void _ReadImageValueBilinearBGR(unsigned char* buffer, int bufferwidth, 
                                       int bufferheight, double x, double y, 
                                       unsigned char* r, unsigned char* g, 
                                       unsigned char* b, unsigned char* a)
   {
      double uf = Fract(x);
      double vf = Fract(y);
      int nPixelX = (int)x;
      int nPixelY = (int)y;

      int u00,v00,u10,v10,u01,v01,u11,v11;
      u00 = nPixelX; if (u00>bufferwidth) u00 = bufferwidth-1;
      v00 = nPixelY; if (v00>bufferheight) u00 = bufferheight-1;
      u10 = nPixelX+1; if (u10>bufferwidth) u10 = bufferwidth-1;
      v10 = nPixelY; if (v10>bufferheight) v10 = bufferheight-1;
      u01 = nPixelX; if (u01>bufferwidth) u01 = bufferwidth-1;
      v01 = nPixelY+1; if (v01>bufferheight) v01 = bufferheight-1;
      u11 = nPixelX+1; if (u11>bufferwidth) u11 = bufferwidth-1;
      v11 = nPixelY+1; if (v11>bufferheight) v11 = bufferheight-1;

      unsigned char r00,g00,b00,a00;
      unsigned char r10,g10,b10,a10;
      unsigned char r01,g01,b01,a01;
      unsigned char r11,g11,b11,a11;

      _ReadImageDataMemBGR(buffer, bufferwidth, bufferheight, 
                              u00,v00,&r00,&g00,&b00,&a00);
      _ReadImageDataMemBGR(buffer, bufferwidth, bufferheight, 
                              u10,v10,&r10,&g10,&b10,&a10);
      _ReadImageDataMemBGR(buffer, bufferwidth, bufferheight, 
                              u01,v01,&r01,&g01,&b01,&a01);
      _ReadImageDataMemBGR(buffer, bufferwidth, bufferheight, 
                              u11,v11,&r11,&g11,&b11,&a11);

      double rd, gd, bd, ad;

      rd = (((double)r00)*(1-uf)*(1-vf)+((double)r10)*uf*(1-vf)
               +((double)r01)*(1-uf)*vf+((double)r11)*uf*vf)+0.5;
      gd = (((double)g00)*(1-uf)*(1-vf)+((double)g10)*uf*(1-vf)
               +((double)g01)*(1-uf)*vf+((double)g11)*uf*vf)+0.5;
      bd = (((double)b00)*(1-uf)*(1-vf)+((double)b10)*uf*(1-vf)
               +((double)b01)*(1-uf)*vf+((double)b11)*uf*vf)+0.5;
      ad = (((double)a00)*(1-uf)*(1-vf)+((double)a10)*uf*(1-vf)
               +((double)a01)*(1-uf)*vf+((double)a11)*uf*vf)+0.5;

      rd = Clamp(rd, 0.0, 255.0);
      gd = Clamp(gd, 0.0, 255.0);
      bd = Clamp(bd, 0.0, 255.0);
      ad = Clamp(ad, 0.0, 255.0);

      *r = (unsigned char) rd;
      *g = (unsigned char) gd;
      *b = (unsigned char) bd;
      *a = (unsigned char) ad;
   }
//------------------------------------------------------------------------------
inline int TestRectRectIntersect(double ulx1,double uly1,double lrx1,double lry1,   
                                 double ulx2,double uly2,double lrx2,double lry2)
{
  if (ulx1 >= lrx2 
      || lrx1 <= ulx2 
      || uly1 <= lry2 
      || lry1 >= uly2)
    return FALSE;
    
  return TRUE;
}
//------------------------------------------------------------------------------
// dataset info encapsulates important information about the dataset
struct datasetinfo
{
   double      ulx;
   double      lry;
   double      lrx;
   double      uly;
   double      affineTransformation[6];
   double      affineTransformation_inverse[6];
   double      pixelwidth;
   double      pixelheight;
   int         nBands;
   int         nSizeX;
   int         nSizeY;
};
//------------------------------------------------------------------------------
int InvertGeoMatrix(double* mGeoMatrix, double* mInvGeoMatrix)
{
  double det, inv_det;
  det = mGeoMatrix[1] * mGeoMatrix[5] - mGeoMatrix[2] * mGeoMatrix[4];

  if( fabs(det) < GM_EPSILON )
    return FALSE;

  inv_det = 1.0 / det;

  mInvGeoMatrix[1] =  mGeoMatrix[5] * inv_det;
  mInvGeoMatrix[4] = -mGeoMatrix[4] * inv_det;
  mInvGeoMatrix[2] = -mGeoMatrix[2] * inv_det;
  mInvGeoMatrix[5] =  mGeoMatrix[1] * inv_det;
  mInvGeoMatrix[0] = ( mGeoMatrix[2] * mGeoMatrix[3] 
                      - mGeoMatrix[0] * mGeoMatrix[5]) * inv_det;
  mInvGeoMatrix[3] = ( -mGeoMatrix[1] * mGeoMatrix[3] 
                      + mGeoMatrix[0] * mGeoMatrix[4]) * inv_det;

  return TRUE;
} 
//------------------------------------------------------------------------------
/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_gdal_render_map(mapcache_context *ctx, mapcache_map *map)
{
  int x,y;
  double minx, miny, maxx, maxy;
  //double gminx, gminy, gmaxx, gmaxy;
  int tilewidth, tileheight;
  char *dstSRS;
  char *srcSRS = "";
  char* inputfile;
  datasetinfo oSrcDataset;
  datasetinfo oDstDataset;
  
  OGRCoordinateTransformationH pCT;
  OGRCoordinateTransformationH pCTBack;
  
  double quality = 2.0;
  
  OGRSpatialReferenceH srcref;
  OGRSpatialReferenceH dstref;
  GDALDatasetH hDataset;
  
  mapcache_buffer *data = mapcache_buffer_create(0,ctx->pool);
  mapcache_source_gdal *gdal = (mapcache_source_gdal*)map->tileset->source;
  inputfile = gdal->datastr;
  
  // extent of tile:
  minx = map->extent.minx;
  miny = map->extent.miny;
  maxx = map->extent.maxx;
  maxy = map->extent.maxy;
  // width of tile (pixels)
  tilewidth = map->width;
  // heoight of tile (pixel)
  tileheight = map->height;
  
  
  if (gdal->extent != NULL)
  {
      // gdal->extent->minx;
   
  }
 
  // Setup GDAL
  GDALAllRegister();
  CPLErrorReset();
  
  // Setup Destination Spatial Reference System
  dstref = OSRNewSpatialReference(NULL);
  if (OSRSetFromUserInput (dstref, map->grid_link->grid->srs ) == OGRERR_NONE) 
  {
     OSRExportToWkt(dstref, &dstSRS);   
  }
  else 
  {
    ctx->set_error(ctx,500, "failed to parse gdal srs %s", map->grid_link->grid->srs);
    return;
  }
  
  // Open Dataset
  hDataset = GDALOpen( gdal->datastr, GA_ReadOnly );
   if( hDataset == NULL ) {
    ctx->set_error(ctx,500,"GDAL failed to open %s",gdal->datastr);
    return;
   }  
  
  // Retrieve Spatial Reference System of Source Dataset
  if (gdal->srs == NULL)
  {
      if( GDALGetProjectionRef( hDataset ) != NULL && strlen(GDALGetProjectionRef( hDataset )) > 0 )
         srcSRS = apr_pstrdup(ctx->pool,GDALGetProjectionRef( hDataset ));
      else if( GDALGetGCPProjection( hDataset ) != NULL && strlen(GDALGetGCPProjection(hDataset)) > 0 && GDALGetGCPCount( hDataset ) > 1 )
         srcSRS = apr_pstrdup(ctx->pool,GDALGetGCPProjection( hDataset ));
  }
  else
  {
      srcSRS = apr_pstrdup(ctx->pool,gdal->srs);
      //ctx->log(ctx,MAPCACHE_NOTICE,"**Source SRS: %s", srcSRS);
  }
      
  // Setup Source SRS
  srcref = OSRNewSpatialReference(NULL);
  if (OSRSetFromUserInput(srcref, srcSRS) != OGRERR_NONE)
  {
     ctx->set_error(ctx,500,"Error: can't create spatial reference of source");
     return; 
  }
  
  // Handle GeoTransform:
  GDALGetGeoTransform(hDataset, oSrcDataset.affineTransformation);
   
  if (!InvertGeoMatrix(oSrcDataset.affineTransformation, oSrcDataset.affineTransformation_inverse))
  {
     ctx->set_error(ctx,500,"Error: can't create inverse of affine transformation (src)");
     return;  
  }
  
  // Setup source dataset
  oSrcDataset.nBands = GDALGetRasterCount(hDataset);
  oSrcDataset.nSizeX = GDALGetRasterXSize(hDataset);
  oSrcDataset.nSizeY = GDALGetRasterYSize(hDataset);
  oSrcDataset.pixelwidth  = oSrcDataset.affineTransformation[1];
  oSrcDataset.pixelheight = oSrcDataset.affineTransformation[5];
  oSrcDataset.ulx = oSrcDataset.affineTransformation[0];
  oSrcDataset.uly = oSrcDataset.affineTransformation[3];
  oSrcDataset.lrx = oSrcDataset.ulx + oSrcDataset.affineTransformation[1] * oSrcDataset.nSizeX;
  oSrcDataset.lry = oSrcDataset.uly + oSrcDataset.affineTransformation[5] * oSrcDataset.nSizeY;
  
  //ctx->log(ctx,MAPCACHE_NOTICE,"Src Dataset: (%f,%f)-(%f,%f)", oSrcDataset.ulx,oSrcDataset.uly,oSrcDataset.lrx,oSrcDataset.lry);

  // Setup destination dataset (=Tile to be cached)  
  oDstDataset.nBands = 4;
  oDstDataset.nSizeX = tilewidth;
  oDstDataset.nSizeY = tileheight;
  oDstDataset.pixelwidth = fabs(maxx - minx) / (double)(tilewidth);
  oDstDataset.pixelheight = fabs(maxy - miny) / (double)(tileheight);
  oDstDataset.affineTransformation[0] = minx;
  oDstDataset.affineTransformation[1] = oDstDataset.pixelwidth;
  oDstDataset.affineTransformation[2] = 0;
  oDstDataset.affineTransformation[3] = maxy;
  oDstDataset.affineTransformation[4] = 0;
  oDstDataset.affineTransformation[5] = -oDstDataset.pixelheight;
  oDstDataset.ulx = minx;
  oDstDataset.uly = maxy;
  oDstDataset.lrx = maxx;
  oDstDataset.lry = miny;
  
  if (!InvertGeoMatrix(oDstDataset.affineTransformation, oDstDataset.affineTransformation_inverse))
  {
     ctx->set_error(ctx,500,"Error: can't create inverse of affine transformation (dst)");
     return;  
  }

  // Create Coordinate transformation:
  pCT        = OCTNewCoordinateTransformation(srcref, dstref);
  pCTBack    = OCTNewCoordinateTransformation(dstref, srcref);
  
  if (!pCT)
  {
   ctx->set_error(ctx,500,"Error: can't create forward transformation");
   return;  
  }
  
  if (!pCTBack)
  {
   ctx->set_error(ctx,500,"Error: can't create backward transformation");
   return;  
  }
  
  // Rectangle within source required for tile
  double dest_ulx = 1e20;
  double dest_lry = 1e20;
  double dest_lrx = -1e20;
  double dest_uly = -1e20;
 
  //Transform every pixel along border of tile
  int p;
  for (p=0;p<=oDstDataset.nSizeX;p++)
  {
    double lng,lat;
    x = p;
    y = 0;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &lng, &lat, NULL))
    { 
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
    x = p;
    y = oDstDataset.nSizeY;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &lng, &lat, NULL))
    {
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
  }
  for (p=0;p<=oDstDataset.nSizeY;p++)
  {
    double lng,lat; 
    x = 0;
    y = p;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &lng, &lat, NULL))
    {
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
    x = oDstDataset.nSizeX;
    y = p;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &lng, &lat, NULL))
    {
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
  }
    
  if (!TestRectRectIntersect(dest_ulx, dest_uly, dest_lrx, dest_lry,
                             oSrcDataset.ulx,oSrcDataset.uly,oSrcDataset.lrx,oSrcDataset.lry))
  {
    map->raw_image = mapcache_image_create(ctx);
    map->raw_image->w = map->width;
    map->raw_image->h = map->height;
    map->raw_image->stride = 4 * map->width;
    map->raw_image->data = malloc(map->width*map->height*4);
    
    for (y=0;y<map->height;y++)
    {
      for (x=0;x<map->width;x++)
      {
        map->raw_image->data[4*map->width*y+4*x+0] = 0;
        map->raw_image->data[4*map->width*y+4*x+1] = 0;
        map->raw_image->data[4*map->width*y+4*x+2] = 255;
        map->raw_image->data[4*map->width*y+4*x+3] = 255;
      }
    }
    
    apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
    
    OCTDestroyCoordinateTransformation(pCT);   
    OCTDestroyCoordinateTransformation(pCTBack); 
    OSRDestroySpatialReference(dstref);
    OSRDestroySpatialReference(srcref);
    GDALClose(hDataset);
    return;
  }
  
  double x0,y0,x1,y1;
  int nXOff;   // Start pixel x
  int nYOff;   // Start pixel y
  int nXSize;  // width (number of pixels to read)
  int nYSize;  // height (number of pixels to read)

  x0 = oSrcDataset.affineTransformation_inverse[0] + dest_ulx * oSrcDataset.affineTransformation_inverse[1] + dest_uly * oSrcDataset.affineTransformation_inverse[2];
  y0 = oSrcDataset.affineTransformation_inverse[3] + dest_ulx * oSrcDataset.affineTransformation_inverse[4] + dest_uly * oSrcDataset.affineTransformation_inverse[5];
  x1 = oSrcDataset.affineTransformation_inverse[0] + dest_lrx * oSrcDataset.affineTransformation_inverse[1] + dest_lry * oSrcDataset.affineTransformation_inverse[2];
  y1 = oSrcDataset.affineTransformation_inverse[3] + dest_lrx * oSrcDataset.affineTransformation_inverse[4] + dest_lry * oSrcDataset.affineTransformation_inverse[5];


  nXOff = (int)(x0);
  nYOff = (int)(y0);
  
  if (nXOff<0) { nXOff = 0;}
  if (nYOff<0) { nYOff = 0;}
  if (nYOff>oSrcDataset.nSizeY-1) {nYOff = oSrcDataset.nSizeY-1;}
  if (nXOff>oSrcDataset.nSizeX-1) {nXOff = oSrcDataset.nSizeX-1;}
  
  nXSize = (int)x1 - nXOff + 1;
  nYSize = (int)y1 - nYOff + 1;
  
  if (nXOff + nXSize > oSrcDataset.nSizeX-1)
  {
     nXSize = oSrcDataset.nSizeX-1 - nXOff;
  }
  
  if (nYOff + nYSize > oSrcDataset.nSizeY-1)
  {
     nYSize = oSrcDataset.nSizeY-1 - nYOff;
  }
  
  if (nXSize<=0 || nYSize<=0)
  {     
    // return empty tile (transparent)
    map->raw_image = mapcache_image_create(ctx);
    map->raw_image->w = map->width;
    map->raw_image->h = map->height;
    map->raw_image->stride = 4 * map->width;
    map->raw_image->data = malloc(map->width*map->height*4);
    //memset(map->raw_image->data, 0, map->width*map->height*4);
    
    for (y=0;y<map->height;y++)
    {
      for (x=0;x<map->width;x++)
      {
        map->raw_image->data[4*map->width*y+4*x+0] = 0;
        map->raw_image->data[4*map->width*y+4*x+1] = 255;
        map->raw_image->data[4*map->width*y+4*x+2] = 0;
        map->raw_image->data[4*map->width*y+4*x+3] = 255;
      }
    }
    
    apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
    
    OCTDestroyCoordinateTransformation(pCT);   
    OCTDestroyCoordinateTransformation(pCTBack); 
    OSRDestroySpatialReference(dstref);
    OSRDestroySpatialReference(srcref);
    GDALClose(hDataset);
    return;
  }
    
  int sourcetilewidth;  // nXSize would be 100%
  int sourcetileheight; // nYSize would be 100%
  
  double aspect = (double)nXSize/(double)nYSize;
  sourcetilewidth = quality * GM_MAX(tilewidth, tileheight);
  sourcetileheight = (int)((double)sourcetilewidth/aspect);
  
  double scalex = (double)sourcetilewidth/(double)nXSize;
  double scaley = (double)sourcetileheight/(double)nYSize;
  
  /*ctx->log(ctx,MAPCACHE_NOTICE,"TILE CALCULATED");
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_ulx = %10.3f", dest_ulx);
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_uly = %10.3f", dest_uly);
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_lry = %10.3f", dest_lry);
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_lrx = %10.3f", dest_lrx);
  ctx->log(ctx,MAPCACHE_NOTICE,"Scale: (%f, %f)", scalex, scaley);
  ctx->log(ctx,MAPCACHE_NOTICE,"(xOff,yOff)=(%i,%i); Size=(%i,%i)", nXOff, nYOff, nXSize, nYSize);
  ctx->log(ctx,MAPCACHE_NOTICE,"Reading Tile-Size: (%i, %i)", sourcetilewidth, sourcetileheight);*/
  
  // Retrieve data from source
  unsigned char *pData = apr_palloc(ctx->pool,sourcetilewidth*sourcetileheight*3);
  
  if (pData == NULL)
  {
     ctx->set_error(ctx,500,"Error: Cant allocate memory: %i bytes", sourcetilewidth*sourcetileheight*3);
     return; 
  }
  
  if (CE_None != GDALDatasetRasterIO(hDataset, GF_Read, 
         nXOff, nYOff,    // Pixel position in source dataset 
         nXSize, nYSize,  // width/height in source dataset
         pData,           // target buffer
         sourcetilewidth, sourcetileheight, // dimension of target buffer
         GDT_Byte,        // reading bytes...
         oSrcDataset.nBands, // number of input bands
         NULL,            // band map is ignored
         3,               // pixelspace
         3*sourcetilewidth, //linespace, 
         1                  //bandspace.
  ))
  {
    ctx->set_error(ctx,500,"Error: GDALDatasetRasterIO failed!");
    return;  
  }
  
  // Close Dataset
  GDALClose( hDataset );

  //----------------------------------------------------------------------------
  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  
  for (y=0;y<map->height;y++)
  {
     for (x=0;x<map->width;x++)
     {
        
       double x_coord = oDstDataset.ulx + ((double)x)*oDstDataset.pixelwidth;
       double y_coord = oDstDataset.uly - ((double)y)*oDstDataset.pixelheight;
       
       OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);
                   
       double xx = oSrcDataset.affineTransformation_inverse[0] + x_coord * oSrcDataset.affineTransformation_inverse[1] + y_coord * oSrcDataset.affineTransformation_inverse[2];
       double yy = oSrcDataset.affineTransformation_inverse[3] + x_coord * oSrcDataset.affineTransformation_inverse[4] + y_coord * oSrcDataset.affineTransformation_inverse[5];
       
       xx -= nXOff;
       yy -= nYOff;
       xx *= scalex;
       yy *= scaley;
       
      unsigned char r,g,b,a;
       
       _ReadImageDataMemBGR(pData, sourcetilewidth, 
                                       sourcetileheight, (int)(xx), (int)(yy), 
                                       &r,&g,&b,&a);
       
       map->raw_image->data[4*map->width*y+4*x+0] = b;
       map->raw_image->data[4*map->width*y+4*x+1] = g;
       map->raw_image->data[4*map->width*y+4*x+2] = r;
       map->raw_image->data[4*map->width*y+4*x+3] = a;
       
       
       // Draw Grid:
       //if (x==0 || y==0 || x==tilewidth-1 || y==tileheight-1)
       //{
       //  map->raw_image->data[4*map->width*y+4*x+0] = 0;
       //  map->raw_image->data[4*map->width*y+4*x+1] = 0;
       //  map->raw_image->data[4*map->width*y+4*x+2] = 0;
       //  map->raw_image->data[4*map->width*y+4*x+3] = 255;
       //}
     }
  }
  
  // free SRS
  OCTDestroyCoordinateTransformation(pCT);   
  OCTDestroyCoordinateTransformation(pCTBack); 
  OSRDestroySpatialReference(dstref);
  OSRDestroySpatialReference(srcref);
  
  apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
}
/*----------------------------------------------------------------------------*/
void _mapcache_source_gdal_query(mapcache_context *ctx, mapcache_feature_info *fi)
{
  ctx->set_error(ctx,500,"gdal source does not support queries");
}
/*----------------------------------------------------------------------------*/
void parse_extent(char* text, mapcache_extent* extent)
{
  double minx, miny, maxx, maxy;
  if (text == NULL || extent == NULL)
  {
      return;
  }
  
  extent->minx = -1e20;
  extent->miny = -1e20;
  extent->maxx = 1e20;
  extent->maxy = 1e20;
  
  char* pch = strtok(text," ");
  if (pch != NULL) { minx = atof(pch); }
  else {return;}
  pch = strtok(text," ");
  if (pch != NULL) { miny = atof(pch); }
  else {return;}
   pch = strtok(text," ");
  if (pch != NULL) { maxx = atof(pch); }
  else {return;}
   pch = strtok(text," ");
  if (pch != NULL) { maxy = atof(pch); }
  else {return;}
  
  extent->minx = minx;
  extent->miny = miny;
  extent->maxx = maxx;
  extent->maxy = maxy;
}
/*----------------------------------------------------------------------------*/
/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_gdal_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_source *source)
{
  ezxml_t cur_node;
  mapcache_source_gdal *src = (mapcache_source_gdal*)source;

  if ((cur_node = ezxml_child(node,"data")) != NULL) {
    src->datastr = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"srs")) != NULL) {
    src->srs = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"extent")) != NULL) {
     src->extent = malloc(sizeof(mapcache_extent));
     parse_extent(cur_node->txt, src->extent);
     apr_pool_cleanup_register(ctx->pool, src->extent,(void*)free, apr_pool_cleanup_null);
  }

  /*if ((cur_node = ezxml_child(node,"gdalparams")) != NULL) {
    for(cur_node = cur_node->child; cur_node; cur_node = cur_node->sibling) {
      apr_table_set(src->gdal_params, cur_node->name, cur_node->txt);
    }
  }*/
}
/*----------------------------------------------------------------------------*/
/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_gdal_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
}
/*----------------------------------------------------------------------------*/
mapcache_source* mapcache_source_gdal_create(mapcache_context *ctx)
{
  mapcache_source_gdal *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_gdal));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate gdal source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->srs = NULL;
  source->extent = NULL;
  source->source.type = MAPCACHE_SOURCE_GDAL;
  source->source.render_map = _mapcache_source_gdal_render_map;
  source->source.configuration_check = _mapcache_source_gdal_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_gdal_configuration_parse_xml;
  source->source.query_info = _mapcache_source_gdal_query;
  return (mapcache_source*)source;
}
/*----------------------------------------------------------------------------*/
#else
mapcache_source* mapcache_source_gdal_create(mapcache_context *ctx)
{
   ctx->set_error(ctx, 400, "failed to create gdal source, GDAL support is not compiled in this version");
   return NULL;
}
/*----------------------------------------------------------------------------*/
#endif // USE_GDAL

