/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: filesytem cache backend.
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

#ifdef USE_S3

#include <apr_file_info.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <string.h>
#include <errno.h>
#include <apr_mmap.h>


//------------------------------------------------------------------------------
typedef struct object_userdata object_userdata;
struct object_userdata
{
    unsigned char* buffer;   // memory of buffer
    
    FILE* source;   // source file (or memory-stream) to put to S3
    int   status;   // status of put: 
                    //     0=ok, 
                    //     1=file not found
                    //     2=other error
    int64_t length;       // size of file / buffer
    int64_t lastModified; // last modified date
    
    
    int64_t _memoryPos;    // current memory position (private)
    int     _createbuffer; // create buffer ?
};

//------------------------------------------------------------------------------

S3Status responsePropertiesCallback(
                const S3ResponseProperties *properties,
                void *callbackData)
{
        object_userdata* data = (object_userdata*)callbackData;
        
        data->_memoryPos = 0;
        data->length =  properties->contentLength;
        data->lastModified = properties->lastModified;
        
        
        if (data->length > 0 && data->_createbuffer)
        {
          data->buffer = (unsigned char*) malloc(data->length);
        }
       
  
        return S3StatusOK;
}

//------------------------------------------------------------------------------

static void responseCompleteCallback(
                S3Status status,
                const S3ErrorDetails *error,
                void *callbackData)
{
    object_userdata* data = (object_userdata*)callbackData;
    
    if (status == S3StatusOK)
    {
      data->status = 0;
    }
    else if (status == S3StatusErrorNoSuchKey)
    {
      data->status = 1;
    }
    else 
    {
      data->status = 2;
    }
    return;
}

//------------------------------------------------------------------------------

S3ResponseHandler responseHandler =
{
        &responsePropertiesCallback,
        &responseCompleteCallback
};
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
static S3Status getObjectDataCallback(int bufferSize, const char *buffer, void *callbackData)
{  
        object_userdata* data = (object_userdata*)callbackData;
        
        if (!data->buffer)
        {
            return S3StatusAbortedByCallback;
        }
          
        if (bufferSize+data->_memoryPos > data->length)
        {
            return S3StatusAbortedByCallback;
        }
        
        void* dest_adr = data->buffer+data->_memoryPos;
        memcpy(dest_adr, buffer, bufferSize);
        data->_memoryPos += bufferSize;
        
        return S3StatusOK;
}

//------------------------------------------------------------------------------
static int putObjectDataCallback(int bufferSize, char *buffer, void *callbackData)
{
    object_userdata* data = (object_userdata*)callbackData;

    int ret = 0;

    if (data->_memoryPos >= data->length)
    {
      return 0;
    }
    
    if (data->length)
    {
      memcpy(buffer, data->buffer+data->_memoryPos, bufferSize);
      data->_memoryPos += bufferSize;
      return bufferSize;
    }
    
    return 0;
}

//------------------------------------------------------------------------------
// Delete File:
void DeleteS3(S3BucketContext* bucketContext, char* filename)
{
  object_userdata tmp;
  tmp._createbuffer = 1;
  
  S3_delete_object(bucketContext, filename, NULL, &responseHandler, &tmp);
}
//------------------------------------------------------------------------------
// Test if File exists:
int ExistsS3(S3BucketContext* bucketContext, const char* filename)
{
  object_userdata tmp;
  
  tmp._createbuffer = 0; // don't create a buffer!
  
  S3_head_object(bucketContext, filename, NULL, &responseHandler, &tmp);
  
  if (tmp.status == 0)
  {
      return TRUE;
  }
  
  return FALSE;
}

