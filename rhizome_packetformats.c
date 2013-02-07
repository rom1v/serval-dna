/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "serval.h"
#include "conf.h"
#include "rhizome.h"
#include <assert.h>
#include "overlay_buffer.h"
#include "overlay_address.h"
#include "overlay_packet.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Android doesn't have log2(), and we don't really need to do floating point
   math to work out how big a file is.
 */
int log2ll(unsigned long long x)
{
  unsigned char lookup[16]={0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4};
  int v=-1;
  if (x>0xffffffff) { v+=32; x=x>>32LL; }
  if (x>0xffff)     { v+=16; x=x>>16LL; }
  if (x>0xff)       { v+= 8; x=x>> 8LL; }
  if (x>0xf)        { v+= 4; x=x>> 4LL; }
  v+=lookup[x&0xf];
  return v;
}


int rhizome_manifest_to_bar(rhizome_manifest *m,unsigned char *bar)
{
  IN();
  /* BAR = Bundle Advertisement Record.
     Basically a 32byte precis of a given manifest, that includes version, time-to-live
     and geographic bounding box information that is used to help manage flooding of
     bundles.

     Old BAR format (no longer used):

     64 bits - manifest ID prefix.
     56 bits - low 56 bits of version number.
     8 bits  - TTL of bundle in hops.
     64 bits - length of associated file.
     16 bits - min latitude (-90 - +90).
     16 bits - min longitude (-180 - +180).
     16 bits - max latitude (-90 - +90).
     16 bits - max longitude (-180 - +180).

     New BAR format with longer manifest ID prefix:

     120 bits - manifest ID prefix.
     8 bits - log2(length) of associated file.
     56 bits - low 56 bits of version number.
     16 bits - min latitude (-90 - +90).
     16 bits - min longitude (-180 - +180).
     16 bits - max latitude (-90 - +90).
     16 bits - max longitude (-180 - +180).
     8 bits  - TTL of bundle in hops (0xff = unlimited distribution)

 */

  if (!m) { RETURN(WHY("null manifest passed in")); }

  int i;

  /* Manifest prefix */
  for(i=0;i<RHIZOME_BAR_PREFIX_BYTES;i++) 
    bar[RHIZOME_BAR_PREFIX_OFFSET+i]=m->cryptoSignPublic[i];
  /* file length */
  bar[RHIZOME_BAR_FILESIZE_OFFSET]=log2ll(m->fileLength);
  /* Version */
  for(i=0;i<7;i++) bar[RHIZOME_BAR_VERSION_OFFSET+6-i]=(m->version>>(8*i))&0xff;

  /* geo bounding box */
  double minLat=rhizome_manifest_get_double(m,"min_lat",-90);
  if (minLat<-90) minLat=-90; if (minLat>90) minLat=90;
  double minLong=rhizome_manifest_get_double(m,"min_long",-180);
  if (minLong<-180) minLong=-180; if (minLong>180) minLong=180;
  double maxLat=rhizome_manifest_get_double(m,"max_lat",+90);
  if (maxLat<-90) maxLat=-90; if (maxLat>90) maxLat=90;
  double maxLong=rhizome_manifest_get_double(m,"max_long",+180);
  if (maxLong<-180) maxLong=-180; if (maxLong>180) maxLong=180;  
  unsigned short v;
  int o=RHIZOME_BAR_GEOBOX_OFFSET;
  v=(minLat+90)*(65535/180); bar[o++]=(v>>8)&0xff; bar[o++]=(v>>0)&0xff;
  v=(minLong+180)*(65535/360); bar[o++]=(v>>8)&0xff; bar[o++]=(v>>0)&0xff;
  v=(maxLat+90)*(65535/180); bar[o++]=(v>>8)&0xff; bar[o++]=(v>>0)&0xff;
  v=(maxLong+180)*(65535/360); bar[o++]=(v>>8)&0xff; bar[o++]=(v>>0)&0xff;

  /* TTL */
  if (m->ttl>0) bar[RHIZOME_BAR_TTL_OFFSET]=m->ttl-1; 
  else bar[RHIZOME_BAR_TTL_OFFSET]=0;
  
  RETURN(0);
}

