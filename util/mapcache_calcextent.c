#include "mapcache.h"
#include <stdio.h>
#include <apr_getopt.h>

#ifdef USE_GDAL
#include <gdal.h>
#include <cpl_conv.h>

#include "gdal_alg.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"
#endif

#define GM_MIN(x,y)  ((x<y) ? x : y)
#define GM_MAX(x,y)  ((x>y) ? x : y)
//------------------------------------------------------------------------------
typedef struct datasetinfo datasetinfo;
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
static const apr_getopt_option_t calcextent_options[] = {
  /* long-option, short-option, has-arg flag, description */
  { "file", 'f', TRUE, "input dataset to calculate mercator extents"},
  { "srs",  's', TRUE, "spatial reference system of input dataset"},
  { "help", 'h', FALSE,"show help" },
  { NULL, 0, 0, NULL },
};

//------------------------------------------------------------------------------
int usage(const char *progname, char *msg)
{
  int i=0;
  if(msg)
    printf("%s\nusage: %s options\n",msg,progname);
  else
    printf("usage: %s options\n",progname);

  while(calcextent_options[i].name) {
    if(calcextent_options[i].has_arg==TRUE) {
      printf("-%c|--%s [value]: %s\n",calcextent_options[i].optch,calcextent_options[i].name, calcextent_options[i].description);
    } else {
      printf("-%c|--%s: %s\n",calcextent_options[i].optch,calcextent_options[i].name, calcextent_options[i].description);
    }
    i++;
  }
  apr_terminate();
  return 1;
}

