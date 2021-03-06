/* Copyright (C) 2005-2011 M. T. Homer Reid
 *
 * This file is part of BUFF-EM.
 *
 * BUFF-EM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * BUFF-EM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * FIBBICache.cc -- caching of (F)requency-(I)ndependent 
 *               -- (B)asis function -- (B)asis function (I)ntegrals
 * 
 * homer reid    -- 4/2015
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <tr1/unordered_map>
#include <pthread.h> // needed for rwlock
#include <omp.h> // needed for rwlock

#include <libhrutil.h>

#include "libbuff.h"

namespace buff {

#define KEYLEN 27
#define KEYSIZE (KEYLEN*sizeof(float))

#define DATALEN FIBBIDATALEN
#define DATASIZE (DATALEN*sizeof(double))

/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
/*- note: i found this on wikipedia ... ------------------------*/
/*--------------------------------------------------------------*/
static long JenkinsHash(const char *key, size_t len)
{
    long hash;
    unsigned int i;
    for(hash = i = 0; i < len; ++i)
    {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

long HashFunction(const float *Key)
{ return JenkinsHash( (const char *)Key, KEYSIZE );
} 

/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
typedef struct { float  Key[KEYLEN];    } KeyStruct;
typedef struct { double Data[DATALEN];  } DataStruct;

typedef std::pair<KeyStruct, DataStruct> KDPair;

struct KeyHash
 {
   long operator() (const KeyStruct &K) const 
    { return HashFunction(K.Key); }
 };

typedef struct 
 { 
   bool operator()(const KeyStruct &K1, const KeyStruct &K2) const
    { 
      return !memcmp( (const void *)K1.Key,
                      (const void *)K2.Key,
                      KEYSIZE
                    );
    };

 } KeyCmp;

typedef std::tr1::unordered_map< KeyStruct,
                                 DataStruct,
                                 KeyHash,
                                 KeyCmp> KDMap;

/*--------------------------------------------------------------*/
/*- class constructor                                          -*/
/*--------------------------------------------------------------*/
FIBBICache::FIBBICache(char *MeshFileName)
{
  KDMap *KDM=new KDMap;
  opTable = (void *)KDM;

  pthread_rwlock_init(&lock,0);

  Hits=Misses=0;

  /*--------------------------------------------------------------*/
  /*- attempt to preload cache                                   -*/
  /*--------------------------------------------------------------*/
  PreloadFileName=0;
  RecordsPreloaded=0;
  RecordsWritten=0;
  if (MeshFileName)
   { 
     char CacheFileName[MAXSTR];
     int Status=1;

     // first look for ${BUFF_CACHE_PATH}/MeshFile.cache 
     char *CacheDir=getenv("BUFF_CACHE_PATH");
     if (CacheDir)
      { snprintf(CacheFileName, MAXSTR, "%s/%s.cache",
                 CacheDir,GetFileBase(MeshFileName));
        Status=PreLoad(CacheFileName);
      };

     // next look for ./MeshFile.cache
     if (Status!=0)
      { snprintf(CacheFileName, MAXSTR, "%s.cache",
                 GetFileBase(MeshFileName));
        Status=PreLoad(CacheFileName);
      };
   };
}

/*--------------------------------------------------------------*/
/*- class destructor  ------------------------------------------*/
/*--------------------------------------------------------------*/
FIBBICache::~FIBBICache()
{
  pthread_rwlock_destroy(&lock);

  if (PreloadFileName)
   free(PreloadFileName);

  KDMap *KDM = (KDMap *)opTable;
  delete KDM;

} 

/***************************************************************/
/* the following three routines implement a technique for      */
/* assigning a search key to pairs of SWG basis functions in   */
/* a translation-independent (but not rotation-indepedent) way */
/***************************************************************/
static void inline VecSubFloat(double *V1, double *V2, float *V1mV2)
{ V1mV2[0] = (float)(V1[0] - V2[0]);
  V1mV2[1] = (float)(V1[1] - V2[1]);
  V1mV2[2] = (float)(V1[2] - V2[2]);
}

void GetFIBBICacheKey(SWGVolume *OA, int nfA,
                      SWGVolume *OB, int nfB,
                      float *K)
{

  double  *VA = OA->Vertices;
  SWGFace *FA = OA->Faces[nfA];
  double *QPA = VA + 3*(FA->iQP);
  double *QMA = VA + 3*(FA->iQM);
  double *V1A = VA + 3*(FA->iV1);
  double *V2A = VA + 3*(FA->iV2);
  double *V3A = VA + 3*(FA->iV3);

  double  *VB = OB->Vertices;
  SWGFace *FB = OB->Faces[nfB];
  double *QPB = VB + 3*(FB->iQP);
  double *QMB = VB + 3*(FB->iQM);
  double *V1B = VB + 3*(FB->iV1);
  double *V2B = VB + 3*(FB->iV2);
  double *V3B = VB + 3*(FB->iV3);

  VecSubFloat(QMA, QPA, K + 0*3);
  VecSubFloat(V1A, QPA, K + 1*3);
  VecSubFloat(V2A, QPA, K + 2*3);
  VecSubFloat(V3A, QPA, K + 3*3);
  VecSubFloat(QPB, QPA, K + 4*3);
  VecSubFloat(QMB, QPA, K + 5*3);
  VecSubFloat(V1B, QPA, K + 6*3);
  VecSubFloat(V2B, QPA, K + 7*3);
  VecSubFloat(V3B, QPA, K + 8*3);

}

/*--------------------------------------------------------------*/
/*- routine for fetching a FIBBI data record from a FIBBICache: */
/*- we look through our table to see if we have a record for    */
/*- this panel pair, we return it if we do, and otherwise we    */
/*- compute a new FIBBI data record for this panel pair and     */
/*- add it to the table.                                        */
/*--------------------------------------------------------------*/
void FIBBICache::GetFIBBIData(SWGVolume *OA, int nfA,
                              SWGVolume *OB, int nfB,
                              double *Data)
{
  /***************************************************************/
  /* look for this key in the cache ******************************/
  /***************************************************************/
  KeyStruct Key;
  GetFIBBICacheKey(OA, nfA, OB, nfB, Key.Key);

  KDMap *KDM    = (KDMap *)opTable;
  bool Found;
  pthread_rwlock_rdlock(&lock);
  KDMap::iterator p=KDM->find(Key);
  Found = (p != (KDM->end()) );
  if (Found) memcpy(Data, p->second.Data, DATASIZE);
  pthread_rwlock_unlock(&lock);

  if ( Found )
   { Hits++;
     return;
   };
  
  /***************************************************************/
  /* if it was not found, compute a new FIBBI data record and add*/
  /* it to the cache                                             */
  /***************************************************************/
  Misses++;
  ComputeFIBBIData(OA, nfA, OB, nfB, Data);
  DataStruct DS;
  memcpy(DS.Data, Data, DATASIZE);
  pthread_rwlock_wrlock(&lock);
  KDM->insert( KDPair(Key,DS) );
  pthread_rwlock_unlock(&lock);
}

/***************************************************************/
/* this routine and the following routine implement a mechanism*/
/* for storing the contents of a FIBBI cache to a binary file, */
/* and subsequently pre-loading a FIBBI cache with the content */
/* of a file created by this storage operation.                */
/*                                                             */
/* cache files have the canonical file name MeshFile.cache     */
/* where MeshFile.vmsh is the tetrahedral mesh file.           */
/* cache files are written to the directory ${BUFF_CACHE_PATH} */
/* if that environment variable is defined, and otherwise to   */
/* the current working directory.                              */
/*                                                             */
/* the file format is pretty simple (and non-portable w.r.t.   */
/* endianness):                                                */
/*  bytes 0--12:   'FIBBI_GCACHE' + 0                          */
/*  next xx bytes:  first record                               */ 
/*  next xx bytes:  second record                              */
/*  ...             ...                                        */
/*                                                             */
/* where xx is the size of the record; each record consists of */
/* a search key (KEYLEN float values) followed by the content  */
/* of the FIBBI data record for that search key.               */
/*                                                             */
/* note: FIBBICF = 'FIBBI cache file'                          */
/***************************************************************/
const char FIBBICF_GSignature[]  = "FIBBI_GCACHE";
#define FIBBICF_SIGSIZE (sizeof(FIBBICF_GSignature))

void FIBBICache::Store(const char *MeshFileName)
{
  if (!MeshFileName)
   return;
  char MFNCopy[MAXSTR];
  strncpy(MFNCopy,MeshFileName,MAXSTR);

  char FileName[MAXSTR];
  
  char *s=getenv("BUFF_CACHE_PATH");
  if (!s)
   snprintf(FileName,MAXSTR,"%s.cache",GetFileBase(MFNCopy));
  else
   snprintf(FileName,MAXSTR,"%s/%s.cache",s,GetFileBase(MFNCopy));

  KDMap *KDM = (KDMap *)opTable;

  /*--------------------------------------------------------------*/
  /*- pause to check if the following conditions are satisfied:  -*/
  /*-  (1) the FIBBI cache was preloaded from an input file whose-*/
  /*-      name matches the name of the output file to which we  -*/
  /*-      are being asked to dump the cache                     -*/
  /*-  (2) the number of cache records hasn't changed since we   -*/
  /*-      preloaded from the input file.                        -*/
  /*- if both conditions are satisfied, we don't bother to dump  -*/
  /*- the cache since the operation would result in a cache dump -*/
  /*- file identical to the one that already exists.             -*/
  /*--------------------------------------------------------------*/
  unsigned int NumRecords = KDM->size();
  if (NumRecords==RecordsWritten)
   { Log("FC::S FIBBI cache unchanged since last written to disk (skipping cache dump");
     return;
   };

  if (     PreloadFileName
       && !strcmp(PreloadFileName, FileName)
       && NumRecords==RecordsPreloaded
     )
   { Log("FC::S FIBBI cache unchanged since reading from %s (skipping cache dump)",FileName);
     return;
   };

  /*--------------------------------------------------------------*/
  /*- i assume that Preload() and Store() won't be called from    */
  /*- multithreaded code sections.                                */
  /*--------------------------------------------------------------*/
  //FCLock.read_lock();

  FILE *f=fopen(FileName,"w");
  if (!f)
   { Log("FC::S warning: could not open file %s (aborting cache dump)...",FileName);
     return;
   };
  Log("FC::S Writing FIBBI cache to file %s...",FileName);

  /*---------------------------------------------------------------------*/
  /*- write file signature ----------------------------------------------*/
  /*---------------------------------------------------------------------*/
  fwrite(FIBBICF_GSignature, FIBBICF_SIGSIZE,1,f);

  /*---------------------------------------------------------------------*/
  /*- iterate through the table and write entries to the file one-by-one */
  /*---------------------------------------------------------------------*/
  RecordsWritten=0;
  KDMap::iterator it;
  for ( it = KDM->begin(); it != KDM->end(); it++ )
   { 
     const float *Key   = it->first.Key;
     double *Data = it->second.Data;
     if (    (1 != fwrite(Key,  KEYSIZE,  1, f )) 
          || (1 != fwrite(Data, DATASIZE, 1, f ))
        ) break;
     RecordsWritten++;
   };

  /*---------------------------------------------------------------------*/
  /*- and that's it -----------------------------------------------------*/
  /*---------------------------------------------------------------------*/
  fclose(f);
  Log("FC::S ...wrote %i/%i FIBBI records.",RecordsWritten,NumRecords);

  //FCLock.read_unlock();
}

// return 0 on success, nonzero on failure
int FIBBICache::PreLoad(const char *FileName)
{
  
  /*--------------------------------------------------------------*/
  /*- try to open the file ---------------------------------------*/
  /*--------------------------------------------------------------*/
  FILE *f=fopen(FileName,"r");
  if (!f)
   { Log("FC::P could not open file %s (skipping cache preload)",FileName);
     return 1;
   };

  KDMap *KDM       = (KDMap *)opTable;
  int RecordSize   = KEYSIZE + DATASIZE;
  int NumRecords   = 0; 
  int RecordsRead  = 0;
  unsigned long M0 = GetMemoryUsage();

  /*--------------------------------------------------------------*/
  /*- run through some sanity checks to make sure we have a valid */
  /*- cache file                                                  */
  /*--------------------------------------------------------------*/
  const char *ErrMsg=0;
  struct stat fileStats;
  off_t FileSize;
  if ( fstat(fileno(f), &fileStats) )
   { ErrMsg="invalid cache file";
     goto fail;
   };
  
  // check that the file signature is present and has the right size 
  FileSize=fileStats.st_size;
  char FileSignature[FIBBICF_SIGSIZE]; 
  if ( (FileSize < ((signed int )FIBBICF_SIGSIZE)) )
   { ErrMsg="invalid cache file";
     goto fail;
   };
  if ( 1!=fread(FileSignature, FIBBICF_SIGSIZE, 1, f) )
   { ErrMsg="invalid cache file";
     goto fail;
   };

  // check that the file signature is one that we recognize
  if ( strcmp(FileSignature, FIBBICF_GSignature)  )
   { ErrMsg="invalid cache file";
     goto fail;
   };

  // the file size, minus the portion taken up by the signature,
  // should be an integer multiple of the size of a (Key,Data) pair
  FileSize-=FIBBICF_SIGSIZE;
  if ( (FileSize % RecordSize)!=0 )
   { ErrMsg="cache file has incorrect size";
     goto fail;
   };

  /*--------------------------------------------------------------*/
  /*- now just read records from the file one at a time and add   */
  /*- them to the table.                                          */
  /*--------------------------------------------------------------*/
  Log("FC::P Preloading FIBBI records from file %s...",FileName);
  NumRecords = FileSize / RecordSize;
  for(int nr=0; nr<NumRecords; nr++)
   { 
     KeyStruct Key;
     DataStruct Data; 
     
     if (    (fread( Key.Key,    KEYSIZE,  1, f) != 1)
          || (fread( Data.Data,  DATASIZE, 1, f) != 1)
        )
      { Log("FC::P file %s: read only %i/%i records",FileName,RecordsRead,NumRecords);
        fclose(f);
	return 1;
      };
  
     KDM->insert( KDPair(Key,Data) );
     RecordsRead++;
   };

  /*--------------------------------------------------------------*/
  /* the most recent file from which we preloaded, and the number */
  /* of records preloaded, are stored within the class body to    */
  /* allow us to skip dumping the cache back to disk in cases     */
  /* where that would amount to just rewriting the same cache     */
  /*--------------------------------------------------------------*/
  if (PreloadFileName)
   free(PreloadFileName);
  PreloadFileName=strdupEC(FileName);
  RecordsPreloaded=RecordsRead;

  Log("FC::P ...successfully preloaded %i FIBBI records.",RecordsPreloaded);
#define ONEMEG (1<<20)
  Log("FC::P Cache memory use: %i MB.",(GetMemoryUsage() - M0)/(ONEMEG));
  fclose(f);
  return 0;

  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
 fail:
  Log("FC::P warning: file %s: %s (skipping cache preload)",FileName,ErrMsg);
  fclose(f);
  return 1;
}

/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
int FIBBICache::Size()
{ 
  if (opTable==0) return -1;
  KDMap *KDM = (KDMap *)opTable;
  return KDM->size();

}

} // namespace buff
