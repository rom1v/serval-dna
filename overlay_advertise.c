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
#include "overlay_address.h"
#include "overlay_buffer.h"
#include "overlay_packet.h"

/* List of prioritised advertisements */
#define OVERLAY_MAX_ADVERTISEMENT_REQUESTS 16
overlay_node *oad_requests[OVERLAY_MAX_ADVERTISEMENT_REQUESTS];
int oad_request_count=0;

/* Where we are up to in the node list for round-robin advertising */
int oad_bin=0;
int oad_slot=0;

/* Which round of the node list we are up to.
   This is used for reducing the advertisement rate for stable nodes.
   Initially this will just mean advertising higher-scoring nodes
   less often.

   Our goal is to advertise all nodes often enough to maintain connectivity,
   without wasting any packets.

   Basically high-scoring nodes can be advertised less often than low-scoring
   nodes.
   
   Let's advertise nodes <100 every round, <200 every 2 rounds, and >=200
   every 4th round.
*/
int oad_round=0;

/* Request that this node be advertised as a matter of priority */
int overlay_route_please_advertise(overlay_node *n)
{
  if (oad_request_count<OVERLAY_MAX_ADVERTISEMENT_REQUESTS)
    {
      oad_requests[oad_request_count++]=n;
      return 0;
    }
  else return 1;
}

struct subscriber *last_advertised=NULL;

int add_advertisement(struct subscriber *subscriber, void *context){
  struct overlay_buffer *e=context;
  
  if (subscriber->node){
    overlay_node *n=subscriber->node;
    
    ob_append_bytes(e,subscriber->sid,6);
    ob_append_byte(e,n->best_link_score);
    ob_append_byte(e,n->observations[n->best_observation].gateways_en_route);
    
    // stop if we run out of space
    if (ob_makespace(e,8)!=0){
      last_advertised=subscriber;
      return 1;
    }
    
    // or we've been called twice and looped around
    if (subscriber == last_advertised){
      last_advertised = NULL;
      return 1;
    }
  }
  
  return 0;
}

int overlay_route_add_advertisements(overlay_interface *interface, struct overlay_buffer *e)
{
  /* Construct a route advertisement frame and append it to e.
     
     Work out available space in packet for advertisments, and fit the 
     highest scoring nodes from the current portion in.
     
     Each advertisement consists of an address prefix followed by score.
     We will use 6 bytes of prefix to make it reasonably hard to generate
     collisions, including by birthday paradox (good for networks upto about
     20million nodes), and one byte each for score gateways_en_route.

     XXX - We need to send full addresses sometimes so that receiver can
     resolve them. Either that or we need to start supporting the PLEASEEXPLAIN
     packets, which is probably a better solution.

     The receiver will discount the score based on their measured reliability
     for packets to arrive from us; we just repeat what discounted score
     we have remembered.

     Hacking the frame together this way is less flexible, but much faster
     than messing about with malloc() and setting address fields.

     The src,dst and nexthop can each be encoded with a single byte.
     Thus using a fixed 1-byte RFS field we are limited to RFS<0xfa,
     which gives us 30 available advertisement slots per packet.
   */
  
  if (!my_subscriber)
    return WHY("Cannot advertise because I don't know who I am");
  
  ob_checkpoint(e);
  
  if (ob_append_byte(e,OF_TYPE_NODEANNOUNCE))
    return WHY("could not add node advertisement header");
  ob_append_byte(e,1); /* TTL */
  
  // assume we might fill the packet
  ob_append_rfs(e, e->sizeLimit - e->position);

  /* Add address fields */
  struct broadcast broadcast;
  overlay_broadcast_generate_address(&broadcast);
  overlay_broadcast_append(e,&broadcast);
  
  ob_append_byte(e,OA_CODE_PREVIOUS);
  
  overlay_address_append_self(interface,e);
  overlay_address_set_sender(my_subscriber);
  
  // TODO high priority advertisements first....
  /*
  while (slots>0&&oad_request_count) {
      oad_request_count--;
      ob_append_bytes(e,oad_requests[oad_request_count]->subscriber->sid,6);
      ob_append_byte(e,oad_requests[oad_request_count]->best_link_score);
      ob_append_byte(e,oad_requests[oad_request_count]
		     ->observations[oad_requests[oad_request_count]
				    ->best_observation].gateways_en_route);
      slots--;
      slots_used++;
    } 
*/
  struct subscriber *start = last_advertised;
  int start_pos = e->position;
  
  // append announcements starting from the last node we advertised
  enum_subscribers(start, add_advertisement, e);

  // if we didn't start at the beginning and still have space, start again from the beginning
  if (start && e->sizeLimit - e->position >=8){
    enum_subscribers(NULL, add_advertisement, e);
  }
  
  if (e->position == start_pos){
    // no advertisements? don't bother to send the payload at all.
    ob_rewind(e);
    overlay_address_clear();
  }else
    ob_patch_rfs(e,COMPUTE_RFS_LENGTH);

  return 0;
}

/* Pull out the advertisements and update our routing table accordingly.
   Because we are using a non-standard abbreviation scheme, we have to extract
   and search for the nodes ourselves.

   Also, we need to discount the scores based on the score of the sender.
   We can either do this once now (more computationally efficient), or have 
   a rather complicated scheme whereby we attempt to trace through the list
   of nodes from here to there.  That seems silly, and is agains't the BATMAN
   approach of each node just knowing single-hop information.
 */
int overlay_route_saw_advertisements(int i, struct overlay_frame *f, long long now)
{
  IN();
  while(f->payload->position < f->payload->sizeLimit)
    {
      struct subscriber *subscriber;
      unsigned char *sid = ob_get_bytes_ptr(f->payload, 6);
      int score=ob_get(f->payload);
      int gateways_en_route=ob_get(f->payload);

      // stop if hit end of payload
      if (!sid || score<0 || gateways_en_route<0)
	continue;
      
      subscriber = find_subscriber(sid, 6, 0);
      
      if (!subscriber){
	WARN("Dispatch PLEASEEXPLAIN not implemented");
	continue;
      }
      
      /* Don't record routes to ourselves */
      if (subscriber->reachable==REACHABLE_SELF) {
	if (debug & DEBUG_OVERLAYROUTING)
	  DEBUGF("Ignore announcement about me (%s)", alloca_tohex_sid(subscriber->sid));
	continue;
      }
      
      /* File it */
      overlay_route_record_link(now, subscriber->sid, f->source->sid,
				i,
				/* time range that this advertisement covers.
				   XXX - Make it up for now. */
				now-2500,now,
				score,gateways_en_route);
      
    }
  
  RETURN(0);;
}
