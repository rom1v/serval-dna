/*
 Copyright (C) 2012 Serval Project Inc
 
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
#include "overlay_buffer.h"
#include "overlay_packet.h"
#include "str.h"
#include "strbuf.h"

typedef struct overlay_txqueue {
  struct overlay_frame *first;
  struct overlay_frame *last;
  int length; /* # frames in queue */
  int maxLength; /* max # frames in queue before we consider ourselves congested */
  
  /* Latency target in ms for this traffic class.
   Frames older than the latency target will get dropped. */
  int latencyTarget;
} overlay_txqueue;

overlay_txqueue overlay_tx[OQ_MAX];

struct outgoing_packet{
  overlay_interface *interface;
  int i;
  struct subscriber *unicast_subscriber;
  int add_advertisements;
  struct sockaddr_in dest;
  int header_length;
  struct overlay_buffer *buffer;
  struct decode_context context;
};

struct sched_ent next_packet;
struct profile_total send_packet;

static void overlay_send_packet(struct sched_ent *alarm);
static int overlay_calc_queue_time(overlay_txqueue *queue, struct overlay_frame *frame);

int overlay_queue_init(){
  /* Set default congestion levels for queues */
  int i;
  for(i=0;i<OQ_MAX;i++) {
    overlay_tx[i].maxLength=100;
    overlay_tx[i].latencyTarget=1000; /* Keep packets in queue for 1 second by default */
  }
  /* expire voice/video call packets much sooner, as they just aren't any use if late */
  overlay_tx[OQ_ISOCHRONOUS_VOICE].maxLength=20;
  overlay_tx[OQ_ISOCHRONOUS_VOICE].latencyTarget=200;
  overlay_tx[OQ_ISOCHRONOUS_VIDEO].latencyTarget=200;
  return 0;  
}

/* remove and free a payload from the queue */
static struct overlay_frame *
overlay_queue_remove(overlay_txqueue *queue, struct overlay_frame *frame){
  struct overlay_frame *prev = frame->prev;
  struct overlay_frame *next = frame->next;
  if (prev)
    prev->next = next;
  else if(frame == queue->first)
    queue->first = next;
  
  if (next)
    next->prev = prev;
  else if(frame == queue->last)
    queue->last = prev;
  
  queue->length--;
  
  op_free(frame);
  
  return next;
}

#if 0 // unused
static int
overlay_queue_dump(overlay_txqueue *q)
{
  strbuf b = strbuf_alloca(8192);
  struct overlay_frame *f;
  strbuf_sprintf(b,"overlay_txqueue @ 0x%p\n",q);
  strbuf_sprintf(b,"  length=%d\n",q->length);
  strbuf_sprintf(b,"  maxLenght=%d\n",q->maxLength);
  strbuf_sprintf(b,"  latencyTarget=%d milli-seconds\n",q->latencyTarget);
  strbuf_sprintf(b,"  first=%p\n",q->first);
  f=q->first;
  while(f) {
    strbuf_sprintf(b,"    %p: ->next=%p, ->prev=%p\n",
		   f,f->next,f->prev);
    if (f==f->next) {
      strbuf_sprintf(b,"        LOOP!\n"); break;
    }
    f=f->next;
  }
  strbuf_sprintf(b,"  last=%p\n",q->last);
  f=q->last;
  while(f) {
    strbuf_sprintf(b,"    %p: ->next=%p, ->prev=%p\n",
		   f,f->next,f->prev);
    if (f==f->prev) {
      strbuf_sprintf(b,"        LOOP!\n"); break;
    }
    f=f->prev;
  }
  DEBUG(strbuf_str(b));
  return 0;
}
#endif