mapcache_cfg *cfg;
mapcache_context ctx;
//------------------------------------------------------------------------------
void mapcache_context_extent_log(mapcache_context *ctx, mapcache_log_level level, char *msg, ...)
{
  va_list args;
  va_start(args,msg);
  vfprintf(stderr,msg,args);
  va_end(args);
  printf("\n");
}
//------------------------------------------------------------------------------
int main(int argc, const char **argv)
{
#ifdef USE_GDAL
  apr_getopt_t *opt;
  int rv,optch;
  const char *optarg;
  const char* inputfile = 0;
  const char *srs=NULL;
  OGRSpatialReferenceH srcref;
  OGRSpatialReferenceH dstref;

  apr_initialize();
  apr_pool_create(&ctx.pool,NULL);
  mapcache_context_init(&ctx);
  ctx.process_pool = ctx.pool;
  cfg = mapcache_configuration_create(ctx.pool);
  ctx.config = cfg;
  ctx.log= mapcache_context_extent_log;
  apr_getopt_init(&opt, ctx.pool, argc, argv);

  printf("Welcome to mapcache_calcextent!\nNOTE: This is an experimental first version supporting WGS84 (EPSG:4326) as destination only.\n");
  
  while ((rv = apr_getopt_long(opt, calcextent_options, &optch, &optarg)) == APR_SUCCESS) 
  {
    switch (optch) {
      case 'h':
        return usage(argv[0],NULL);
        break;
      case 'f':
        inputfile = optarg;
        break;
      case 's':
        srs = optarg;
        break;
    }
  }
  
  if (rv != APR_EOF) 
  {
    return usage(argv[0],"bad options");
  }
  
  if( ! inputfile ) 
  {
    return usage(argv[0],"input file not specified");
  }
  
  GDALDatasetH hDataset;
  
  printf("Initializing GDAL...");
  GDALAllRegister();
  CPLErrorReset();
  printf("Ok\n");
  
  hDataset = GDALOpen( inputfile, GA_ReadOnly );
  if( hDataset == NULL ) 
  {
    printf("[ERROR] failed to open %s\n",inputfile);
    return 1;
  }  
   
  const char *srcSRS = "";
   
  if (srs == NULL)
  {
    printf("Retrieving Spatial Reference System...");
    if( GDALGetProjectionRef( hDataset ) != NULL && strlen(GDALGetProjectionRef( hDataset )) > 0 )
      srcSRS = GDALGetProjectionRef(hDataset);
    else if( GDALGetGCPProjection( hDataset ) != NULL && strlen(GDALGetGCPProjection(hDataset)) > 0 && GDALGetGCPCount( hDataset ) > 1 )
      srcSRS = GDALGetGCPProjection( hDataset);
    printf("Ok\n");
  }
  else
  {
    printf ("Using user provided SRS: %s", srs);
    srcSRS = srs;
  }
  
  printf("Setting up Spatial Reference System...");
  // Setup Source SRS
  srcref = OSRNewSpatialReference(NULL);
  if (OSRSetFromUserInput(srcref, srcSRS) != OGRERR_NONE)
  {
     printf("ERROR: can't create source spatial reference\n");
     return 1; 
  }
  printf("Ok\n");
  
  printf("Setting up target Spatial Reference System...");
  // Setup Destination Spatial Reference System
  dstref = OSRNewSpatialReference(NULL);
  if (OSRSetFromUserInput (dstref, "EPSG:4326" ) != OGRERR_NONE) 
  {
     printf("ERROR: can't create dest spatial reference\n");
     return 1; 
  }
  printf("Ok\n");
  
  OGRCoordinateTransformationH pCT = OCTNewCoordinateTransformation(srcref, dstref);
  
  if (!pCT)
  {
   printf("ERROR: can't create forward transformation\n");
   return 1;  
  }
  
  datasetinfo oSrcDataset;
  GDALGetGeoTransform(hDataset, oSrcDataset.affineTransformation);
  oSrcDataset.nBands = GDALGetRasterCount(hDataset);
  oSrcDataset.nSizeX = GDALGetRasterXSize(hDataset);
  oSrcDataset.nSizeY = GDALGetRasterYSize(hDataset);
  oSrcDataset.pixelwidth  = oSrcDataset.affineTransformation[1];
  oSrcDataset.pixelheight = oSrcDataset.affineTransformation[5];
  oSrcDataset.ulx = oSrcDataset.affineTransformation[0];
  oSrcDataset.uly = oSrcDataset.affineTransformation[3];
  oSrcDataset.lrx = oSrcDataset.ulx + oSrcDataset.affineTransformation[1] * oSrcDataset.nSizeX;
  oSrcDataset.lry = oSrcDataset.uly + oSrcDataset.affineTransformation[5] * oSrcDataset.nSizeY;

  printf("Ok\n");
  printf("Dataset size: (%i, %i)\n", oSrcDataset.nSizeX,oSrcDataset.nSizeY);
  
  // Rectangle within source required for tile
  double dest_ulx = 1e20;
  double dest_lry = 1e20;
  double dest_lrx = -1e20;
  double dest_uly = -1e20;
  
  double x,y;
  //Transform every pixel along border of tile
  int p;
  for (p=0;p<=oSrcDataset.nSizeX;p++)
  {
    double lng,lat;
    x = p;
    y = 0;
    lat = oSrcDataset.affineTransformation[3] + x*oSrcDataset.affineTransformation[4] + y*oSrcDataset.affineTransformation[5];
    lng = oSrcDataset.affineTransformation[0] + x*oSrcDataset.affineTransformation[1] + y*oSrcDataset.affineTransformation[2];
    if (OCTTransform(pCT, 1, &lng, &lat, NULL))
    { 
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
    x = p;
    y = oSrcDataset.nSizeY;
    lat = oSrcDataset.affineTransformation[3] + x*oSrcDataset.affineTransformation[4] + y*oSrcDataset.affineTransformation[5];
    lng = oSrcDataset.affineTransformation[0] + x*oSrcDataset.affineTransformation[1] + y*oSrcDataset.affineTransformation[2];
    if (OCTTransform(pCT, 1, &lng, &lat, NULL))
    {
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
  }
  for (p=0;p<=oSrcDataset.nSizeY;p++)
  {
    double lng,lat; 
    x = 0;
    y = p;
    lat = oSrcDataset.affineTransformation[3] + x*oSrcDataset.affineTransformation[4] + y*oSrcDataset.affineTransformation[5];
    lng = oSrcDataset.affineTransformation[0] + x*oSrcDataset.affineTransformation[1] + y*oSrcDataset.affineTransformation[2];
    if (OCTTransform(pCT, 1, &lng, &lat, NULL))
    {
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
    x = oSrcDataset.nSizeX;
    y = p;
    lat = oSrcDataset.affineTransformation[3] + x*oSrcDataset.affineTransformation[4] + y*oSrcDataset.affineTransformation[5];
    lng = oSrcDataset.affineTransformation[0] + x*oSrcDataset.affineTransformation[1] + y*oSrcDataset.affineTransformation[2];
    if (OCTTransform(pCT, 1, &lng, &lat, NULL))
    {
      dest_ulx = GM_MIN(lng, dest_ulx);
      dest_lry = GM_MIN(lat, dest_lry);
      dest_lrx = GM_MAX(lng, dest_lrx);
      dest_uly = GM_MAX(lat, dest_uly);
    }
  }
  
  printf("EXTENT: %3.15f %3.15f %3.15f %3.15f\n", dest_ulx, dest_lry, dest_lrx, dest_uly);
 
   OCTDestroyCoordinateTransformation(pCT);    
   OSRDestroySpatialReference(dstref);
   OSRDestroySpatialReference(srcref);
   GDALClose(hDataset);
  
  apr_terminate();
  return 0;
  
#else
  printf("Error: mapcache is not compiled with GDAL support.\n");
  return 0;
#endif
  
}