//------------------------------------------------------------------------------
// Retrieve File:
// Don't forget to call free after retrieving the file.
void GetS3(S3BucketContext* bucketContext, const char* filename, object_userdata* gu)
{
  S3GetObjectHandler getObjectHandler;
  getObjectHandler.responseHandler = responseHandler;
  getObjectHandler.getObjectDataCallback = &getObjectDataCallback;

  gu->_createbuffer = 1;
  
  S3_get_object(bucketContext,  // S3BucketContext
                  filename,     // key
                  NULL,            // S3GetConditions
                  0,               // Start Byte
                  0,               // Bytecount
                  NULL,            // S3RequestContext
                  &getObjectHandler, // S3GetObjectHandler
                  gu);            // callbackData
}
//------------------------------------------------------------------------------
// Put File:
void SetS3(S3BucketContext* bucketContext, const char* filename, object_userdata* gu)
{
    S3PutObjectHandler putObjectHandler;
    putObjectHandler.responseHandler = responseHandler;
    putObjectHandler.putObjectDataCallback = &putObjectDataCallback;

   gu->_createbuffer = 0;
   if (gu->length == 0 || gu->buffer == 0)
   {
      return; // nothing to write...
   }
   gu->_memoryPos = 0;
   
   //---------------------------------------------------------------------
   //---------------------------------------------------------------------
   // WARNING: To support reduced redundandy libs3 must be modified:
   //     In the header (libs3.h) change 
   //           #define S3_METADATA_HEADER_NAME_PREFIX     "x-amz-meta-"
   //    to     #define S3_METADATA_HEADER_NAME_PREFIX     "x-amz-"
   //  and compile the library again. 
   //  WARNING2: after this change you can't use metadata....
   //  I don't know if there is another way... 
   //   (...If there isn't, I will add header support to libs3 in future) 
   //---------------------------------------------------------------------
   //---------------------------------------------------------------------
   
   S3PutProperties putprop;
   S3NameValue     storage_class;
   storage_class.name = "storage-class";
   storage_class.value = "REDUCED_REDUNDANCY";
   
   putprop.contentType = NULL;
   putprop.md5 = NULL;
   putprop.cacheControl = NULL;
   putprop.contentDispositionFilename = NULL;
   putprop.contentEncoding = NULL;
   putprop.expires = 0;
   putprop.cannedAcl = S3CannedAclPublicRead;  // this should be customized.
                                               // use of S3CannedAclAuthenticatedRead if you don't want it to be public
   putprop.metaDataCount = 1;
   putprop.metaData = &storage_class;
   putprop.useServerSideEncryption = 0;

   
   S3_put_object(bucketContext, filename, gu->length, &putprop, NULL, &putObjectHandler, gu);
}
//------------------------------------------------------------------------------
/**
 * \brief returns base path for given tile
 *
 * \param tile the tile to get base path from
 * \param path pointer to a char* that will contain the filename
 * \private \memberof mapcache_cache_s3
 */
static void _mapcache_cache_s3_base_tile_key(mapcache_context *ctx, mapcache_tile *tile, char **path)
{
  *path = apr_pstrcat(ctx->pool,
                      ((mapcache_cache_s3*)tile->tileset->cache)->base_directory,"/",
                      tile->tileset->name,"/",
                      tile->grid_link->grid->name,
                      NULL);
  if(tile->dimensions) {
    const apr_array_header_t *elts = apr_table_elts(tile->dimensions);
    int i = elts->nelts;
    while(i--) {
      apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,i,apr_table_entry_t));
      const char *dimval = mapcache_util_str_sanitize(ctx->pool,entry->val,"/.",'#');
      *path = apr_pstrcat(ctx->pool,*path,"/",dimval,NULL);
    }
  }
}

//------------------------------------------------------------------------------
/**
 * \brief return filename for given tile
 *
 * \param tile the tile to get the key from
 * \param path pointer to a char* that will contain the filename
 * \param r
 * \private \memberof mapcache_cache_s3
 */
static void _mapcache_cache_s3_tilecache_tile_key(mapcache_context *ctx, mapcache_tile *tile, char **path)
{
  mapcache_cache_s3 *dcache = (mapcache_cache_s3*)tile->tileset->cache;
  if(dcache->base_directory) {
    char *start;
    _mapcache_cache_s3_base_tile_key(ctx, tile, &start);
    
    *path = apr_psprintf(ctx->pool,"%s/%u/%u/%u.%s" ,
                         start,
                         tile->z,
                         tile->y,
                         tile->x,
                         tile->tileset->format?tile->tileset->format->extension:"png");
  } else {
    *path = dcache->filename_template;
    *path = mapcache_util_str_replace(ctx->pool,*path, "{tileset}", tile->tileset->name);
    *path = mapcache_util_str_replace(ctx->pool,*path, "{grid}", tile->grid_link->grid->name);
    *path = mapcache_util_str_replace(ctx->pool,*path, "{ext}",
                                      tile->tileset->format?tile->tileset->format->extension:"png");
    if(strstr(*path,"{x}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{x}",
                                        apr_psprintf(ctx->pool,"%d",tile->x));
    else
      *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_x}",
                                        apr_psprintf(ctx->pool,"%d",
                                            tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1));
    if(strstr(*path,"{y}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{y}",
                                        apr_psprintf(ctx->pool,"%d",tile->y));
    else
      *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_y}",
                                        apr_psprintf(ctx->pool,"%d",
                                            tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1));
    if(strstr(*path,"{z}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{z}",
                                        apr_psprintf(ctx->pool,"%d",tile->z));
    else
      *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_z}",
                                        apr_psprintf(ctx->pool,"%d",
                                            tile->grid_link->grid->nlevels - tile->z - 1));
    if(tile->dimensions) {
      char *dimstring="";
      const apr_array_header_t *elts = apr_table_elts(tile->dimensions);
      int i = elts->nelts;
      while(i--) {
        apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,i,apr_table_entry_t));
        char *dimval = apr_pstrdup(ctx->pool,entry->val);
        char *iter = dimval;
        while(*iter) {
          /* replace dangerous characters by '#' */
          if(*iter == '.' || *iter == '/') {
            *iter = '#';
          }
          iter++;
        }
        dimstring = apr_pstrcat(ctx->pool,dimstring,"#",entry->key,"#",dimval,NULL);
      }
      *path = mapcache_util_str_replace(ctx->pool,*path, "{dim}", dimstring);
    }
  }
  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