int overlay_payload_enqueue(struct overlay_frame *p)
{
  /* Add payload p to queue q.
   
   Queues get scanned from first to last, so we should append new entries
   on the end of the queue.
   
   Complain if there are too many frames in the queue.
   */
  
  if (!p) return WHY("Cannot queue NULL");
  
  do{
    if (p->destination_resolved)
      break;
    if (!p->destination)
      break;
    int r = subscriber_is_reachable(p->destination);
    if (r&REACHABLE)
      break;
    
    if (directory_service){
      r = subscriber_is_reachable(directory_service);
      if (r&REACHABLE)
	break;
    }
    
    return WHYF("Cannot send %x packet, destination %s is %s", p->type, 
		alloca_tohex_sid(p->destination->sid), r==REACHABLE_SELF?"myself":"unreachable");
  } while(0);
  
  if (p->queue>=OQ_MAX) 
    return WHY("Invalid queue specified");
  
  overlay_txqueue *queue = &overlay_tx[p->queue];
  
  if (config.debug.packettx)
    DEBUGF("Enqueuing packet for %s* (q[%d]length = %d)",
	   p->destination?alloca_tohex(p->destination->sid, 7): alloca_tohex(p->broadcast_id.id,BROADCAST_LEN),
	   p->queue, queue->length);
  
  if (p->payload && ob_remaining(p->payload)<0){
    // HACK, maybe should be done in each caller
    // set the size of the payload based on the position written
    ob_limitsize(p->payload,ob_position(p->payload));
  }
  
  if (queue->length>=queue->maxLength) 
    return WHYF("Queue #%d congested (size = %d)",p->queue,queue->maxLength);
  
  if (p->send_copies<=0)
    p->send_copies=1;
  else if(p->send_copies>5)
    return WHY("Too many copies requested");
  
  if (!p->destination_resolved){
    if (!p->destination){
      int i;
      int drop=1;
      
      // hook to allow for flooding via olsr
      olsr_send(p);
      
      // make sure there is an interface up that allows broadcasts
      for(i=0;i<OVERLAY_MAX_INTERFACES;i++){
	if (overlay_interfaces[i].state==INTERFACE_STATE_UP
	    && overlay_interfaces[i].send_broadcasts){
	  p->broadcast_sent_via[i]=0;
	  drop=0;
	}else
	  p->broadcast_sent_via[i]=1;
      }
      
      // just drop it now
      if (drop){
	WARN("No broadcast interfaces to send with");
	return -1;
      }
    }
  }
  
  struct overlay_frame *l=queue->last;
  if (l) l->next=p;
  p->prev=l;
  p->next=NULL;
  p->enqueued_at=gettime_ms();
  
  queue->last=p;
  if (!queue->first) queue->first=p;
  queue->length++;
  if (p->queue==OQ_ISOCHRONOUS_VOICE)
    rhizome_saw_voice_traffic();
  
  overlay_calc_queue_time(queue, p);
  
  return 0;
}

static void
overlay_init_packet(struct outgoing_packet *packet, struct subscriber *destination, int unicast,
		    overlay_interface *interface, struct sockaddr_in addr){
  packet->interface = interface;
  packet->i = (interface - overlay_interfaces);
  packet->dest=addr;
  packet->buffer=ob_new();
  packet->add_advertisements=1;
  if (unicast)
    packet->unicast_subscriber = destination;
  ob_limitsize(packet->buffer, packet->interface->mtu);
  
  overlay_packet_init_header(&packet->context, packet->buffer, destination, unicast, packet->i, 0);
  packet->header_length = ob_position(packet->buffer);
}

