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
#include "rhizome.h"
#include <assert.h>
#include "overlay_buffer.h"
#include "overlay_address.h"
#include "overlay_packet.h"
#include <stdlib.h>

int rhizome_manifest_to_bar(rhizome_manifest *m,unsigned char *bar)
{
  IN();
  /* BAR = Bundle Advertisement Record.
     Basically a 32byte precis of a given manifest, that includes version, time-to-live
     and geographic bounding box information that is used to help manage flooding of
     bundles.

     64 bits - manifest ID prefix.
     56 bits - low 56 bits of version number.
     8 bits  - TTL of bundle in hops.
     64 bits - length of associated file.
     16 bits - min latitude (-90 - +90).
     16 bits - min longitude (-180 - +180).
     16 bits - max latitude (-90 - +90).
     16 bits - max longitude (-180 - +180).
 */

  if (!m) { RETURN(WHY("null manifest passed in")); }

  int i;

  /* Manifest prefix */
  for(i=0;i<8;i++) bar[i]=m->cryptoSignPublic[i];
  /* Version */
  for(i=0;i<7;i++) bar[8+6-i]=(m->version>>(8*i))&0xff;
  /* TTL */
  if (m->ttl>0) bar[15]=m->ttl-1; else bar[15]=0;
  /* file length */
  for(i=0;i<8;i++) bar[16+7-i]=(m->fileLength>>(8*i))&0xff;
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
  v=(minLat+90)*(65535/180); bar[24]=(v>>8)&0xff; bar[25]=(v>>0)&0xff;
  v=(minLong+180)*(65535/360); bar[26]=(v>>8)&0xff; bar[27]=(v>>0)&0xff;
  v=(maxLat+90)*(65535/180); bar[28]=(v>>8)&0xff; bar[29]=(v>>0)&0xff;
  v=(maxLong+180)*(65535/360); bar[30]=(v>>8)&0xff; bar[31]=(v>>0)&0xff;
  
  RETURN(0);
}