//------------------------------------------------------------------------------

static void _mapcache_cache_s3_template_tile_key(mapcache_context *ctx, mapcache_tile *tile, char **path)
{
  mapcache_cache_s3 *dcache = (mapcache_cache_s3*)tile->tileset->cache;

  *path = dcache->filename_template;
  *path = mapcache_util_str_replace(ctx->pool,*path, "{tileset}", tile->tileset->name);
  *path = mapcache_util_str_replace(ctx->pool,*path, "{grid}", tile->grid_link->grid->name);
  *path = mapcache_util_str_replace(ctx->pool,*path, "{ext}",
                                    tile->tileset->format?tile->tileset->format->extension:"png");

  if(strstr(*path,"{x}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{x}",
                                      apr_psprintf(ctx->pool,"%d",tile->x));
  else
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_x}",
                                      apr_psprintf(ctx->pool,"%d",
                                          tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1));
  if(strstr(*path,"{y}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{y}",
                                      apr_psprintf(ctx->pool,"%d",tile->y));
  else
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_y}",
                                      apr_psprintf(ctx->pool,"%d",
                                          tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1));
  if(strstr(*path,"{z}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{z}",
                                      apr_psprintf(ctx->pool,"%d",tile->z));
  else
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_z}",
                                      apr_psprintf(ctx->pool,"%d",
                                          tile->grid_link->grid->nlevels - tile->z - 1));
  if(tile->dimensions) {
    char *dimstring="";
    const apr_array_header_t *elts = apr_table_elts(tile->dimensions);
    int i = elts->nelts;
    while(i--) {
      apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,i,apr_table_entry_t));
      char *dimval = apr_pstrdup(ctx->pool,entry->val);
      char *iter = dimval;
      while(*iter) {
        /* replace dangerous characters by '#' */
        if(*iter == '.' || *iter == '/') {
          *iter = '#';
        }
        iter++;
      }
      dimstring = apr_pstrcat(ctx->pool,dimstring,"#",entry->key,"#",dimval,NULL);
    }
    *path = mapcache_util_str_replace(ctx->pool,*path, "{dim}", dimstring);
  }

  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

//------------------------------------------------------------------------------

static void _mapcache_cache_s3_arcgis_tile_key(mapcache_context *ctx, mapcache_tile *tile, char **path)
{
  mapcache_cache_s3 *dcache = (mapcache_cache_s3*)tile->tileset->cache;
  if(dcache->base_directory) {
    char *start;
    _mapcache_cache_s3_base_tile_key(ctx, tile, &start);
    *path = apr_psprintf(ctx->pool,"%s/L%02d/R%08x/C%08x.%s" ,
                         start,
                         tile->z,
                         tile->y,
                         tile->x,
                         tile->tileset->format?tile->tileset->format->extension:"png");
  }

  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

//------------------------------------------------------------------------------
// EXISTS ?
static int _mapcache_cache_s3_has_tile(mapcache_context *ctx, mapcache_tile *tile)
{
  mapcache_cache_s3* cache;
  char *filename;
  apr_finfo_t finfo;
  int rv;
  
  cache = (mapcache_cache_s3*)tile->tileset->cache;
  
  if (cache->maxzoom>0)  // maxzoom is set
  {
    if (tile->z>cache->maxzoom)
    {
      return MAPCACHE_FALSE;
    }
  }
  
  cache->tile_key(ctx, tile, &filename);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FALSE;
  }
  
  S3BucketContext bucketContext;
  bucketContext.hostName = cache->host;
  bucketContext.bucketName = cache->bucket;
  bucketContext.protocol = S3ProtocolHTTP;
  bucketContext.uriStyle = S3UriStylePath;
  bucketContext.accessKeyId = cache->access_key;
  bucketContext.secretAccessKey = cache->secret_key;
  
  if (ExistsS3(&bucketContext, filename))
  {
    return MAPCACHE_TRUE;
  }
  
  return MAPCACHE_FALSE;
}

