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
// data processing code (which is usually faster than WMS-Requests)
//
// This code also supports elevation data (from 1 band raster) with the following
// data types:
//  -> unsigned int, int
//  -> float, double
//  -> short, unsigned short
//  -> char
// internally elevation is treated as float array.
//
// Created by Martin Christen, martin.christen@fhnw.ch
//------------------------------------------------------------------------------

typedef struct datasetinfo datasetinfo;

#define GM_EPSILON 2.2204460492503131e-16 
#define GM_EPSILONFLT 1.19209290e-7f
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
inline void _ReadImageDataMemBGRA(unsigned char* buffer, int bufferwidth, 
                                 int bufferheight, int x, int y, 
                                 unsigned char* r, unsigned char* g, 
                                 unsigned char* b, unsigned char* a)
{
  if (x<0 || y<0 || x>bufferwidth-1 || y>bufferheight-1) 
  { 
     *b=0; *g=0; *r=0; *a=0;
     return;
  }
  
  *b = buffer[bufferwidth*4*y+4*x+2];
  *g = buffer[bufferwidth*4*y+4*x+1];
  *r = buffer[bufferwidth*4*y+4*x];
  *a = buffer[bufferwidth*4*y+4*x+3];
}
//------------------------------------------------------------------------------
// Approximate RGB values for mapping elevation to visible 
// wavelengths between 380 nm and 780nm
// based on: http://www.physics.sfasu.edu/astro/color/spectra.html
inline void _CalcSpectrumColor(double value, double mine, double maxe, char* r, char* g, char* b)
{
  double w = value * 400.0 / (maxe-mine) + 380;  // w in visible spectrum [380...780]
  
  if (w >= 380.0 &&  w < 440.0)
  {
    *r = (unsigned char) (255.0*(-(w - 440.0) / (440.0 - 380.0)));
    *g = 0;
    *b = 255;
  }
  else if (w >= 440.0 && w < 490)
  {
    *r = 0;
    *g = (unsigned char) (255.0*((w - 440.0) / (490.0 - 440.0)));
    *b = 255;
  }
  else if (w >= 490.0 && w < 510)
  {  
    *r = 0;
    *g = 255;
    *b = (255.0*(-(w - 510.0) / (510.0 - 490.0)));
  }
  else if (w >= 510.0 && w < 580.0)
  { 
    *r = (unsigned char)  (255*(w - 510.0) / (580.0 - 510.0));
    *g = 255;
    *b = 0;
  }
  else if (w >= 580.0 && w < 645.0)
  { 
    *r = 255;
    *g = (unsigned char) (255*(-(w - 645.0) / (645.0 - 580.0)));
    *b = 0;
  }
  else if (w >= 645.0 && w <= 780.0)
  {
    *r = 255;
    *g = 0;
    *b = 0;
  }
  else
  {
    *r = 0;
    *g = 0;
    *b = 0;
  }
  
}
//------------------------------------------------------------------------------
inline void _ReadImageDataMemGray(unsigned char* buffer, int bufferwidth, 
                                 int bufferheight, int x, int y, 
                                 unsigned char* r, unsigned char* g, 
                                 unsigned char* b, unsigned char* a, float NODATA, int datatype)
{
  if (x<0 || y<0 || x>bufferwidth-1 || y>bufferheight-1) 
  { 
     *b=0; *g=0; *r=0; *a=0;
     return;
  }
  
  unsigned int ui_value;
  int i_value;
  float f_value;
  double d_value;
  unsigned short us_value;
  short s_value;
  char b_value;
  
  double value = 0.0;
  
  switch(datatype)
  {
    case 1:  // GDT_UInt32
      ui_value = ((unsigned int*)buffer)[bufferwidth*y+x];
      value = (double) ui_value;
      break;
    case 2:  // GDT_Int32
      i_value = ((int*)buffer)[bufferwidth*y+x];
      value = (double) i_value;
      break;
    case 3:  // GDT_Float32
      f_value = ((float*)buffer)[bufferwidth*y+x];
      value = (double) f_value;
      break;
    case 4:  // GDT_Float64
      d_value = ((double*)buffer)[bufferwidth*y+x];
      value = d_value;
      break;
    case 5: 
      us_value = ((unsigned short*)buffer)[bufferwidth*y+x];
      value = (double) us_value;
    break;
    case 6:  
      s_value = ((short*)buffer)[bufferwidth*y+x];
      value = (double) s_value;
    break;
    case 7:  
      b_value = ((char*)buffer)[bufferwidth*y+x];
      value = (double) b_value;
    break;
    default:
      return;
  }
 
  
  if (fabs(value-NODATA)<GM_EPSILONFLT)
  {
    *b=0; *g=0; *r=0; *a=0;
  }
  else
  {
    value = (double)Clamp(value,0.0,8000);
    _CalcSpectrumColor(value, 0.0, 8000.00, r,g,b);
    *a = 255;
    //*b = (unsigned char)Clamp(value,0.0,255.0);
    //*g = *b;
    //*r = *b;
    //*a = 255;
  }
  
  if (value != 0)
  {
    int stop = 1;
    int stop2 = 1;
  }
 
}
//
//------------------------------------------------------------------------------
inline void _ReadImageDataMemElv(unsigned char* buffer, int bufferwidth, 
                                 int bufferheight, int x, int y, 
                                 unsigned char* r, unsigned char* g, 
                                 unsigned char* b, unsigned char* a, float NODATA, int datatype)
{
  if (x<0 || y<0 || x>bufferwidth-1 || y>bufferheight-1) 
  { 
     *b=0; *g=0; *r=0; *a=0;
     return;
  }
  
  unsigned int ui_value;
  int i_value;
  float f_value;
  double d_value;
  unsigned short us_value;
  short s_value;
  char b_value;
  
  float value = 0.0f;
  
  switch(datatype)
  {
    case 1:  // GDT_UInt32
      ui_value = ((unsigned int*)buffer)[bufferwidth*y+x];
      value = (float) ui_value;
      break;
    case 2:  // GDT_Int32f
      i_value = ((int*)buffer)[bufferwidth*y+x];
      value = (float) i_value;
      break;
    case 3:  // GDT_Float32
      f_value = ((float*)buffer)[bufferwidth*y+x];
      value = f_value;
      break;
    case 4:  // GDT_Float64
      d_value = ((double*)buffer)[bufferwidth*y+x];
      value = (float) d_value;
      break;
    case 5: 
      us_value = ((unsigned short*)buffer)[bufferwidth*y+x];
      value = (float) us_value;
    break;
    case 6:  
      s_value = ((short*)buffer)[bufferwidth*y+x];
      value = (float) s_value;
    break;
    case 7:  
      b_value = ((char*)buffer)[bufferwidth*y+x];
      value = (float) b_value;
    break;
    default:
      return;
  }
 
  
  if (value <= NODATA)
  {
    value = 0.0f;
  }

  // floating point to RGBA conversion...
  unsigned char* val_chr = (unsigned char*)&value;
  *r = val_chr[0];
  *g = val_chr[1];
  *b = val_chr[2];
  *a = val_chr[3];
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
inline void CreateMapBGR(mapcache_context *ctx, mapcache_map *map, 
      datasetinfo* pSrcDataset, datasetinfo* pDstDataset, 
      int sourcetilewidth, int sourcetileheight, mapcache_source_gdal* gdal, 
      OGRCoordinateTransformationH pCTBack, OGRCoordinateTransformationH pCTWGS84, 
      double minx_data_wgs84, double miny_data_wgs84,
      double maxx_data_wgs84,double maxy_data_wgs84, int nXOff,
      int nYOff, double scalex, double scaley, unsigned char* pData)
{
  //----------------------------------------------------------------------------
  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  int x,y;
  for (y=0;y<map->height;y++)
  {
     for (x=0;x<map->width;x++)
     {
       unsigned char r,g,b,a; 
       double x_coord = pDstDataset->ulx + ((double)x)*pDstDataset->pixelwidth;
       double y_coord = pDstDataset->uly - ((double)y)*pDstDataset->pixelheight;
       
       // note: non-global datasets may have too large numbers (overflow) to
       //       process on a global system. Therefore this additional check is
       //       implemented
       if (gdal->extent != NULL)
       {
            double x_wgs84 = x_coord;
            double y_wgs84 = y_coord;
            OCTTransform(pCTWGS84, 1, &x_wgs84, &y_wgs84, NULL);
            
            if (x_wgs84>=minx_data_wgs84 &&
                x_wgs84<=maxx_data_wgs84 &&
                y_wgs84>=miny_data_wgs84 &&
                y_wgs84<=maxy_data_wgs84)
            {
              // pixel will be inside dataset!
              OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
              double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
              double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
              xx -= nXOff;
              yy -= nYOff;
              xx *= scalex;
              yy *= scaley;
            
              _ReadImageDataMemBGR(pData, sourcetilewidth, 
                                              sourcetileheight, (int)(xx), (int)(yy), 
                                              &r,&g,&b,&a);
            }
            else
            {  // outside global extent -> completely transparent...
               r=0;
               g=0;
               b=0;
               a=0;
            }
       }
       else
       {
          OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
          double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
          double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
          xx -= nXOff;
          yy -= nYOff;
          xx *= scalex;
          yy *= scaley;
         
          _ReadImageDataMemBGR(pData, sourcetilewidth, 
                                          sourcetileheight, (int)(xx), (int)(yy), 
                                          &r,&g,&b,&a);
       }
       
       map->raw_image->data[4*map->width*y+4*x+0] = b;
       map->raw_image->data[4*map->width*y+4*x+1] = g;
       map->raw_image->data[4*map->width*y+4*x+2] = r;
       map->raw_image->data[4*map->width*y+4*x+3] = a;
     }
  }
}
//------------------------------------------------------------------------------
inline void CreateMapBGRA(mapcache_context *ctx, mapcache_map *map, 
      datasetinfo* pSrcDataset, datasetinfo* pDstDataset, 
      int sourcetilewidth, int sourcetileheight, mapcache_source_gdal* gdal, 
      OGRCoordinateTransformationH pCTBack, OGRCoordinateTransformationH pCTWGS84, 
      double minx_data_wgs84, double miny_data_wgs84,
      double maxx_data_wgs84,double maxy_data_wgs84, int nXOff,
      int nYOff, double scalex, double scaley, unsigned char* pData)
{
  //----------------------------------------------------------------------------
  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  int x,y;
  for (y=0;y<map->height;y++)
  {
     for (x=0;x<map->width;x++)
     {
       unsigned char r,g,b,a; 
       double x_coord = pDstDataset->ulx + ((double)x)*pDstDataset->pixelwidth;
       double y_coord = pDstDataset->uly - ((double)y)*pDstDataset->pixelheight;
       
       // note: non-global datasets may have too large numbers (overflow) to
       //       process on a global system. Therefore this additional check is
       //       implemented
       if (gdal->extent != NULL)
       {
            double x_wgs84 = x_coord;
            double y_wgs84 = y_coord;
            OCTTransform(pCTWGS84, 1, &x_wgs84, &y_wgs84, NULL);
            
            if (x_wgs84>=minx_data_wgs84 &&
                x_wgs84<=maxx_data_wgs84 &&
                y_wgs84>=miny_data_wgs84 &&
                y_wgs84<=maxy_data_wgs84)
            {
              // pixel will be inside dataset!
              OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
              double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
              double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
              xx -= nXOff;
              yy -= nYOff;
              xx *= scalex;
              yy *= scaley;
            
              _ReadImageDataMemBGRA(pData, sourcetilewidth, 
                                              sourcetileheight, (int)(xx), (int)(yy), 
                                              &r,&g,&b,&a);
            }
            else
            {  // outside global extent -> completely transparent...
               r=0;
               g=0;
               b=0;
               a=0;
            }
       }
       else
       {
          OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
          double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
          double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
          xx -= nXOff;
          yy -= nYOff;
          xx *= scalex;
          yy *= scaley;
         
          _ReadImageDataMemBGRA(pData, sourcetilewidth, 
                                          sourcetileheight, (int)(xx), (int)(yy), 
                                          &r,&g,&b,&a);
       }
       
       map->raw_image->data[4*map->width*y+4*x+0] = b;
       map->raw_image->data[4*map->width*y+4*x+1] = g;
       map->raw_image->data[4*map->width*y+4*x+2] = r;
       map->raw_image->data[4*map->width*y+4*x+3] = a;
     }
  }
}
//------------------------------------------------------------------------------
inline void CreateMapGray(mapcache_context *ctx, mapcache_map *map, 
      datasetinfo* pSrcDataset, datasetinfo* pDstDataset, 
      int sourcetilewidth, int sourcetileheight, mapcache_source_gdal* gdal, 
      OGRCoordinateTransformationH pCTBack, OGRCoordinateTransformationH pCTWGS84, 
      double minx_data_wgs84, double miny_data_wgs84,
      double maxx_data_wgs84,double maxy_data_wgs84, int nXOff,
      int nYOff, double scalex, double scaley, unsigned char* pData, float NODATA, int datatype)
{
  //----------------------------------------------------------------------------
  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  int x,y;
  for (y=0;y<map->height;y++)
  {
     for (x=0;x<map->width;x++)
     {
       unsigned char r,g,b,a; 
       double x_coord = pDstDataset->ulx + ((double)x)*pDstDataset->pixelwidth;
       double y_coord = pDstDataset->uly - ((double)y)*pDstDataset->pixelheight;
       
       // note: non-global datasets may have too large numbers (overflow) to
       //       process on a global system. Therefore this additional check is
       //       implemented
       if (gdal->extent != NULL)
       {
            double x_wgs84 = x_coord;
            double y_wgs84 = y_coord;
            OCTTransform(pCTWGS84, 1, &x_wgs84, &y_wgs84, NULL);
            
            if (x_wgs84>=minx_data_wgs84 &&
                x_wgs84<=maxx_data_wgs84 &&
                y_wgs84>=miny_data_wgs84 &&
                y_wgs84<=maxy_data_wgs84)
            {
              // pixel will be inside dataset!
              OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
              double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
              double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
              xx -= nXOff;
              yy -= nYOff;
              xx *= scalex;
              yy *= scaley;
            
              _ReadImageDataMemGray(pData, sourcetilewidth, 
                                              sourcetileheight, (int)(xx), (int)(yy), 
                                              &r,&g,&b,&a, NODATA, datatype);
            }
            else
            {  // outside global extent -> completely transparent...
               r=0;
               g=0;
               b=0;
               a=0;
            }
       }
       else
       {
          OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
          double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
          double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
          xx -= nXOff;
          yy -= nYOff;
          xx *= scalex;
          yy *= scaley;
         
          _ReadImageDataMemGray(pData, sourcetilewidth, 
                                          sourcetileheight, (int)(xx), (int)(yy), 
                                          &r,&g,&b,&a, NODATA, datatype);
       }
       
       map->raw_image->data[4*map->width*y+4*x+0] = b;
       map->raw_image->data[4*map->width*y+4*x+1] = g;
       map->raw_image->data[4*map->width*y+4*x+2] = r;
       map->raw_image->data[4*map->width*y+4*x+3] = a;
     }
  }
}
//------------------------------------------------------------------------------
inline void CreateMapElevation(mapcache_context *ctx, mapcache_map *map, 
      datasetinfo* pSrcDataset, datasetinfo* pDstDataset, 
      int sourcetilewidth, int sourcetileheight, mapcache_source_gdal* gdal, 
      OGRCoordinateTransformationH pCTBack, OGRCoordinateTransformationH pCTWGS84, 
      double minx_data_wgs84, double miny_data_wgs84,
      double maxx_data_wgs84,double maxy_data_wgs84, int nXOff,
      int nYOff, double scalex, double scaley, unsigned char* pData, float NODATA, int datatype, int elevationblock)
{
  //----------------------------------------------------------------------------
  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->is_elevation = MC_ELEVATION_YES;
  map->raw_image->w = elevationblock;
  map->raw_image->h = elevationblock;
  map->raw_image->stride = 4 * elevationblock;
  map->raw_image->x0 = map->extent.minx / map->grid_link->grid->extent.minx;
  map->raw_image->y0 = map->extent.miny / map->grid_link->grid->extent.miny;
  map->raw_image->x1 = map->extent.maxx / map->grid_link->grid->extent.maxx;
  map->raw_image->y1 = map->extent.maxy / map->grid_link->grid->extent.maxy;
  map->raw_image->data = malloc(elevationblock*elevationblock*4);
  apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
  
  int x,y;
  for (y=0;y<elevationblock;y++)
  {
     for (x=0;x<elevationblock;x++)
     {
       unsigned char r,g,b,a; 
       double x_coord = pDstDataset->ulx + ((double)x)*pDstDataset->pixelwidth;
       double y_coord = pDstDataset->uly - ((double)y)*pDstDataset->pixelheight;
       
       // note: non-global datasets may have too large numbers (overflow) to
       //       process on a global system. Therefore this additional check is
       //       implemented
       if (gdal->extent != NULL)
       {
            double x_wgs84 = x_coord;
            double y_wgs84 = y_coord;
            OCTTransform(pCTWGS84, 1, &x_wgs84, &y_wgs84, NULL);
            
            if (x_wgs84>=minx_data_wgs84 &&
                x_wgs84<=maxx_data_wgs84 &&
                y_wgs84>=miny_data_wgs84 &&
                y_wgs84<=maxy_data_wgs84)
            {
              // pixel will be inside dataset!
              OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
              double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
              double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
              xx -= nXOff;
              yy -= nYOff;
              xx *= scalex;
              yy *= scaley;
            
              _ReadImageDataMemElv(pData, sourcetilewidth, 
                                              sourcetileheight, (int)(xx), (int)(yy), 
                                              &r,&g,&b,&a, NODATA, datatype);
            }
            else
            {  // outside global extent -> completely transparent...
               r=0;
               g=0;
               b=0;
               a=0;
            }
       }
       else
       {
          OCTTransform(pCTBack, 1, &x_coord, &y_coord, NULL);            
          double xx = pSrcDataset->affineTransformation_inverse[0] + x_coord * pSrcDataset->affineTransformation_inverse[1] + y_coord * pSrcDataset->affineTransformation_inverse[2];
          double yy = pSrcDataset->affineTransformation_inverse[3] + x_coord * pSrcDataset->affineTransformation_inverse[4] + y_coord * pSrcDataset->affineTransformation_inverse[5];
          xx -= nXOff;
          yy -= nYOff;
          xx *= scalex;
          yy *= scaley;
         
          _ReadImageDataMemElv(pData, sourcetilewidth, 
                                          sourcetileheight, (int)(xx), (int)(yy), 
                                          &r,&g,&b,&a, NODATA, datatype);
       }
       
       map->raw_image->data[4*elevationblock*y+4*x+0] = b;
       map->raw_image->data[4*elevationblock*y+4*x+1] = g;
       map->raw_image->data[4*elevationblock*y+4*x+2] = r;
       map->raw_image->data[4*elevationblock*y+4*x+3] = a;
     }
  }
}
//------------------------------------------------------------------------------
/**
 * \private \memberof mapcache_source_gdal
 */
void _mapcache_source_gdal_render_map_image(mapcache_context *ctx, mapcache_map *map)
{
  double minx, miny, maxx, maxy;
  double minx_data_wgs84, miny_data_wgs84, maxx_data_wgs84, maxy_data_wgs84;

  //double gminx, gminy, gmaxx, gmaxy;
  int tilewidth, tileheight;
  char *dstSRS;
  char *srcSRS = "";
  datasetinfo oSrcDataset;
  datasetinfo oDstDataset;
  
  OGRCoordinateTransformationH pCT;
  OGRCoordinateTransformationH pCTBack;
  OGRCoordinateTransformationH pCTWGS84 = NULL;
  
  double quality = 2.0;
  
  OGRSpatialReferenceH srcref;
  OGRSpatialReferenceH dstref;
  OGRSpatialReferenceH wgs84ref = NULL;
  GDALDatasetH hDataset;
  
  mapcache_source_gdal *gdal = (mapcache_source_gdal*)map->tileset->source;
  
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
  
  if (gdal->extent != NULL)
  {
    wgs84ref = OSRNewSpatialReference(NULL);
    if (OSRImportFromEPSG(wgs84ref, 4326) != OGRERR_NONE)
    {
      ctx->set_error(ctx,500,"Error: can't create spatial reference for WGS84");
      return; 
    }
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
  if (gdal->extent != NULL)
  {
    pCTWGS84   = OCTNewCoordinateTransformation(dstref, wgs84ref);
    if (!pCTWGS84)
    {
      ctx->set_error(ctx,500,"Error: can't create transformation to WGS84");
      return;  
    }
  }
  
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
  
  // warning: this is not always valid. For now this is restricted
  // to projections like mercator -> wgs84.
  // only use the "extent" tag for such datasets.
  if (gdal->extent != NULL)
  {
    minx_data_wgs84 = gdal->extent->minx;
    miny_data_wgs84 = gdal->extent->miny;
    maxx_data_wgs84 = gdal->extent->maxx;
    maxy_data_wgs84 = gdal->extent->maxy;
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
    double x_tile,y_tile;
    int x,y;
    x = p;
    y = 0;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    { 
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
    x = p;
    y = oDstDataset.nSizeY;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    {
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
  }
  for (p=0;p<=oDstDataset.nSizeY;p++)
  {
    double x_tile,y_tile;
    int x,y;
    x = 0;
    y = p;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    {
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
    x = oDstDataset.nSizeX;
    y = p;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    {
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
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
    map->raw_image->is_blank = MC_EMPTY_YES;
    memset(map->raw_image->data, 0, map->width*map->height*4);
    apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
    
    OCTDestroyCoordinateTransformation(pCT);   
    OCTDestroyCoordinateTransformation(pCTBack); 
    OSRDestroySpatialReference(dstref);
    OSRDestroySpatialReference(srcref);
    GDALClose(hDataset);
    return;
  }
  
  double x0,y0,x1,y1;
  int nXOff, nYOff;   // Start pixel y
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
    map->raw_image->is_blank = MC_EMPTY_YES;
    memset(map->raw_image->data, 0, map->width*map->height*4);
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
  unsigned char *pData = NULL;
  int bands = 0;
  float NODATA = -9999;
  int datatype_bytes;
  int datatype;
  
 
  if (oSrcDataset.nBands == 3)
  {
  
    bands = 3;
    pData = apr_palloc(ctx->pool,sourcetilewidth*sourcetileheight*3);
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
  }
  else if (oSrcDataset.nBands == 4)
  {
    bands = 4;
    pData = apr_palloc(ctx->pool,sourcetilewidth*sourcetileheight*4);
    if (pData == NULL)
    {
      ctx->set_error(ctx,500,"Error: Cant allocate memory: %i bytes", sourcetilewidth*sourcetileheight*4);
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
           4,               // pixelspace
           4*sourcetilewidth, //linespace, 
           1                  //bandspace.
    ))
    {
      ctx->set_error(ctx,500,"Error: GDALDatasetRasterIO failed!");
      return;  
    }
  }
  else if (oSrcDataset.nBands == 1)
  {
    bands = 1;
    int success;
    GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1); 
    if (hBand)
    {
      NODATA = (float)GDALGetRasterNoDataValue(hBand, &success);
      //ctx->log(ctx,MAPCACHE_NOTICE,"**NODATA Value: %f", NODATA);
    }
    
    GDALDataType rdd = GDALGetRasterDataType(hBand);
    
    switch (rdd)
    {
      case GDT_Byte:
        datatype = 7;
        datatype_bytes = 1;
      break;
      case GDT_UInt16:
        datatype = 5;
        datatype_bytes = GDALGetDataTypeSize(GDT_UInt16) / 8;
      break;
      case GDT_Int16:
        datatype = 6;
        datatype_bytes = GDALGetDataTypeSize(GDT_Int16) / 8;
      break;
      case GDT_UInt32:
        datatype = 1;
        datatype_bytes = GDALGetDataTypeSize(GDT_UInt32) / 8;
      break;
      case GDT_Int32:
        datatype = 2;
        datatype_bytes = GDALGetDataTypeSize(GDT_Int32) / 8;
      break;
      case GDT_Float32:
        datatype = 3;
        datatype_bytes = GDALGetDataTypeSize(GDT_Float32) / 8;
      break;
      case GDT_Float64:
        datatype = 4;
        datatype_bytes = GDALGetDataTypeSize(GDT_Float64) / 8;
      break;
      default:
         ctx->set_error(ctx,500,"Error: Unsupported Raster Data Type");
      return;
      }
    
    pData = apr_palloc(ctx->pool,sourcetilewidth*sourcetileheight*datatype_bytes);
    if (pData == NULL)
    {
      ctx->set_error(ctx,500,"Error: Cant allocate memory: %i bytes", sourcetilewidth*sourcetileheight*datatype_bytes);
      return; 
    } 
 
    if (CE_None != GDALDatasetRasterIO(hDataset, GF_Read, 
           nXOff, nYOff,    // Pixel position in source dataset 
           nXSize, nYSize,  // width/height in source dataset
           pData,        // target buffer
           sourcetilewidth, sourcetileheight, // dimension of target buffer
           rdd,      
           oSrcDataset.nBands, // number of input bands
           NULL,            // band map is ignored
           datatype_bytes,  // pixelspace
           datatype_bytes*sourcetilewidth, //linespace, 
           1                  //bandspace.
    ))
    {
      ctx->set_error(ctx,500,"Error: GDALDatasetRasterIO failed!");
      return;  
    }
  }
  else
  {
    bands = 0;
    ctx->set_error(ctx,500,"Error: Unsupported number of bands");
    return; 
  }
  // Close Dataset
  GDALClose( hDataset );

  if (bands == 3)
  {
    CreateMapBGR(ctx, map, &oSrcDataset, &oDstDataset, 
                 sourcetilewidth, sourcetileheight, gdal, pCTBack, pCTWGS84,
                 minx_data_wgs84,miny_data_wgs84,maxx_data_wgs84,maxy_data_wgs84,
                 nXOff, nYOff, scalex, scaley, pData
                );
   
  }
  else if (bands == 4)
  {
    CreateMapBGRA(ctx, map, &oSrcDataset, &oDstDataset, 
                 sourcetilewidth, sourcetileheight, gdal, pCTBack, pCTWGS84,
                 minx_data_wgs84,miny_data_wgs84,maxx_data_wgs84,maxy_data_wgs84,
                 nXOff, nYOff, scalex, scaley, pData
                );
  }
  else if (bands == 1)
  {
    CreateMapGray(ctx, map, &oSrcDataset, &oDstDataset, 
                 sourcetilewidth, sourcetileheight, gdal, pCTBack, pCTWGS84,
                 minx_data_wgs84,miny_data_wgs84,maxx_data_wgs84,maxy_data_wgs84,
                 nXOff, nYOff, scalex, scaley, pData, NODATA, datatype);
  }
  
  
  // free SRS
  OCTDestroyCoordinateTransformation(pCT);   
  OCTDestroyCoordinateTransformation(pCTBack); 
  OSRDestroySpatialReference(dstref);
  OSRDestroySpatialReference(srcref);
  
  apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
}

//------------------------------------------------------------------------------
/**
 * \private \memberof mapcache_source_gdal
 */
void _mapcache_source_gdal_render_map_elevation(mapcache_context *ctx, mapcache_map *map)
{
  int elevationblock = map->grid_link->grid->elevationblock;
  double minx, miny, maxx, maxy;
  double minx_data_wgs84, miny_data_wgs84, maxx_data_wgs84, maxy_data_wgs84;

  //double gminx, gminy, gmaxx, gmaxy;
  int tilewidth, tileheight;
  char *dstSRS;
  char *srcSRS = "";
  datasetinfo oSrcDataset;
  datasetinfo oDstDataset;
  
  OGRCoordinateTransformationH pCT;
  OGRCoordinateTransformationH pCTBack;
  OGRCoordinateTransformationH pCTWGS84 = NULL;
  
  double quality = 2.0;
  
  OGRSpatialReferenceH srcref;
  OGRSpatialReferenceH dstref;
  OGRSpatialReferenceH wgs84ref = NULL;
  GDALDatasetH hDataset;
  
  mapcache_source_gdal *gdal = (mapcache_source_gdal*)map->tileset->source;
  
  // extent of tile:
  minx = map->extent.minx;
  miny = map->extent.miny;
  maxx = map->extent.maxx;
  maxy = map->extent.maxy;
  // width of tile (pixels)
  tilewidth = elevationblock;
  // heoight of tile (pixel)
  tileheight = elevationblock;
  
 
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
  
  if (gdal->extent != NULL)
  {
    wgs84ref = OSRNewSpatialReference(NULL);
    if (OSRImportFromEPSG(wgs84ref, 4326) != OGRERR_NONE)
    {
      ctx->set_error(ctx,500,"Error: can't create spatial reference for WGS84");
      return; 
    }
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
  if (gdal->extent != NULL)
  {
    pCTWGS84   = OCTNewCoordinateTransformation(dstref, wgs84ref);
    if (!pCTWGS84)
    {
      ctx->set_error(ctx,500,"Error: can't create transformation to WGS84");
      return;  
    }
  }
  
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
  
  // warning: this is not always valid. For now this is restricted
  // to projections like mercator -> wgs84.
  // only use the "extent" tag for such datasets.
  if (gdal->extent != NULL)
  {
    minx_data_wgs84 = gdal->extent->minx;
    miny_data_wgs84 = gdal->extent->miny;
    maxx_data_wgs84 = gdal->extent->maxx;
    maxy_data_wgs84 = gdal->extent->maxy;
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
    double x_tile,y_tile;
    int x,y;
    x = p;
    y = 0;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    { 
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
    x = p;
    y = oDstDataset.nSizeY;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    {
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
  }
  for (p=0;p<=oDstDataset.nSizeY;p++)
  {
    double x_tile,y_tile;
    int x,y;
    x = 0;
    y = p;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    {
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
    x = oDstDataset.nSizeX;
    y = p;
    y_tile = oDstDataset.affineTransformation[3] + x*oDstDataset.affineTransformation[4] + y*oDstDataset.affineTransformation[5];
    x_tile = oDstDataset.affineTransformation[0] + x*oDstDataset.affineTransformation[1] + y*oDstDataset.affineTransformation[2];
    if (OCTTransform(pCTBack, 1, &x_tile, &y_tile, NULL))
    {
      dest_ulx = GM_MIN(x_tile, dest_ulx);
      dest_lry = GM_MIN(y_tile, dest_lry);
      dest_lrx = GM_MAX(x_tile, dest_lrx);
      dest_uly = GM_MAX(y_tile, dest_uly);
    }
  }
    
  if (!TestRectRectIntersect(dest_ulx, dest_uly, dest_lrx, dest_lry,
                             oSrcDataset.ulx,oSrcDataset.uly,oSrcDataset.lrx,oSrcDataset.lry))
  {
    map->raw_image = mapcache_image_create(ctx);
    map->raw_image->is_elevation = MC_ELEVATION_YES;
    map->raw_image->w = elevationblock;
    map->raw_image->h = elevationblock;
    map->raw_image->stride = 4 * elevationblock;
    map->raw_image->data = malloc(elevationblock*elevationblock*4);
    map->raw_image->x0 = minx / map->grid_link->grid->extent.minx;
    map->raw_image->y0 = miny / map->grid_link->grid->extent.miny;
    map->raw_image->x1 = maxx / map->grid_link->grid->extent.maxx;
    map->raw_image->y1 = maxy / map->grid_link->grid->extent.maxy;
    map->raw_image->is_blank = MC_EMPTY_YES;
    memset(map->raw_image->data, 0, elevationblock*elevationblock*4);
    apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
    
    OCTDestroyCoordinateTransformation(pCT);   
    OCTDestroyCoordinateTransformation(pCTBack); 
    OSRDestroySpatialReference(dstref);
    OSRDestroySpatialReference(srcref);
    GDALClose(hDataset);
    return;
  }
  
  double x0,y0,x1,y1;
  int nXOff, nYOff;   // Start pixel y
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
    map->raw_image->is_elevation = MC_ELEVATION_YES;
    map->raw_image->w = elevationblock;
    map->raw_image->h = elevationblock;
    map->raw_image->stride = 4 * elevationblock;
    map->raw_image->data = malloc(elevationblock*elevationblock*4);
    map->raw_image->x0 = minx / map->grid_link->grid->extent.minx;
    map->raw_image->y0 = miny / map->grid_link->grid->extent.miny;
    map->raw_image->x1 = maxx / map->grid_link->grid->extent.maxx;
    map->raw_image->y1 = maxy / map->grid_link->grid->extent.maxy;
    map->raw_image->is_blank = MC_EMPTY_YES;
    memset(map->raw_image->data, 0, elevationblock*elevationblock*4);
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
  
 
  // Retrieve data from source
  unsigned char *pData = NULL;
  int bands = 0;
  float NODATA = -9999;
  int datatype_bytes;
  int datatype;

 
  if (oSrcDataset.nBands == 1)
  {
    bands = 1;
    int success;
    GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1); 
    if (hBand)
    {
      NODATA = (float)GDALGetRasterNoDataValue(hBand, &success);
      //ctx->log(ctx,MAPCACHE_NOTICE,"**NODATA Value: %f", NODATA);
    }
    
    GDALDataType rdd = GDALGetRasterDataType(hBand);
    
    switch (rdd)
    {
      case GDT_Byte:
        datatype = 7;
        datatype_bytes = 1;
      break;
      case GDT_UInt16:
        datatype = 5;
        datatype_bytes = GDALGetDataTypeSize(GDT_UInt16) / 8;
      break;
      case GDT_Int16:
        datatype = 6;
        datatype_bytes = GDALGetDataTypeSize(GDT_Int16) / 8;
      break;
      case GDT_UInt32:
        datatype = 1;
        datatype_bytes = GDALGetDataTypeSize(GDT_UInt32) / 8;
      break;
      case GDT_Int32:
        datatype = 2;
        datatype_bytes = GDALGetDataTypeSize(GDT_Int32) / 8;
      break;
      case GDT_Float32:
        datatype = 3;
        datatype_bytes = GDALGetDataTypeSize(GDT_Float32) / 8;
      break;
      case GDT_Float64:
        datatype = 4;
        datatype_bytes = GDALGetDataTypeSize(GDT_Float64) / 8;
      break;
      default:
         ctx->set_error(ctx,500,"Error: Unsupported Raster Data Type");
      return;
      }
    
    pData = apr_palloc(ctx->pool,sourcetilewidth*sourcetileheight*datatype_bytes);
    if (pData == NULL)
    {
      ctx->set_error(ctx,500,"Error: Cant allocate memory: %i bytes", sourcetilewidth*sourcetileheight*datatype_bytes);
      return; 
    } 
 
    if (CE_None != GDALDatasetRasterIO(hDataset, GF_Read, 
           nXOff, nYOff,    // Pixel position in source dataset 
           nXSize, nYSize,  // width/height in source dataset
           pData,        // target buffer
           sourcetilewidth, sourcetileheight, // dimension of target buffer
           rdd,      
           oSrcDataset.nBands, // number of input bands
           NULL,            // band map is ignored
           datatype_bytes,  // pixelspace
           datatype_bytes*sourcetilewidth, //linespace, 
           1                  //bandspace.
    ))
    {
      ctx->set_error(ctx,500,"Error: GDALDatasetRasterIO failed!");
      return;  
    }
  }
  else
  {
    bands = 0;
    ctx->set_error(ctx,500,"Error: Unsupported number of bands");
    return; 
  }
  // Close Dataset
  GDALClose( hDataset );


  CreateMapElevation(ctx, map, &oSrcDataset, &oDstDataset, 
                 sourcetilewidth, sourcetileheight, gdal, pCTBack, pCTWGS84,
                 minx_data_wgs84,miny_data_wgs84,maxx_data_wgs84,maxy_data_wgs84,
                 nXOff, nYOff, scalex, scaley, pData, NODATA, datatype, elevationblock);
    
     
    
   /* map->raw_image = mapcache_image_create(ctx);
    map->raw_image->is_elevation = MC_ELEVATION_YES;
    map->raw_image->w = elevationblock;
    map->raw_image->h = elevationblock;
    map->raw_image->stride = 4 * elevationblock;
    map->raw_image->data = malloc(elevationblock*elevationblock*4);
    memset(map->raw_image->data, 0, elevationblock*elevationblock*4);
    apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);*/
    
 
    
  // free SRS
  OCTDestroyCoordinateTransformation(pCT);   
  OCTDestroyCoordinateTransformation(pCTBack); 
  OSRDestroySpatialReference(dstref);
  OSRDestroySpatialReference(srcref);
  
 
  
}
//------------------------------------------------------------------------------
/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_gdal_render_map(mapcache_context *ctx, mapcache_map *map)
{
  int is_elevation = map->tileset->elevation;
  
  if (is_elevation)
  {
    _mapcache_source_gdal_render_map_elevation(ctx, map);
  }
  else
  {
    _mapcache_source_gdal_render_map_image(ctx, map);
  }
  
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
  char* next;
  
  char* pch = strtok_r(text," ",&next);
  if (pch != NULL) { minx = atof(pch); }
  else {return;}
  pch = strtok_r(NULL," ",&next);
  if (pch != NULL) { miny = atof(pch); }
  else {return;}
   pch = strtok_r(NULL," ",&next);
  if (pch != NULL) { maxx = atof(pch); }
  else {return;}
   pch = strtok_r(NULL," ",&next);
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

