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

//#define DEV_MODE

#include <gdal.h>
#include <cpl_conv.h>

#include "gdal_alg.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"

//------------------------------------------------------------------------------
// This is an optimized dataset transformation based on OpenWebGlobe 
// data processing code (which is much faster than the original implementation.)
// Created by Martin Christen, martin.christen@fhnw.ch
// 11/30/2012
//------------------------------------------------------------------------------

typedef struct datasetinfo datasetinfo;

#define GM_EPSILON 2.2204460492503131e-16  
#define GM_MIN(x,y)  ((x<y) ? x : y)
#define GM_MAX(x,y)  ((x>y) ? x : y)

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
  datasetinfo oTileDataset;
  
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
  if( GDALGetProjectionRef( hDataset ) != NULL && strlen(GDALGetProjectionRef( hDataset )) > 0 )
    srcSRS = apr_pstrdup(ctx->pool,GDALGetProjectionRef( hDataset ));
  else if( GDALGetGCPProjection( hDataset ) != NULL && strlen(GDALGetGCPProjection(hDataset)) > 0 && GDALGetGCPCount( hDataset ) > 1 )
    srcSRS = apr_pstrdup(ctx->pool,GDALGetGCPProjection( hDataset ));
  
  // Setup Source SRS
  srcref = OSRNewSpatialReference(NULL);
  if (OSRSetFromUserInput(srcref, srcSRS) != OGRERR_NONE)
  {
     ctx->set_error(ctx,500,"Error: can't create spatial reference of source");
     return; 
  }
  
  // Handle GeoTransform:
  GDALGetGeoTransform(hDataset, oSrcDataset.affineTransformation);
  
  oSrcDataset.nBands = GDALGetRasterCount(hDataset);
  oSrcDataset.nSizeX = GDALGetRasterXSize(hDataset);
  oSrcDataset.nSizeY = GDALGetRasterYSize(hDataset);
  
  if (!InvertGeoMatrix(oSrcDataset.affineTransformation, oSrcDataset.affineTransformation_inverse))
  {
     ctx->set_error(ctx,500,"Error: can't create inverse of affine transformation (src)");
     return;  
  }
  
  oSrcDataset.pixelwidth  = oSrcDataset.affineTransformation[1];
  oSrcDataset.pixelheight = oSrcDataset.affineTransformation[5];
  oSrcDataset.ulx = oSrcDataset.affineTransformation[0];
  oSrcDataset.uly = oSrcDataset.affineTransformation[3];
  oSrcDataset.lrx = oSrcDataset.ulx + oSrcDataset.affineTransformation[1] * oSrcDataset.nSizeX;
  oSrcDataset.lry = oSrcDataset.uly + oSrcDataset.affineTransformation[5] * oSrcDataset.nSizeY;

  // Setup destination dataset (=Tile to be cached)  
  oDstDataset.nBands = 4;
  oDstDataset.nSizeX = tilewidth;
  oDstDataset.nSizeY = tileheight;
  oDstDataset.ulx = minx;
  oDstDataset.uly = maxy;
  oDstDataset.lrx = maxx;
  oDstDataset.lry = miny;
  oDstDataset.pixelwidth = fabs(maxx - minx) / tilewidth;
  oDstDataset.pixelheight = fabs(maxy - miny) / tileheight;
  oDstDataset.affineTransformation[0] = minx;
  oDstDataset.affineTransformation[1] = oDstDataset.pixelwidth;
  oDstDataset.affineTransformation[2] = 0;
  oDstDataset.affineTransformation[3] = maxy;
  oDstDataset.affineTransformation[4] = 0;
  oDstDataset.affineTransformation[5] = -oDstDataset.pixelheight;

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
    unsigned long x,y;
    double lng,lat;
    x = p;
    y = 0;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    OCTTransform(pCTBack, 1, &lng, &lat, NULL);
    dest_ulx = GM_MIN(lng, dest_ulx);
    dest_lry = GM_MIN(lat, dest_lry);
    dest_lrx = GM_MAX(lng, dest_lrx);
    dest_uly = GM_MAX(lat, dest_uly);
    x = p;
    y = oDstDataset.nSizeY;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    OCTTransform(pCTBack, 1, &lng, &lat, NULL);
    dest_ulx = GM_MIN(lng, dest_ulx);
    dest_lry = GM_MIN(lat, dest_lry);
    dest_lrx = GM_MAX(lng, dest_lrx);
    dest_uly = GM_MAX(lat, dest_uly);
  }
  for (p=0;p<=oDstDataset.nSizeY;p++)
  {
    unsigned long x,y;
    double lng,lat; 
    x = 0;
    y = p;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    OCTTransform(pCTBack, 1, &lng, &lat, NULL);
    dest_ulx = GM_MIN(lng, dest_ulx);
    dest_lry = GM_MIN(lat, dest_lry);
    dest_lrx = GM_MAX(lng, dest_lrx);
    dest_uly = GM_MAX(lat, dest_uly);
    x = oDstDataset.nSizeX;
    y = p;
    lat = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    lng = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    OCTTransform(pCTBack, 1, &lng, &lat, NULL);
    dest_ulx = GM_MIN(lng, dest_ulx);
    dest_lry = GM_MIN(lat, dest_lry);
    dest_lrx = GM_MAX(lng, dest_lrx);
    dest_uly = GM_MAX(lat, dest_uly);
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
  nXSize = (int)(x1-x0+1);
  nYSize = (int)(y1-y0+1);
  if (nXSize<=0) nXSize = 1;
  if (nYSize<=0) nYSize = 1;
  
  // Calculate tile size for read (this can be further optimized)
  double aspect = (double)nXSize/(double)nYSize;
  int sourcetilewidth = quality * GM_MAX(tilewidth, tileheight);
  int sourcetileheight = sourcetilewidth/aspect;
  
 
  oTileDataset.ulx = dest_ulx;
  oTileDataset.lry = dest_lry;
  oTileDataset.lrx = dest_lrx;
  oTileDataset.uly = dest_uly;
  oTileDataset.pixelwidth = fabs(oTileDataset.lrx - oTileDataset.ulx) / (sourcetilewidth-1);
  oTileDataset.pixelheight = fabs(oTileDataset.uly - oTileDataset.lry) / (sourcetileheight-1);
  oTileDataset.affineTransformation[0] = dest_ulx;
  oTileDataset.affineTransformation[1] = oTileDataset.pixelwidth;
  oTileDataset.affineTransformation[2] = 0;
  oTileDataset.affineTransformation[3] = dest_uly;
  oTileDataset.affineTransformation[4] = 0;
  oTileDataset.affineTransformation[5] = -oTileDataset.pixelheight;
  oTileDataset.nBands = 3;
  oTileDataset.nSizeX = sourcetilewidth;
  oTileDataset.nSizeY = sourcetileheight;
  
  if (!InvertGeoMatrix(oTileDataset.affineTransformation, oTileDataset.affineTransformation_inverse))
  {
     ctx->set_error(ctx,500,"Error: can't create inverse of affine transformation (tile)");
     return;  
  }
  
  
#ifdef DEV_MODE
  ctx->log(ctx,MAPCACHE_NOTICE,"TILE CALCULATED");
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_ulx = %10.3f", dest_ulx);
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_uly = %10.3f", dest_uly);
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_lry = %10.3f", dest_lry);
  ctx->log(ctx,MAPCACHE_NOTICE,"dest_lrx = %10.3f", dest_lrx);
  ctx->log(ctx,MAPCACHE_NOTICE,"(xOff,yOff)=(%i,%i); Size=(%i,%i)", nXOff, nYOff, nXSize, nYSize);
  ctx->log(ctx,MAPCACHE_NOTICE,"Reading Tile-Size: (%i, %i)", sourcetilewidth, sourcetileheight);
#endif
    
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
        
       double x_coord = oDstDataset.ulx + x*oDstDataset.pixelwidth;
       double y_coord = oDstDataset.uly - y*oDstDataset.pixelheight;
       
       OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);
       
       x0 = oTileDataset.affineTransformation_inverse[0] + x_coord * oTileDataset.affineTransformation_inverse[1] + y_coord * oTileDataset.affineTransformation_inverse[2];
       y0 = oTileDataset.affineTransformation_inverse[3] + x_coord * oTileDataset.affineTransformation_inverse[4] + y_coord * oTileDataset.affineTransformation_inverse[5];
 
       int src_x = (int)x0;
       int src_y = (int)y0;
       
       if (src_x<sourcetilewidth && src_y<sourcetileheight && src_x>=0 && src_y>=0)
       {
         map->raw_image->data[4*map->width*y+4*x+0] = pData[3*sourcetilewidth*src_y+3*src_x+2];
         map->raw_image->data[4*map->width*y+4*x+1] = pData[3*sourcetilewidth*src_y+3*src_x+1];
         map->raw_image->data[4*map->width*y+4*x+2] = pData[3*sourcetilewidth*src_y+3*src_x+0];
         map->raw_image->data[4*map->width*y+4*x+3] = 255;
       }
       else
       {
         map->raw_image->data[4*map->width*y+4*x+0] = 0;
         map->raw_image->data[4*map->width*y+4*x+1] = 0;
         map->raw_image->data[4*map->width*y+4*x+2] = 255;
         map->raw_image->data[4*map->width*y+4*x+3] = 255;
       }
          
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
#if 0
/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_gdal_render_map(mapcache_context *ctx, mapcache_map *map)
{
  int x,y;
  double minx, miny, maxx, maxy;
  double gminx, gminy, gmaxx, gmaxy;
  int width, height;
  char *dstSRS;
  char *srcSRS = "";
  char* inputfile;

  OGRSpatialReferenceH hSRS;
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
  width = map->width;
  // heoight of tile (pixel)
  height = map->height;
  
  // extent of dataset
  gminx = map->grid_link->grid->extent.minx;
  gminy = map->grid_link->grid->extent.miny;
  gmaxx = map->grid_link->grid->extent.maxx;
  gmaxy = map->grid_link->grid->extent.maxy;

#ifdef DEV_MODE
  //ctx->log(ctx,MAPCACHE_NOTICE,"EXTENT: (%f,%f)-(%f,%f) [(%f,%f)-(%f,%f)], srs: %s",map->extent.minx,map->extent.miny,map->extent.maxx,map->extent.maxy, gminx, gminy, gmaxx, gmaxy, map->grid_link->grid->srs);
  //ctx->log(ctx,MAPCACHE_NOTICE,"file: %s", gdal->datastr);
  //ctx->log(ctx,MAPCACHE_NOTICE,"width: %i, height: %i", width, height);
#endif  
  
  // Setup GDAL
  GDALAllRegister();
  CPLErrorReset();
  
  // Setup Destination Spatial Reference
  hSRS = OSRNewSpatialReference(NULL);
  if (OSRSetFromUserInput (hSRS, map->grid_link->grid->srs ) == OGRERR_NONE) 
  {
     OSRExportToWkt(hSRS, &dstSRS);   
  }
  else 
  {
    ctx->set_error(ctx,500, "failed to parse gdal srs %s", map->grid_link->grid->srs);
    return;
  }
  // free SRS
  OSRDestroySpatialReference(hSRS);
  
#ifdef DEV_MODE
   //ctx->log(ctx,MAPCACHE_NOTICE,"SRS: %s", dstSRS);
#endif
  
 
  // Open Dataset
  hDataset = GDALOpen( gdal->datastr, GA_ReadOnly );
  if( hDataset == NULL ) {
    ctx->set_error(ctx,500,"GDAL failed to open %s",gdal->datastr);
    return;
  }
  
  //----------------------------------------------------------------------------
  // Check that there's at least one raster band
  if ( GDALGetRasterCount(hDataset) == 0 ) {
    ctx->set_error(ctx,500,"raster %s has no bands",gdal->datastr);
    return;
  }

  if( GDALGetProjectionRef( hDataset ) != NULL && strlen(GDALGetProjectionRef( hDataset )) > 0 )
    srcSRS = apr_pstrdup(ctx->pool,GDALGetProjectionRef( hDataset ));
  else if( GDALGetGCPProjection( hDataset ) != NULL && strlen(GDALGetGCPProjection(hDataset)) > 0 && GDALGetGCPCount( hDataset ) > 1 )
    srcSRS = apr_pstrdup(ctx->pool,GDALGetGCPProjection( hDataset ));

#ifdef DEV_MODE
  //ctx->log(ctx,MAPCACHE_NOTICE,"souce srs: %s", srcSRS);
#endif
  
  GDALDriverH hDriver = GDALGetDriverByName( "MEM" );
  GDALDatasetH hDstDS;
  
  //----------------------------------------------------------------------------
  // Create a transformation object from the source to
  // destination coordinate system.
  void *hTransformArg = GDALCreateGenImgProjTransformer( hDataset, srcSRS,
                                                         NULL, dstSRS,
                                                         TRUE, 0.0, 0 );
  if( hTransformArg == NULL ) {
    ctx->set_error(ctx,500,"gdal failed to create SRS transformation object");
    return;
  }
  
  //----------------------------------------------------------------------------
  // Get approximate output definition
  int nPixels, nLines;
  double adfDstGeoTransform[6];
  if( GDALSuggestedWarpOutput( hDataset,
                               GDALGenImgProjTransform, hTransformArg,
                               adfDstGeoTransform, &nPixels, &nLines )
      != CE_None ) {
    ctx->set_error(ctx,500,"gdal failed to create suggested warp output");
    return;
  }
  
  GDALDestroyGenImgProjTransformer( hTransformArg );
  double dfXRes = fabs(maxx - minx) / width;
  double dfYRes = fabs(maxy - miny) / height;

  adfDstGeoTransform[0] = minx;
  adfDstGeoTransform[3] = maxy;
  adfDstGeoTransform[1] = dfXRes;
  adfDstGeoTransform[5] = -dfYRes;
  hDstDS = GDALCreate( hDriver, "tempd_gdal_image", width, height, 4, GDT_Byte, NULL );
  
  //----------------------------------------------------------------------------
  // Write out the projection definition

  GDALSetProjection( hDstDS, dstSRS );
  GDALSetGeoTransform( hDstDS, adfDstGeoTransform );
  char               **papszWarpOptions = NULL;
  papszWarpOptions = CSLSetNameValue( papszWarpOptions, "INIT", "0" );

  //----------------------------------------------------------------------------
  // Create a transformation object from the source to               
  // destination coordinate system.                                  

  GDALTransformerFunc pfnTransformer = NULL;
  void               *hGenImgProjArg=NULL, *hApproxArg=NULL;
  hTransformArg = hGenImgProjArg = GDALCreateGenImgProjTransformer( hDataset, srcSRS, hDstDS, dstSRS, TRUE, 0, 0 );

  if( hTransformArg == NULL )
    exit( 1 );

  pfnTransformer = GDALGenImgProjTransform;

  hTransformArg = hApproxArg = GDALCreateApproxTransformer( GDALGenImgProjTransform, hGenImgProjArg, 0.125 );
  
  
  pfnTransformer = GDALApproxTransform;
  
  //---------------------------------------------------------------------------
  // Invoke the warper
   
  GDALSimpleImageWarp( hDataset, hDstDS, 0, NULL,
                       pfnTransformer, hTransformArg,
                       GDALDummyProgress, NULL, papszWarpOptions );

  CSLDestroy( papszWarpOptions );

  if( hApproxArg != NULL )
    GDALDestroyApproxTransformer( hApproxArg );

  if( hGenImgProjArg != NULL )
    GDALDestroyGenImgProjTransformer( hGenImgProjArg );

  if(GDALGetRasterCount(hDstDS) != 4) {
    ctx->set_error(ctx,500,"gdal did not create a 4 band image");
    return;
  }

  GDALRasterBandH *redband, *greenband, *blueband, *alphaband;

  redband = GDALGetRasterBand(hDstDS,3);
  greenband = GDALGetRasterBand(hDstDS,2);
  blueband = GDALGetRasterBand(hDstDS,1);
  alphaband = GDALGetRasterBand(hDstDS,4);

  unsigned char *rasterdata = apr_palloc(ctx->pool,width*height*4);
  data->buf = rasterdata;
  data->avail = width*height*4;
  data->size = width*height*4;

  GDALRasterIO(redband,GF_Read,0,0,width,height,(void*)(rasterdata),width,height,GDT_Byte,4,4*width);
  GDALRasterIO(greenband,GF_Read,0,0,width,height,(void*)(rasterdata+1),width,height,GDT_Byte,4,4*width);
  GDALRasterIO(blueband,GF_Read,0,0,width,height,(void*)(rasterdata+2),width,height,GDT_Byte,4,4*width);
  if(GDALGetRasterCount(hDataset)==4)
    GDALRasterIO(alphaband,GF_Read,0,0,width,height,(void*)(rasterdata+3),width,height,GDT_Byte,4,4*width);
  else {
    unsigned char *alphaptr;
    int i;
    for(alphaptr = rasterdata+3, i=0; i<width*height; i++, alphaptr+=4) {
      *alphaptr = 255;
    }
  }

  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = width;
  map->raw_image->h = height;
  map->raw_image->stride = width * 4;
  map->raw_image->data = rasterdata;
  
  //----------------------------------------------------------------------------
  // Close GDAL Datasets
  GDALClose( hDstDS );
  GDALClose( hDataset );
  
  //----------------------------------------------------------------------------
  //----------------------------------------------------------------------------
  
  /*map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  
  for (y=0;y<map->height;y++)
  {
     for (x=0;x<map->width;x++)
     {
       map->raw_image->data[4*map->width*y+4*x+0] = x % 255;
       map->raw_image->data[4*map->width*y+4*x+1] = x*y % 255;
       map->raw_image->data[4*map->width*y+4*x+2] = y % 255;
     }
  }*/
  
  apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
}
#endif
/*----------------------------------------------------------------------------*/
void _mapcache_source_gdal_query(mapcache_context *ctx, mapcache_feature_info *fi)
{
  ctx->set_error(ctx,500,"gdal source does not support queries");
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

  if ((cur_node = ezxml_child(node,"gdalparams")) != NULL) {
    for(cur_node = cur_node->child; cur_node; cur_node = cur_node->sibling) {
      apr_table_set(src->gdal_params, cur_node->name, cur_node->txt);
    }
  }
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