//------------------------------------------------------------------------------
// Delete Key from S3

static void _mapcache_cache_s3_delete(mapcache_context *ctx, mapcache_tile *tile)
{
  apr_status_t ret;
  mapcache_cache_s3* cache;
  char errmsg[120];
  char *filename;
  
  cache = (mapcache_cache_s3*)tile->tileset->cache;
  cache->tile_key(ctx, tile, &filename);
  GC_CHECK_ERROR(ctx);
  
  S3BucketContext bucketContext;
  bucketContext.hostName = cache->host;
  bucketContext.bucketName = cache->bucket;
  bucketContext.protocol = S3ProtocolHTTP;
  bucketContext.uriStyle = S3UriStylePath;
  bucketContext.accessKeyId = cache->access_key;
  bucketContext.secretAccessKey = cache->secret_key;

  DeleteS3(&bucketContext, filename);

}

//------------------------------------------------------------------------------

/**
 * \brief get file content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored in the file
 * \private \memberof mapcache_cache_s3
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_s3_get(mapcache_context *ctx, mapcache_tile *tile)
{
  char *filename;
  mapcache_cache_s3* cache;
  

  cache = (mapcache_cache_s3*)tile->tileset->cache;
  
  if (cache->maxzoom>0)  // maxzoom is set
  {
    if (tile->z>cache->maxzoom)
    {
      return MAPCACHE_CACHE_MISS;
    }
  }
  
  cache->tile_key(ctx, tile, &filename);
  if(GC_HAS_ERROR(ctx)) 
  {
    return MAPCACHE_FAILURE;
  }
  
  //ctx->log(ctx,MAPCACHE_NOTICE,"GET Tile %s", filename);
  
  S3BucketContext bucketContext;
  bucketContext.hostName = cache->host;
  bucketContext.bucketName = cache->bucket;
  bucketContext.protocol = S3ProtocolHTTP;
  bucketContext.uriStyle = S3UriStylePath;
  bucketContext.accessKeyId = cache->access_key;
  bucketContext.secretAccessKey = cache->secret_key;
  
  object_userdata gu;
  
  GetS3(&bucketContext, filename, &gu);
    
  if (gu.status == 0)
  {
    // tile downloaded successfully
    if (gu.buffer)
    {
      tile->encoded_data = mapcache_buffer_create(sizeof(mapcache_buffer),ctx->pool);
      tile->encoded_data->buf = (char*)gu.buffer;
      tile->encoded_data->size = (int)gu.length;
      tile->encoded_data->avail = (int)gu.length;
      tile->encoded_data->pool = 0;
      tile->mtime = 0 ; // gu.lastModified; // #todo this must be converted...
     
      // custom cleanup buffer: (mem was allocated with malloc...)
      apr_pool_cleanup_register(ctx->pool, gu.buffer,(void*)free, apr_pool_cleanup_null);
      return MAPCACHE_SUCCESS;
    }
  }
  else if (gu.status == 1)
  {
    return MAPCACHE_CACHE_MISS; // doesn't exist...
  }
  
  return MAPCACHE_FAILURE;
 
}

//------------------------------------------------------------------------------

/**
 * \brief write tile data to S3
 *
 * writes the content of mapcache_tile::data to disk.
 * \returns MAPCACHE_FAILURE if there is no data to write
 * \returns MAPCACHE_SUCCESS if the tile has been successfully written to S3
 * \private \memberof mapcache_cache_s3
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_s3_set(mapcache_context *ctx, mapcache_tile *tile)
{
  apr_size_t bytes;
  char errmsg[120];
  char *filename;
  mapcache_cache_s3* cache;
  
  cache = (mapcache_cache_s3*)tile->tileset->cache;
  
#ifdef DEBUG
  /* all this should be checked at a higher level */
  if(!tile->encoded_data && !tile->raw_image) {
    ctx->set_error(ctx,500,"attempting to write empty tile to disk");
    return;
  }
  if(!tile->encoded_data && !tile->tileset->format) {
    ctx->set_error(ctx,500,"received a raw tile image for a tileset with no format");
    return;
  }