int bundles_available=-1;
int bundle_offset[2]={0,0};
int overlay_rhizome_add_advertisements(int interface_number, struct overlay_buffer *e)
{
  IN();

  /* behave differently during voice mode.
     Basically don't encourage people to grab stuff from us, but keep
     just enough activity going so that it is possible to send a (small)
     message/file during a call. 

     XXX Eventually only advertise small/recently changed files during voice calls.
     We need to change manifest table to include payload length to make our life
     easy here (also would let us order advertisements by size of payload).
     For now, we will just advertised only occassionally.
 */
  time_ms_t now = gettime_ms();
  if (now<rhizome_voice_timeout)
    RETURN(0);

  int pass;
  int bytes=e->sizeLimit-e->position;
  int overhead=1+11+1+2+2; /* maximum overhead */
  int slots=(bytes-overhead)/RHIZOME_BAR_BYTES;
  if (slots>30) slots=30;
  int bundles_advertised=0;

  if (slots<1) { RETURN(WHY("No room for node advertisements")); }

  if (!rhizome_db) { RETURN(WHY("Rhizome not enabled")); }

  if (ob_append_byte(e,OF_TYPE_RHIZOME_ADVERT))
    RETURN(WHY("could not add rhizome bundle advertisement header"));
  ob_append_byte(e, 1); /* TTL (1 byte) */

  ob_append_rfs(e,1+11+1+2+RHIZOME_BAR_BYTES/* RFS */);

  /* Stuff in dummy address fields (11 bytes) */
  struct broadcast broadcast_id;
  overlay_broadcast_generate_address(&broadcast_id);
  overlay_broadcast_append(e, &broadcast_id);
  ob_append_byte(e, OA_CODE_PREVIOUS);
  overlay_address_append_self(&overlay_interfaces[interface_number], e);

  /* Randomly choose whether to advertise manifests or BARs first. */
  int skipmanifests=random()&1;
  /* Version of rhizome advert block (1 byte):
     1 = manifests then BARs,
     2 = BARs only,
     3 = HTTP port then manifests then BARs,
     4 = HTTP port then BARs only
   */
  ob_append_byte(e,3+skipmanifests);
  /* Rhizome HTTP server port number (2 bytes) */
  ob_append_ui16(e, rhizome_http_server_port);

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

  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;

  /* Get number of bundles available if required */
  long long tmp = 0;
  if (sqlite_exec_int64_retry(&retry, &tmp, "SELECT COUNT(BAR) FROM MANIFESTS;") != 1)
    { RETURN(WHY("Could not count BARs for advertisement")); }
  bundles_available = (int) tmp;
  if (bundles_available==-1||(bundle_offset[0]>=bundles_available)) 
    bundle_offset[0]=0;
  if (bundles_available==-1||(bundle_offset[1]>=bundles_available)) 
    bundle_offset[1]=0;
  if(0)
    DEBUGF("%d bundles in database (%d %d), slots=%d.",bundles_available,
	   bundle_offset[0],bundle_offset[1],slots);
  
  sqlite3_stmt *statement=NULL;
  sqlite3_blob *blob=NULL;

  for(pass=skipmanifests;pass<2;pass++) {
    ob_checkpoint(e);
    switch(pass) {
    case 0: /* Full manifests */
      statement = sqlite_prepare("SELECT MANIFEST,ROWID FROM MANIFESTS LIMIT %d,%d", bundle_offset[pass], slots);
      break;
    case 1: /* BARs */
      statement = sqlite_prepare("SELECT BAR,ROWID FROM MANIFESTS LIMIT %d,%d", bundle_offset[pass], slots);
      break;
    }
    if (!statement)
      RETURN(WHY("Could not prepare sql statement for fetching BARs for advertisement"));
    while(  sqlite_step_retry(&retry, statement) == SQLITE_ROW
	&&  e->position+RHIZOME_BAR_BYTES<=e->sizeLimit
    ) {
      int column_type=sqlite3_column_type(statement, 0);
      switch(column_type) {
      case SQLITE_BLOB:
	if (blob)
	  sqlite3_blob_close(blob);
	blob = NULL;
	int ret;
	int64_t rowid = sqlite3_column_int64(statement, 1);
	do ret = sqlite3_blob_open(rhizome_db, "main", "manifests", pass?"bar":"manifest", rowid, 0 /* read only */, &blob);
	  while (sqlite_code_busy(ret) && sqlite_retry(&retry, "sqlite3_blob_open"));
	if (!sqlite_code_ok(ret)) {
	  WHYF("sqlite3_blob_open() failed, %s", sqlite3_errmsg(rhizome_db));
	  continue;
	}
	sqlite_retry_done(&retry, "sqlite3_blob_open");

	int blob_bytes=sqlite3_blob_bytes(blob);
	if (pass&&(blob_bytes!=RHIZOME_BAR_BYTES)) {
	  if (debug&DEBUG_RHIZOME)
	    DEBUG("Found a BAR that is the wrong size - ignoring");
	  sqlite3_blob_close(blob);
	  blob=NULL;
	  continue;
	}

	/* Only include manifests that are <=1KB inline.
	    Longer ones are only advertised by BAR */
	if (blob_bytes>1024) {
	  WARN("ignoring manifest > 1k");
	  sqlite3_blob_close(blob);
	  blob = NULL;
	  bundle_offset[pass]++;
	  continue;
	}

	int overhead=0;
	if (!pass) overhead=2;

	/* make sure there's enough room for the blob, its length,
	the 0xFF end marker and 1 spare for the rfs length to increase */
	if (ob_makespace(e,overhead+blob_bytes+2))
	  goto stopStuffing;
	  
	if (!pass) {
	      /* include manifest length field */
	      ob_append_ui16(e, blob_bytes);
	}
	    
	unsigned char *dest=ob_append_space(e, blob_bytes);
	if (!dest){
	  WHY("Reading blob will overflow overlay_buffer");
	  goto stopStuffing;
	}
	
	if (sqlite3_blob_read(blob,dest,blob_bytes,0) != SQLITE_OK) {
	  WHYF("sqlite3_blob_read() failed, %s", sqlite3_errmsg(rhizome_db));
	  goto stopStuffing;
	}

	bundles_advertised++;
	bundle_offset[pass]++;

	sqlite3_blob_close(blob);
	blob=NULL;
	
	ob_checkpoint(e);
      }
    }
  stopStuffing:
    if (blob)
      sqlite3_blob_close(blob);
    blob = NULL;
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

  ob_patch_rfs(e, COMPUTE_RFS_LENGTH);

  RETURN(0);
}

int overlay_rhizome_saw_advertisements(int i, struct overlay_frame *f, long long now)
{
  IN();
  if (!f) { RETURN(-1); }
  int ad_frame_type=ob_get(f->payload);
  struct sockaddr_in httpaddr = *(struct sockaddr_in *)f->recvaddr;
  httpaddr.sin_port = htons(RHIZOME_HTTP_PORT);
  int manifest_length;
  rhizome_manifest *m=NULL;
  char httpaddrtxt[INET_ADDRSTRLEN];
  
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
	  RETURN(0);
	}
	
	if (rhizome_read_manifest_file(m, (char *)data, manifest_length) == -1) {
	  WHY("Error importing manifest body");
	  rhizome_manifest_free(m);
	  RETURN(0);
	}
	
	char manifest_id_prefix[RHIZOME_MANIFEST_ID_STRLEN + 1];
	if (rhizome_manifest_get(m, "id", manifest_id_prefix, sizeof manifest_id_prefix) == NULL) {
	  WHY("Manifest does not contain 'id' field");
	  rhizome_manifest_free(m);
	  RETURN(0);
	}
	/* trim manifest ID to a prefix for ease of debugging 
	   (that is the only use of this */
	manifest_id_prefix[8]=0; 
	long long version = rhizome_manifest_get_ll(m, "version");
	if (debug & DEBUG_RHIZOME_RX) DEBUGF("manifest id=%s* version=%lld", manifest_id_prefix, version);

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
	  RETURN(0);
	}
	
	if (rhizome_ignore_manifest_check(m, &httpaddr))
	  {
	    /* Ignoring manifest that has caused us problems recently */
	    if (1) WARNF("Ignoring manifest with errors: %s*", manifest_id_prefix);
	  }
	else if (m&&(!m->errors))
	  {
	    /* Manifest is okay, so see if it is worth storing */
	    if (rhizome_manifest_version_cache_lookup(m)) {
	      /* We already have this version or newer */
	      if (debug & DEBUG_RHIZOME_RX) DEBUG("We already have that manifest or newer.");
	    } else {
	      if (debug & DEBUG_RHIZOME_RX) DEBUG("Not seen before.");
	      
	      rhizome_suggest_queue_manifest_import(m, &httpaddr);
	      // the above function will free the manifest structure, make sure we don't free it again
	      m=NULL;
	    }
	  }
	else
	  {
	    if (debug & DEBUG_RHIZOME) DEBUG("Unverified manifest has errors - so not processing any further.");
	    /* Don't waste any time on this manifest in future attempts for at least
	       a minute. */
	    rhizome_queue_ignore_manifest(m, &httpaddr, 60000);
	  }
	
	if (m)
	  rhizome_manifest_free(m);
	m=NULL;
      }
      break;
    }
  RETURN(0);
}