// update the alarm time and return 1 if changed
static int
overlay_calc_queue_time(overlay_txqueue *queue, struct overlay_frame *frame){
  do{
    if (frame->destination_resolved)
      break;
    if (!frame->destination)
      break;
    if (subscriber_is_reachable(frame->destination)&REACHABLE)
      break;
    if (directory_service){
      if (subscriber_is_reachable(directory_service)&REACHABLE)
	break;
    }
    // ignore payload alarm if the destination is currently unreachable
    return 0;
  }while(0);
  
  time_ms_t next_allowed_packet=0;
  if (frame->interface){
    next_allowed_packet = limit_next_allowed(&frame->interface->transfer_limit);
  }else{
    // check all interfaces
    int i;
    for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
    {
      if (overlay_interfaces[i].state!=INTERFACE_STATE_UP)
	continue;
      time_ms_t next_packet = limit_next_allowed(&overlay_interfaces[i].transfer_limit);
      if (next_allowed_packet==0||next_packet < next_allowed_packet)
	next_allowed_packet = next_packet;
    }
    if (next_allowed_packet==0)
      return 0;
  }
  
  if (next_packet.alarm==0 || next_allowed_packet < next_packet.alarm){
    if (!next_packet.function){
      next_packet.function=overlay_send_packet;
      send_packet.name="overlay_send_packet";
      next_packet.stats=&send_packet;
    }
    unschedule(&next_packet);
    next_packet.alarm=next_allowed_packet;
    // small grace period, we want to read incoming IO first
    next_packet.deadline=next_allowed_packet+15;
    schedule(&next_packet);
  }
  
  return 0;
}

static void
overlay_stuff_packet(struct outgoing_packet *packet, overlay_txqueue *queue, time_ms_t now){
  struct overlay_frame *frame = queue->first;
  
  // TODO stop when the packet is nearly full?
  while(frame){
    if (frame->enqueued_at + queue->latencyTarget < now){
      DEBUGF("Dropping frame type %x for %s due to expiry timeout", 
	     frame->type, frame->destination?alloca_tohex_sid(frame->destination->sid):"All");
      frame = overlay_queue_remove(queue, frame);
      continue;
    }
    
    /* Note, once we queue a broadcast packet we are committed to sending it out every interface, 
     even if we hear it from somewhere else in the mean time
     */
    
    // quickly skip payloads that have no chance of fitting
    if (packet->buffer && ob_limit(frame->payload) > ob_remaining(packet->buffer))
      goto skip;
    
    if (!frame->destination_resolved){
      frame->next_hop = frame->destination;
      
      if (frame->next_hop){
	// Where do we need to route this payload next?
	
	int r = subscriber_is_reachable(frame->next_hop);
	
	// first, should we try to bounce this payload off the directory service?
	if (r==REACHABLE_NONE && 
	    directory_service && 
	    frame->next_hop!=directory_service){
	  frame->next_hop=directory_service;
	  r=subscriber_is_reachable(directory_service);
	}
	
	// do we need to route via a neighbour?
	if (r&REACHABLE_INDIRECT){
	  frame->next_hop = frame->next_hop->next_hop;
	  r = subscriber_is_reachable(frame->next_hop);
	}
	
	if (!(r&REACHABLE_DIRECT)){
	  goto skip;
	}
	
	frame->interface = frame->next_hop->interface;
	
	if(r&REACHABLE_UNICAST){
	  frame->recvaddr = frame->next_hop->address;
	  frame->unicast = 1;
	  // ignore resend logic for unicast packets, where wifi gives better resilience
	  frame->send_copies=1;
	}else
	  frame->recvaddr = frame->interface->broadcast_address;
	
	frame->destination_resolved=1;
      }else{
	
	if (packet->buffer){
	  // check if we can stuff into this packet
	  if (frame->broadcast_sent_via[packet->i]){
	    goto skip;
	  }
	  frame->interface = packet->interface;
	  frame->recvaddr = packet->interface->broadcast_address;
	  
	}else{
	  // find an interface that we haven't broadcast on yet
	  frame->interface = NULL;
	  int i, keep=0;
	  for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
	  {
	    if (overlay_interfaces[i].state!=INTERFACE_STATE_UP || frame->broadcast_sent_via[i])
	      continue;
	    keep=1;
	    time_ms_t next_allowed = limit_next_allowed(&overlay_interfaces[i].transfer_limit);
	    if (next_allowed > now)
	      continue;
	    frame->interface = &overlay_interfaces[i];
	    frame->recvaddr = overlay_interfaces[i].broadcast_address;
	    break;
	  }
	  
	  if (!keep){
	    // huh, we don't need to send it anywhere?
	    frame = overlay_queue_remove(queue, frame);
	    continue;
	  }
	  
	  if (!frame->interface)
	    goto skip;
	}
      }
    }
    
    if (!packet->buffer){
      // can we send a packet on this interface now?
      if (limit_is_allowed(&frame->interface->transfer_limit))
	goto skip;
      
      if (frame->source_full)
	my_subscriber->send_full=1;
      overlay_init_packet(packet, frame->next_hop, frame->unicast, frame->interface, frame->recvaddr);
    }else{
      // is this packet going our way?
      if (frame->interface!=packet->interface || memcmp(&packet->dest, &frame->recvaddr, sizeof(packet->dest))!=0){
	goto skip;
      }
    }    
    
    if (config.debug.overlayframes){
      DEBUGF("Sending payload type %x len %d for %s via %s", frame->type, ob_position(frame->payload),
	     frame->destination?alloca_tohex_sid(frame->destination->sid):"All",
	     frame->next_hop?alloca_tohex_sid(frame->next_hop->sid):alloca_tohex(frame->broadcast_id.id, BROADCAST_LEN));
    }
    
    if (overlay_frame_append_payload(&packet->context, packet->interface, frame, packet->buffer)){
      // payload was not queued
      goto skip;
    }
    
    if (frame->destination)
      frame->destination->last_tx=now;
    if (frame->next_hop)
      frame->next_hop->last_tx=now;
    
    // don't send rhizome adverts if the packet contains a voice payload
    if (frame->queue==OQ_ISOCHRONOUS_VOICE)
      packet->add_advertisements=0;
    
    // mark the payload as sent
    int keep_payload = 0;
    
    if (frame->destination_resolved){
      frame->send_copies --;
      if (frame->send_copies>0)
	keep_payload=1;
    }else{
      int i;
      frame->broadcast_sent_via[packet->i]=1;
      
      // check if there is still a broadcast to be sent      
      for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
      {
	if (overlay_interfaces[i].state==INTERFACE_STATE_UP)
	  if (!frame->broadcast_sent_via[i]){
	    keep_payload=1;
	    break;
	  }
      }
    }
    
    if (!keep_payload){
      frame = overlay_queue_remove(queue, frame);
      continue;
    } 
    
  skip:
    // if we can't send the payload now, check when we should try next
    overlay_calc_queue_time(queue, frame);
    frame = frame->next;
  }
}