long long rhizome_bar_version(unsigned char *bar)
{
  long long version=0;
  int i;
  for(i=0;i<7;i++) version|=bar[RHIZOME_BAR_VERSION_OFFSET+6-i]<<(8LL*i);
  return version;
}

/* This function only displays the first 8 bytes, and should not be used
   for comparison. */
unsigned long long rhizome_bar_bidprefix_ll(unsigned char *bar)
{
  long long bidprefix=0;
  int i;
  for(i=0;i<8;i++) 
    bidprefix|=((unsigned long long)bar[RHIZOME_BAR_PREFIX_OFFSET+7-i])<<(8*i);
  return bidprefix;
}

struct advertisement_state{
  long long bundles_available;
  int64_t bundle_last_rowid[2];
};
struct advertisement_state advert_state[OVERLAY_MAX_INTERFACES];

int overlay_rhizome_add_advertisements(struct decode_context *context, int interface_number, struct overlay_buffer *e)
{
  IN();

  /* We need to change manifest table to include payload length to make our life
     easy here (also would let us order advertisements by size of payload).
     For now, we will just advertised only occassionally.

     XXX We will move all processing of Rhizome into a separate process
     so that the CPU delays caused by Rhizome verifying signatures isn't a problem.
 */
  if (!is_rhizome_advertise_enabled()) 
    RETURN(0);
  
  struct advertisement_state *state=&advert_state[interface_number];
  
  int pass;

  /* XXX Should add priority bundles here.
     XXX Should prioritise bundles for subscribed groups, Serval-authorised files
     etc over common bundles.
     XXX Should wait a while after going through bundle list so that we don't waste
     CPU on db queries if there are not many bundles.  Actually, we probably just
     shouldn't be sending bundles blindly on every tick.
     XXX How do we indicate group membership with BARs? Or do groups actively poll?

     XXX XXX XXX We should cache database results so that we don't waste all our time
     and energy asking the database much the same questions possibly many times per
     second.
  */

  // TODO Group handling not completely thought out here yet.

  int (*oldfunc)() = sqlite_set_tracefunc(is_debug_rhizome_ads);
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;

  /* Get number of bundles available if required */
  long long tmp = 0;
  if (sqlite_exec_int64_retry(&retry, &tmp, "SELECT COUNT(BAR) FROM MANIFESTS;") != 1) {
    sqlite_set_tracefunc(oldfunc);
    RETURN(WHY("Could not count BARs for advertisement"));
  }
  
  if (state->bundles_available!=tmp){
    state->bundle_last_rowid[0]=INT64_MAX;
    state->bundle_last_rowid[1]=INT64_MAX;
  }
  
  state->bundles_available = tmp;
  if (tmp<1)
    RETURN(0);
  
  /* Randomly choose whether to advertise manifests or BARs first. */
  int skipmanifests=random()&1;
  
  if (config.debug.rhizome_ads)
    DEBUGF("%lld bundles in database, starting from %lld, %lld.",state->bundles_available,
	   state->bundle_last_rowid[0], state->bundle_last_rowid[1]);
  
  sqlite3_stmt *statement=NULL;

  ob_checkpoint(e);
  
  if (overlay_frame_build_header(context, e, 
				 0, OF_TYPE_RHIZOME_ADVERT, 0, 1, 
				 NULL, NULL,
				 NULL, my_subscriber)){
    ob_rewind(e);
    RETURN(-1);
  }
  
  /* Version of rhizome advert block (1 byte):
   1 = manifests then BARs,
   2 = BARs only,
   3 = HTTP port then manifests then BARs,
   4 = HTTP port then BARs only
   */
  if (ob_append_byte(e,3+skipmanifests)){
    ob_rewind(e);
    RETURN(-1);
  }
  /* Rhizome HTTP server port number (2 bytes) */
  if (ob_append_ui16(e, rhizome_http_server_port)){
    ob_rewind(e);
    RETURN(-1);
  }
  
  for(pass=skipmanifests;pass<2;pass++) {
    ob_checkpoint(e);
    switch(pass) {
    case 0: /* Full manifests */
      statement = sqlite_prepare(&retry, "SELECT MANIFEST,ROWID,ID FROM MANIFESTS WHERE ROWID < %lld ORDER BY ROWID DESC LIMIT 30", 
				 state->bundle_last_rowid[pass]);
      break;
    case 1: /* BARs */
      statement = sqlite_prepare(&retry, "SELECT BAR,ROWID,ID FROM MANIFESTS WHERE ROWID < %lld ORDER BY ROWID DESC LIMIT 30", 
				 state->bundle_last_rowid[pass]);
      break;
    }
    if (!statement) {
      sqlite_set_tracefunc(oldfunc);
      WHY("Could not prepare sql statement for fetching BARs for advertisement");
      goto stopStuffing;
    }
    int count=0;
    
    while(sqlite_step_retry(&retry, statement) == SQLITE_ROW) {
      count++;
      if (sqlite3_column_type(statement, 0)!=SQLITE_BLOB)
	continue;
      
      const void *data = sqlite3_column_blob(statement, 0);
      int blob_bytes = sqlite3_column_bytes(statement, 0);
      int64_t rowid = sqlite3_column_int64(statement, 1);
      const unsigned char *manifestId = sqlite3_column_text(statement, 2);
      
      if (config.debug.rhizome_ads)
	DEBUGF("Considering manifest %s", manifestId);
      
      if (pass&&(blob_bytes!=RHIZOME_BAR_BYTES)) {
	if (config.debug.rhizome_ads)
	  DEBUG("Found a BAR that is the wrong size - ignoring");
	continue;
      }

      /* Only include manifests that are <=1KB inline.
	  Longer ones are only advertised by BAR */
      if (blob_bytes>1024) {
	WARN("ignoring manifest > 1k");
	continue;
      }

      int overhead=(!pass)?2:0;

      /* make sure there's enough room for the blob, its length,
      the 0xFF end marker and 1 spare for the rfs length to increase */
      if (ob_makespace(e,overhead+blob_bytes+1))
	goto stopStuffing;
	
      if (!pass) {
	/* include manifest length field */
	ob_append_ui16(e, blob_bytes);
      }
      
      if (ob_append_bytes(e, (unsigned char *)data, blob_bytes)){
	WHY("Failed to append data into buffer");
	goto stopStuffing;
      }

      state->bundle_last_rowid[pass]=rowid;

      ob_checkpoint(e);
    }
    
    if (count<30){
      // if we hit the end of the cursor, before the end of the packey buffer, restart next time at the beginning.
      state->bundle_last_rowid[pass]=INT64_MAX;
    }
    
  stopStuffing:
    if (statement)
      sqlite3_finalize(statement);
    statement = NULL;
      
    ob_rewind(e);
      
    if (!pass) {
      /* Mark end of whole manifests by writing 0xff, which is more than the MSB
	  of a manifest's length is allowed to be. */
      ob_append_byte(e,0xff);
    }
  }

  ob_patch_rfs(e);

  sqlite_set_tracefunc(oldfunc);
  RETURN(0);
}