#endif

  cache->tile_key(ctx, tile, &filename);
  GC_CHECK_ERROR(ctx);
  
  if(!tile->encoded_data) 
  {
    tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
    GC_CHECK_ERROR(ctx);
  }
  
  /* maxzoom is set, don't write to cache if zoom level is higher */
  if (cache->maxzoom>0)  
  {
    if (tile->z>cache->maxzoom)
    {
      return;
    }
  }
  
  S3BucketContext bucketContext;
  bucketContext.hostName = cache->host;
  bucketContext.bucketName = cache->bucket;
  bucketContext.protocol = S3ProtocolHTTP;
  bucketContext.uriStyle = S3UriStylePath;
  bucketContext.accessKeyId = cache->access_key;
  bucketContext.secretAccessKey = cache->secret_key;
  
  object_userdata gu;
    

  gu.buffer = (unsigned char*) tile->encoded_data->buf;
  gu.length = tile->encoded_data->size;
   
  SetS3(&bucketContext,filename, &gu);
}

//------------------------------------------------------------------------------

/**
 * \private \memberof mapcache_cache_s3
 */
static void _mapcache_cache_s3_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_s3 *dcache = (mapcache_cache_s3*)cache;
  char *layout = NULL;
  int template_layout = MAPCACHE_FALSE;
 
  layout = (char*)ezxml_attr(node,"layout");
  if (!layout || !strlen(layout) || !strcmp(layout,"tilecache")) 
  {
    dcache->tile_key = _mapcache_cache_s3_tilecache_tile_key;
  } 
  else if(!strcmp(layout,"arcgis")) 
  {
    dcache->tile_key = _mapcache_cache_s3_arcgis_tile_key;
  } 
  else if (!strcmp(layout,"template")) 
  {
    dcache->tile_key = _mapcache_cache_s3_template_tile_key;
    template_layout = MAPCACHE_TRUE;
    if ((cur_node = ezxml_child(node,"template")) != NULL) 
    {
      dcache->filename_template = apr_pstrdup(ctx->pool,cur_node->txt);
    } 
    else 
    {
      ctx->set_error(ctx, 400, "no template specified for cache \"%s\"", cache->name);
      return;
    }
  } 
  else 
  {
    ctx->set_error(ctx, 400, "unknown layout type %s for cache \"%s\"", layout, cache->name);
    return;
  }

  if (!template_layout && (cur_node = ezxml_child(node,"base")) != NULL) 
  {
    dcache->base_directory = apr_pstrdup(ctx->pool,cur_node->txt);
  }

  if ((cur_node = ezxml_child(node,"access_key")) != NULL) 
  {
    dcache->access_key = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"secret_key")) != NULL) 
  {
    dcache->secret_key = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"host")) != NULL) 
  {
    dcache->host = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"bucket")) != NULL) 
  {
    dcache->bucket = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  
  if ((cur_node = ezxml_child(node,"maxzoom")) != NULL) 
  {
    dcache->maxzoom = atoi(cur_node->txt);
  }
}

//------------------------------------------------------------------------------

/**
 * \private \memberof mapcache_cache_s3
 */
static void _mapcache_cache_s3_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache,
    mapcache_cfg *cfg)
{
  mapcache_cache_s3 *dcache = (mapcache_cache_s3*)cache;
  /* check all required parameters are configured */
  if((!dcache->base_directory || !strlen(dcache->base_directory)) &&
      (!dcache->filename_template || !strlen(dcache->filename_template))) 
  {
    ctx->set_error(ctx, 400, "s3 cache %s has no base directory or template",dcache->cache.name);
    return;
  }
  
  if (!dcache->access_key || !dcache->secret_key || !dcache->host || !dcache->bucket)
  {
    ctx->set_error(ctx, 400, "s3 cache %s has must set access key, secret key, host, and bucket name!",
                      dcache->cache.name);
    return;
  }
}
//------------------------------------------------------------------------------
/**
 * \brief creates and initializes a mapcache_disk_cache
 */
mapcache_cache* mapcache_cache_s3_create(mapcache_context *ctx)
{
  mapcache_cache_s3 *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_s3));
  if(!cache) 
  {
    ctx->set_error(ctx, 500, "failed to allocate s3 cache");
    return NULL;
  }

  cache->access_key = 0;
  cache->secret_key = 0;
  cache->host = 0;
  cache->bucket = 0;
  cache->maxzoom = 0;
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_S3;
  cache->cache.tile_delete = _mapcache_cache_s3_delete;
  cache->cache.tile_get = _mapcache_cache_s3_get;
  cache->cache.tile_exists = _mapcache_cache_s3_has_tile;
  cache->cache.tile_set = _mapcache_cache_s3_set;
  cache->cache.configuration_post_config = _mapcache_cache_s3_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_s3_configuration_parse_xml;
  return (mapcache_cache*)cache;
}

#endif  // USE_S3

/* vim: ts=2 sts=2 et sw=2
*/