// fill a packet from our outgoing queues and send it
static int
overlay_fill_send_packet(struct outgoing_packet *packet, time_ms_t now) {
  int i;
  IN();
  // while we're looking at queues, work out when to schedule another packet
  unschedule(&next_packet);
  next_packet.alarm=0;
  next_packet.deadline=0;
  
  for (i=0;i<OQ_MAX;i++){
    overlay_txqueue *queue=&overlay_tx[i];
    
    overlay_stuff_packet(packet, queue, now);
  }
  
  if(packet->buffer){
    if (ob_position(packet->buffer) > packet->header_length){
    
      // stuff rhizome announcements at the last moment
      if (packet->add_advertisements)
	overlay_rhizome_add_advertisements(&packet->context, packet->i,packet->buffer);
      
      if (config.debug.packetconstruction)
	ob_dump(packet->buffer,"assembled packet");
      
      if (overlay_broadcast_ensemble(packet->i, &packet->dest, ob_ptr(packet->buffer), ob_position(packet->buffer))){
	// sendto failed. We probably don't have a valid route
	if (packet->unicast_subscriber){
	  set_reachable(packet->unicast_subscriber, REACHABLE_NONE);
	}
      }
    }else
      WARN("No payloads were sent?");
    ob_free(packet->buffer);
    RETURN(1);
  }
  RETURN(0);
}

// when the queue timer elapses, send a packet
static void overlay_send_packet(struct sched_ent *alarm){
  struct outgoing_packet packet;
  bzero(&packet, sizeof(struct outgoing_packet));
  
  overlay_fill_send_packet(&packet, gettime_ms());
}