int overlay_rhizome_saw_advertisements(int i, struct overlay_frame *f, long long now)
{
  IN();
  if (!f) { RETURN(-1); }
  
  if (!rhizome_db) { RETURN(0); }
  
  int ad_frame_type=ob_get(f->payload);
  struct sockaddr_in httpaddr = f->recvaddr;
  httpaddr.sin_port = htons(RHIZOME_HTTP_PORT);
  int manifest_length;
  rhizome_manifest *m=NULL;
  char httpaddrtxt[INET_ADDRSTRLEN];
  
  int (*oldfunc)() = sqlite_set_tracefunc(is_debug_rhizome_ads);

  switch (ad_frame_type) {
    case 3:
      /* The same as type=1, but includes the source HTTP port number */
      httpaddr.sin_port = htons(ob_get_ui16(f->payload));
      // FALL THROUGH ...
    case 1:
      /* Extract whole manifests */
      while(f->payload->position < f->payload->sizeLimit) {
	if (ob_getbyte(f->payload, f->payload->position)==0xff){
	  f->payload->position++;
	  break;
	}
	  
	manifest_length=ob_get_ui16(f->payload);
	if (manifest_length==0) continue;
	
	unsigned char *data = ob_get_bytes_ptr(f->payload, manifest_length);
	if (!data) {
	  assert(inet_ntop(AF_INET, &httpaddr.sin_addr, httpaddrtxt, sizeof(httpaddrtxt)) != NULL);
	  WHYF("Illegal manifest length field in rhizome advertisement frame %d vs %d.", 
	       manifest_length, f->payload->sizeLimit - f->payload->position);
	  break;
	}

	/* Read manifest without verifying signatures (which would waste lots of
	   energy, everytime we see a manifest that we already have).
	   In fact, it would be better here to do a really rough and ready parser
	   to get the id and version fields out, and avoid the memory copies that
	   otherwise happen. 
	   But we do need to make sure that at least one signature is there.
	*/	
	m = rhizome_new_manifest();
	if (!m) {
	  WHY("Out of manifests");
	  sqlite_set_tracefunc(oldfunc);
	  RETURN(0);
	}
	
	if (rhizome_read_manifest_file(m, (char *)data, manifest_length) == -1) {
	  WHY("Error importing manifest body");
	  rhizome_manifest_free(m);
	  sqlite_set_tracefunc(oldfunc);
	  RETURN(0);
	}
	
	char manifest_id_prefix[RHIZOME_MANIFEST_ID_STRLEN + 1];
	if (rhizome_manifest_get(m, "id", manifest_id_prefix, sizeof manifest_id_prefix) == NULL) {
	  WHY("Manifest does not contain 'id' field");
	  rhizome_manifest_free(m);
	  sqlite_set_tracefunc(oldfunc);
	  RETURN(0);
	}
	/* trim manifest ID to a prefix for ease of debugging 
	   (that is the only use of this */
	manifest_id_prefix[8]=0; 
	long long version = rhizome_manifest_get_ll(m, "version");
	if (config.debug.rhizome_ads)
	  DEBUGF("manifest id=%s* version=%lld", manifest_id_prefix, version);

	/* Crude signature presence test */
	for(i=m->manifest_all_bytes-1;i>0;i--)
	  if (!m->manifestdata[i]) {
	    /* A null in the middle says we have a signature */
	    break;
	  }
	if (!i) {
	  /* ignore the announcement, but don't ignore other people
	     offering the same manifest */
	  WARN("Ignoring manifest announcment with no signature");
	  rhizome_manifest_free(m);
	  sqlite_set_tracefunc(oldfunc);
	  RETURN(0);
	}
	
	if (rhizome_ignore_manifest_check(m, &httpaddr,f->source->sid))
	  {
	    /* Ignoring manifest that has caused us problems recently */
	    if (1) WARNF("Ignoring manifest with errors: %s*", manifest_id_prefix);
	  }
	else if (m->errors == 0)
	  {
	    /* Manifest is okay, so see if it is worth storing */
	    if (rhizome_manifest_version_cache_lookup(m)) {
	      /* We already have this version or newer */
	      if (config.debug.rhizome_ads)
		DEBUG("We already have that manifest or newer.");
	    } else {
	      if (config.debug.rhizome_ads)
		DEBUG("Not seen before.");
	      rhizome_suggest_queue_manifest_import(m, &httpaddr,f->source->sid);
	      // the above function will free the manifest structure, make sure we don't free it again
	      m=NULL;
	    }
	  }
	else
	  {
	    if (config.debug.rhizome_ads)
	      DEBUG("Unverified manifest has errors - so not processing any further.");
	    /* Don't waste any time on this manifest in future attempts for at least
	       a minute. */
	    rhizome_queue_ignore_manifest(m, &httpaddr,f->source->sid, 60000);
	  }
	if (m) {
	  rhizome_manifest_free(m);
	  m = NULL;
	}
      }
      break;
    }
  sqlite_set_tracefunc(oldfunc);
  RETURN(0);
}
