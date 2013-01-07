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
#include "str.h"
#include "strbuf.h"
#include "overlay_buffer.h"
#include "overlay_address.h"
#include "overlay_packet.h"

/*
  Here we implement the actual routing algorithm which is heavily based on BATMAN.
  
  The fundamental difference is that we want to allow the mesh to grow beyond the
  size that could ordinarily be accomodated by the available bandwidth.  Some
  explanation follows.

  BATMAN operates by having nodes periodically send "hello" or originator messages,
  either with a limited distribution or with a sufficiently high TTL to spread
  over the whole network.  

  The latter results in a super-linear bandwidth requirement as the network grows 
  in size.

  What we wish to do is to implement the BATMAN concept, but using link-local traffic
  only.  To do this we need to change the high-TTL originator frames into something
  equivalent, but that does not get automatic network-wide distribution.

  What seems possible is to implement the BATMAN approach for link-local neighbours,
  and then have each node periodically announce the link-score to the peers that
  they know about, whether link-local or more distant.  If the number of reported 
  peers is left unconstrained, super-linear bandwidth consumption will still occur.

  However, if the number of peers that each node announces is limited, then bandwidth
  will be capped at a constant factor (which can be chosen based on the bandwidth
  available). The trade-off being that each node will only be able to see some number
  of "nearest" peers based on the available bandwidth.  

  This seems an entirely reasonable outcome, and at least on the surface would appear
  to solve our problem of wanting to allow a global-scale mesh, even if only local
  connectivity is possible, in contrast to existing mesh protocols that will not allow
  any connectivity once the number of nodes grows beyond a certain point.

  Remaining challenges that we have to think through are how to add a hierarchical
  element to the mesh that might allow us to route traffic beyond a nodes' 
  neighbourhood of peers.

  There is some hope to extend the effective range beyond the immediate neighbourhood
  to some degree by rotating the peers that a node reports on, so that a larger total
  set of nodes becomes known to the mesh, in return for less frequent updates on their
  link scores and optimal routes.

  This actually makes some logical sense, as the general direction in which to route 
  a frame to a distant node is less likely to change more slowly than for nearer nodes.
  So we will attempt this.

  With some careful thought, this statistical announcement of peers also serves to allow
  long-range but very low bandwidth links, e.g., satellite or dial-up, as well as long-shot
  WiFi where bandwidth is less constrained.

  Questions arise as to the possibility of introducing routing loops through the use of
  stale information.  So we will certainly need to have some idea of the freshness of 
  routing data.

  Finally, all this works only for bidirectional links.  We will need to think about how
  to handle mono-directional links.  BATMAN does this well, but I don't have the documentation
  here at 36,000 feet to digest it and think about how to incorporate it.

  Having landed and thought about this a bit more, what we will do is send link-local
  announcements which each direct neighbour Y will listen to and build up an estimated
  probability of a packet sent by X reaching them.  This information will be 
  periodically broadcast as the interface ticks, and not forwarded beyond link-local,
  this preventing super-scalar traffic growth.  When X hears that Y's P(X,Y) from 
  such a neighbour reception notice X can record P(X,Y) as its link score to Y. This
  deals with asymmetric delivery probabilities for link-local neighbours.

  So how do we efficiently distribute P(X,Y) to our second-degree neighbours, which 
  we shall call Z? We will assume that P(X,Z) = P(X,Y)*P(Y,Z).  Thus X needs to get
  Y's set of P(Y,a) values. This is easy to arrange if X and Y are bidirectionally
  link-local, as Y can periodically broadcast this information, and X can cache it.
  This process will eventually build up the entire set P(X,b), where b are all nodes
  on the mesh. However, it assumes that every link is bidirectional.  What if X can
  send directly to Y, but Y cannot send directly to X, i.e., P(X,Y)~1, P(Y,X)~0?
  Provided that there is some path P(Y,m)*P(m,X) >0, then Y will eventually learn 
  about it.  If Y knows that P(X,Y)>0, then it knows that X is a link-local neighbour
  monodirectionally, and thus should endeavour to tell X about its direct neighbours.
  This is fairly easy to arrange, and we will try this approach.

  So overall, this results in traffic at each node which is O(n^2+n*m) where n is the
  number of direct neighbours and m is the total number of nodes reachable on the 
  mesh.  As we can limit the number of nodes reachable on the mesh by having nodes
  only advertise their k highest scoring nodes, we can effectively limit the traffic
  to approximately linear with respect to reachable node count, but quadratic with
  respect to the number of immediate neighbours.  This seems a reasonable outcome.

  Related to this we need to continue thinking about how to handle intermittant links in a more
  formal sense, including getting an idea of when nodes might reappear.

  Turning to the practical side of things, we need to keep track of reachability scores for
  nodes via each of our immediate neighbours.  Recognising the statistical nature of 
  the announcments, we probably want to keep track of some that have ceased to be neighbours
  in case they become neighbours again.

  Probably it makes more sense to have a list of known nodes and the most recent and
  highest scoring nodes by which we may reach them, complete with the sequence numbers of last
  observation that they are based upon, and possibly more information down the track to
  support intermittant links.

*/


struct overlay_neighbour_observation {
  /* Sequence numbers are handled as ranges because the tick
   rate can vary between interfaces, and we want to be able to
   estimate the reliability of links to nodes that may have
   several available interfaces.
   We don't want sequence numbers to wrap too often, but we
   would also like to support fairly fast ticking interfaces,
   e.g., for gigabit type links. So lets go with 1ms granularity. */
  unsigned int s1;
  unsigned int s2;
  time_ms_t time_ms;
  unsigned char sender_interface;
  unsigned char valid;
};

struct overlay_neighbour {
  time_ms_t last_observation_time_ms;
  time_ms_t last_metric_update;
  int most_recent_observation_id;
  struct overlay_neighbour_observation observations[OVERLAY_MAX_OBSERVATIONS];
  overlay_node *node;
  
  /* Scores of visibility from each of the neighbours interfaces.
   This is so that the sender knows which interface to use to reach us.
   */
  unsigned char scores[OVERLAY_MAX_INTERFACES];
};

/* We need to keep track of which nodes are our direct neighbours.
   This means we need to keep an eye on how recently we received DIRECT announcements
   from nodes, and keep a list of the most recent ones.  The challenge is to keep the
   list ordered without having to do copies or have nasty linked-list structures that
   require lots of random memory reads to resolve.

   The simplest approach is to maintain a cache of neighbours and practise random
   replacement.  It is however succecptible to cache flushing attacks by adversaries, so
   we will need something smarter in the long term.
*/
#define overlay_max_neighbours 128
int overlay_neighbour_count=0;
struct overlay_neighbour overlay_neighbours[overlay_max_neighbours];

int overlay_route_recalc_node_metrics(overlay_node *n, time_ms_t now);
int overlay_route_recalc_neighbour_metrics(struct overlay_neighbour *n, time_ms_t now);
struct overlay_neighbour *overlay_route_get_neighbour_structure(overlay_node *node, int createP);


overlay_node *get_node(struct subscriber *subscriber, int create){
  if (!subscriber)
    return NULL;
  
  // we don't want to track routing info for ourselves.
  if (subscriber->reachable==REACHABLE_SELF)
    return NULL;
  
  if ((!subscriber->node) && create){
    subscriber->node = (overlay_node *)malloc(sizeof(overlay_node));
    memset(subscriber->node,0,sizeof(overlay_node));
    subscriber->node->subscriber = subscriber;
    // if we're taking over routing calculations, make sure we invalidate any other calculations first
    set_reachable(subscriber, REACHABLE_NONE);
    // This info message is used by tests; don't alter or remove it.
    INFOF("ADD OVERLAY NODE sid=%s", alloca_tohex_sid(subscriber->sid));
  }
  
  return subscriber->node;
}

int overlay_route_ack_selfannounce(overlay_interface *recv_interface,
				   unsigned int s1,unsigned int s2,
				   int interface,
				   struct subscriber *subscriber)
{
  /* Acknowledge the receipt of a self-announcement of an immediate neighbour.
     We could acknowledge immediately, but that requires the transmission of an
     extra packet with all the overhead that entails.  However, there is no real
     need to send the ack out immediately.  It should be entirely reasonable to 
     send the ack out with the next interface tick. 

     So we can craft the ack and submit it to the queue. As the next-hop will get
     determined at TX time, this will ensure that we send the packet out on the 
     right interface to reach the originator of the self-assessment.

     So all we need to do is craft the payload and put it onto the queue for 
     OVERLAY_MESH_MANAGEMENT messages.

     Also, we should check for older such frames on the queue and drop them.

     There is one caveat to the above:  until the first selfannounce gets returned,
     we don't have an open route.  Thus we need to just make sure that the ack
     goes out broadcast if we don't know about a return path. Once the return path
     starts getting built, it should be fine.

   */

  /* XXX Allocate overlay_frame structure and populate it */
  struct overlay_frame *out=NULL;
  out=calloc(sizeof(struct overlay_frame),1);
  if (!out) return WHY("calloc() failed to allocate an overlay frame");

  out->type=OF_TYPE_SELFANNOUNCE_ACK;
  out->modifiers=0;
  out->ttl=6; /* maximum time to live for an ack taking an indirect route back
		 to the originator.  If it were 1, then we would not be able to
		 handle mono-directional links (which WiFi is notorious for).
	         XXX 6 is quite an arbitrary selection however. */

  /* Set destination of ack to source of observed frame */
  out->destination = subscriber;
  /* set source to ourselves */
  out->source = my_subscriber;

  /* Set the time in the ack. Use the last sequence number we have seen
     from this neighbour, as that may be helpful information for that neighbour
     down the track.  My policy is to communicate that information which should
     be helpful for forming and maintaining the health of the mesh, as that way
     each node can in potentially implement a different mesh routing protocol,
     without breaking the wire protocol.  This makes over-the-air software updates
     much safer.

     Combining of adjacent observation reports may mean that the most recent
     observation is not the last one in the list, also the wrapping of the sequence
     numbers means we can't just take the highest-numbered sequence number.  
     So we need to take the observation which was most recently received.
  */
  out->payload=ob_new();

  /* XXX - we should merge contiguous observation reports so that packet loss 
     on the return path doesn't count against the link. */
  ob_append_ui32(out->payload,s1);
  ob_append_ui32(out->payload,s2);
  ob_append_byte(out->payload,interface);

  /* Add to queue. Keep broadcast status that we have assigned here if required to
     get ack back to sender before we have a route. */
  out->queue=OQ_MESH_MANAGEMENT;
  if (overlay_payload_enqueue(out))
    {
      op_free(out);
      return WHY("overlay_payload_enqueue(self-announce ack) failed");
    }
  
  /* XXX Remove any stale versions (or should we just freshen, and forget making
     a new one, since it would be more efficient). */

  return 0;
}

int overlay_route_make_neighbour(overlay_node *n)
{
  if (!n) return WHY("n is NULL");

  /* If it is already a neighbour, then return */
  if (n->neighbour_id) return 0;

  /* It isn't yet a neighbour, so find or free a neighbour slot */
  /* slot 0 is reserved, so skip it */
  if (!overlay_neighbour_count) overlay_neighbour_count=1;
  if (overlay_neighbour_count<overlay_max_neighbours) {
    /* Use next free neighbour slot */
    n->neighbour_id=overlay_neighbour_count++;
  } else {
    /* Evict an old neighbour */
    int nid=1+random()%(overlay_max_neighbours-1);
    if (overlay_neighbours[nid].node) overlay_neighbours[nid].node->neighbour_id=0;
    n->neighbour_id=nid;
  }
  bzero(&overlay_neighbours[n->neighbour_id],sizeof(struct overlay_neighbour));
  overlay_neighbours[n->neighbour_id].node=n;
  
  return 0;
}

struct overlay_neighbour *overlay_route_get_neighbour_structure(overlay_node *node, int createP)
{
  if (!node)
    return NULL;
  
  /* Check if node is already a neighbour, or if not, make it one */
  if (!node->neighbour_id){
    if (!createP)
      return NULL;
    
    if (overlay_route_make_neighbour(node))
      { WHY("overlay_route_make_neighbour() failed"); return NULL; }
  }

  /* Get neighbour structure */
  return &overlay_neighbours[node->neighbour_id];
}

int overlay_route_node_can_hear_me(struct subscriber *subscriber, int sender_interface,
				   unsigned int s1,unsigned int s2,
				   time_ms_t now)
{
  /* 1. Find (or create) node entry for the node.
     2. Replace oldest observation with this observation.
     3. Update score of how reliably we can hear this node */

  /* Get neighbour structure */
  
  struct overlay_neighbour *neh=overlay_route_get_neighbour_structure(get_node(subscriber, 1),1 /* create if necessary */);
  if (!neh)
    return WHY("Unable to create neighbour structure");
  
  int obs_index=neh->most_recent_observation_id;
  int merge=0;

  /* See if this observation is contiguous with a previous one, if so, merge.
     This not only reduces the number of observation slots we need, but dramatically speeds up
     the scanning of recent observations when re-calculating observation scores. */
  while (neh->observations[obs_index].valid && neh->observations[obs_index].s2 >= s1 - 1) {
    if (neh->observations[obs_index].sender_interface == sender_interface) {
      if (config.debug.overlayrouting)
	DEBUGF("merging observation into slot #%d s1=%u s2=%u", obs_index, neh->observations[obs_index].s1, neh->observations[obs_index].s2);
      s1 = neh->observations[obs_index].s1;
      merge=1;
      break;
    }
    if (--obs_index < 0)
      obs_index = OVERLAY_MAX_OBSERVATIONS - 1;
  }
  if (!merge) {
    /* Replace oldest observation with this one */
    obs_index = neh->most_recent_observation_id + 1;
    if (obs_index >= OVERLAY_MAX_OBSERVATIONS)
      obs_index = 0;
  }
  
  if (config.debug.overlayrouting)
    DEBUGF("assign observation slot #%d: s1=%u s2=%u time_ms=%lld", obs_index, s1, s2, (long long)now);
  neh->observations[obs_index].s1=s1;
  neh->observations[obs_index].s2=s2;
  neh->observations[obs_index].sender_interface=sender_interface;
  neh->observations[obs_index].time_ms=now;
  neh->observations[obs_index].valid=1;
  
  neh->most_recent_observation_id=obs_index;
  neh->last_observation_time_ms=now;
  /* force updating of stats for neighbour if we have added an observation */
  neh->last_metric_update=0;

  /* Update reachability metrics for node */
  if (overlay_route_recalc_neighbour_metrics(neh,now))
    return -1;

  if (config.debug.overlayroutemonitor) overlay_route_dump();
  return 0;
}

/* XXX Think about scheduling this node's score for readvertising? */
int overlay_route_recalc_node_metrics(overlay_node *n, time_ms_t now)
{
  int o;
  int best_score=0;
  int best_observation=-1;
  int reachable = REACHABLE_NONE;
  
  overlay_interface *interface=NULL;
  struct subscriber *next_hop=NULL;
  
  // TODO assumption timeout...
  if (n->subscriber->reachable&REACHABLE_ASSUMED){
    reachable=n->subscriber->reachable;
    interface=n->subscriber->interface;
  }
  
  if (n->neighbour_id)
  {
    /* Node is also a direct neighbour, so check score that way */
    if (n->neighbour_id>overlay_max_neighbours||n->neighbour_id<0)
      return WHY("n->neighbour_id is invalid.");
    
    struct overlay_neighbour *neighbour=&overlay_neighbours[n->neighbour_id];
    
    int i;
    for(i=0;i<overlay_interface_count;i++)
    {
      if (overlay_interfaces[i].state==INTERFACE_STATE_UP && 
	  neighbour->scores[i]>best_score)
      {
	best_score=neighbour->scores[i];
	best_observation=-1;
	reachable=REACHABLE_BROADCAST;
	interface = &overlay_interfaces[i];
      }
    }
  }

  if (best_score<=0){
    for(o=0;o<OVERLAY_MAX_OBSERVATIONS;o++)
      {
	// only count observations from neighbours that we *know* we have a 2 way path to
	if (n->observations[o].observed_score && n->observations[o].sender->reachable&REACHABLE
	    && !(n->observations[o].sender->reachable&REACHABLE_ASSUMED))
	  {
	    int discounted_score=n->observations[o].observed_score;
	    discounted_score-=(now-n->observations[o].rx_time)/1000;
	    if (discounted_score<0) discounted_score=0;
	    n->observations[o].corrected_score=discounted_score;
	    if (discounted_score>best_score)  {
	      best_score=discounted_score;
	      best_observation=o;
	      reachable=REACHABLE_INDIRECT;
	      next_hop=n->observations[o].sender;
	    }
	  }
      }
  }
  
  /* Think about scheduling this node's score for readvertising if its score
     has changed a lot?
     Really what we probably want is to advertise when the score goes up, since
     if it goes down, we probably don't need to say anything at all.
  */
  
  int diff=best_score - n->best_link_score;
  if (diff>0) {
    overlay_route_please_advertise(n);
    if (config.debug.overlayroutemonitor) overlay_route_dump();
  }
  int old_best = n->best_link_score;
  
  /* Remember new reachability information */
  switch (reachable){
    case REACHABLE_INDIRECT:
      n->subscriber->next_hop = next_hop;
      break;
    case REACHABLE_BROADCAST:
      n->subscriber->interface = interface;
      break;
  }
  n->best_link_score=best_score;
  n->best_observation=best_observation;
  set_reachable(n->subscriber, reachable);
  
  if (old_best && !best_score){
    INFOF("PEER UNREACHABLE, sid=%s", alloca_tohex_sid(n->subscriber->sid));
    overlay_send_probe(n->subscriber, n->subscriber->address, n->subscriber->interface, OQ_MESH_MANAGEMENT);
    
  }else if(best_score && !old_best){
    INFOF("PEER REACHABLE, sid=%s", alloca_tohex_sid(n->subscriber->sid));
    /* Make sure node is advertised soon */
    overlay_route_please_advertise(n);
  }
  
  return 0;
}

/* Recalculate node reachability metric, but only for directly connected nodes,
   i.e., link-local neighbours.

   The scores should be calculated separately for each interface we can
   hear the node on, so that this information can get back to the sender so that
   they know the best interface to use when trying to talk to us.

   For now we will calculate a weighted sum of recent reachability over some fixed
   length time interval.
   The sequence numbers are all based on a milli-second clock.
   
   For mobile mesh networks we need this metric to be very fast adapting to new
   paths, but to have a memory of older paths in case they are still useful.

   We thus combined equally a measure of very recent reachability (in last 10
   interface ticks perhaps?) with a measure of longer-term reachability (last
   200 seconds perhaps?).  Also, if no recent observations, then we further
   limit the score.
*/
int overlay_route_recalc_neighbour_metrics(struct overlay_neighbour *n, time_ms_t now)
{
  int i;
  time_ms_t most_recent_observation=0;
  IN();

  if (!n->node)
    RETURN(WHY("Neighbour is not a node"));

  if (config.debug.overlayrouting)
    DEBUGF("Updating neighbour metrics for %s", alloca_tohex_sid(n->node->subscriber->sid));
  
  /* At most one update per half second */
  if (n->last_metric_update == 0) {
    if (config.debug.overlayrouting)
      DEBUG("last update was never");
  } else {
    time_ms_t ago = now - n->last_metric_update;
    if (ago < 500) {
      if (config.debug.overlayrouting)
	DEBUGF("last update was %lldms ago -- skipping", (long long)ago);
      RETURN (0);
    }
    if (config.debug.overlayrouting)
      DEBUGF("last update was %lldms ago", (long long)ago);
  }
  n->last_metric_update = now;

  /* Somewhere to remember how many milliseconds we have seen */
  int ms_observed_5sec[OVERLAY_MAX_INTERFACES];
  int ms_observed_200sec[OVERLAY_MAX_INTERFACES];
  for(i=0;i<OVERLAY_MAX_INTERFACES;i++) {
    ms_observed_5sec[i]=0;
    ms_observed_200sec[i]=0;
  }

  /* XXX This simple accumulation scheme does not weed out duplicates, nor weight for recency of
     communication.
     Also, we might like to take into account the interface we received 
     the announcements on. */
  for(i=0;i<OVERLAY_MAX_OBSERVATIONS;i++) {
    if (!n->observations[i].valid ||
	n->observations[i].sender_interface>=OVERLAY_MAX_INTERFACES ||
	overlay_interfaces[n->observations[i].sender_interface].state!=INTERFACE_STATE_UP)
      continue;
      
    /* Work out the interval covered by the observation.
       The times are represented as lowest 32 bits of a 64-bit 
       millisecond clock.  This introduces modulo problems, 
       however by using 32-bit modulo arithmatic here, we avoid
       most of them. */
    unsigned int interval=n->observations[i].s2-n->observations[i].s1;      
    
    /* Check the observation age, and ignore if too old */
    time_ms_t obs_age = now - n->observations[i].time_ms;
    if (config.debug.overlayrouting)
      DEBUGF("tallying obs: %lldms old, %ums long", obs_age,interval);
    
    /* Ignore very large intervals (>1hour) as being likely to be erroneous.
     (or perhaps a clock wrap due to the modulo arithmatic)
     
     One tick per hour should be well and truly slow enough to do
     50KB per 12 hours, which is the minimum traffic charge rate 
     on an expensive BGAN satellite link. 	 
     */
    if (interval>=3600000 || obs_age>20000)
      continue;

    if (config.debug.overlayrouting) 
      DEBUGF("adding %dms (interface %d '%s')",
	      interval,n->observations[i].sender_interface,
	      overlay_interfaces[n->observations[i].sender_interface].name);

    ms_observed_200sec[n->observations[i].sender_interface]+=interval;
    if (obs_age<=5000){
      ms_observed_5sec[n->observations[i].sender_interface]+=(interval>5000?5000:interval);
    }

    if (n->observations[i].time_ms>most_recent_observation) most_recent_observation=n->observations[i].time_ms;
  }

  /* From the sum of observations calculate the metrics.
     We want the score to climb quickly and then plateu.
  */
  
  int scoreChanged=0;
  
  for(i=0;i<OVERLAY_MAX_INTERFACES;i++) {
    int score;
    if (ms_observed_200sec[i]>200000) ms_observed_200sec[i]=200000;
    if (ms_observed_5sec[i]>5000) ms_observed_5sec[i]=5000;
    if (ms_observed_200sec[i]==0) {
      // Not observed at all
      score=0;
    } else {
      int contrib_200=ms_observed_200sec[i]/(200000/128);
      int contrib_5=ms_observed_5sec[i]/(5000/128);

      if (contrib_5<1)
	score=contrib_200/2; 
      else
	score=contrib_5+contrib_200;      

      /* Deal with invalid sequence number ranges */
      if (score<1) score=1;
      if (score>255) score=255;
    }

    if (n->scores[i]!=score){
      scoreChanged=1;
      n->scores[i]=score;
    }
    if ((config.debug.overlayrouting)&&score)
      DEBUGF("Neighbour score on interface #%d = %d (observations for %dms)",i,score,ms_observed_200sec[i]);
  }
  if (scoreChanged)
    overlay_route_recalc_node_metrics(n->node, now);
  
  RETURN(0);
}

/* 
   Self-announcement acks bounce back to the self-announcer from immediate neighbours
   who report the link score they have calculated based on listening to self-announces
   from that peer.  By acking them these scores then get to the originator, who then
   has a score for the link to their neighbour, which is measuring the correct
   direction of the link. 

   Frames consist of 32bit timestamp in seconds followed by zero or more entries
   of the format:
   
   8bits - link score
   8bits - interface number

   this is followed by a 00 byte to indicate the end.

   That way we don't waste lots of bytes on single-interface nodes.
   (But I am sure we can do better).

   These link scores should get stored in our node list as compared to our neighbour list,
   with the node itself listed as the nexthop that the score is associated with.
*/
int overlay_route_saw_selfannounce_ack(struct overlay_frame *f,long long now)
{
  IN();
  if (config.debug.overlayrouting)
    DEBUGF("processing selfannounce ack (payload length=%d)",f->payload->sizeLimit);
  
  if (f->payload->sizeLimit<9) 
    RETURN(WHY("selfannounce ack packet too short"));

  unsigned int s1=ob_get_ui32(f->payload);
  unsigned int s2=ob_get_ui32(f->payload);
  int iface=ob_get(f->payload);

  // Call something like the following for each link
  overlay_route_node_can_hear_me(f->source,iface,s1,s2,now);
  
  RETURN(0);
}

/* if to and via are the same, then this is evidence that we can get to the
   node directly. */
int overlay_route_record_link(time_ms_t now, struct subscriber *to,
			      struct subscriber *via,int sender_interface,
			      unsigned int s1,unsigned int s2,int score,
			      int gateways_en_route)
{
  IN();
  if (config.debug.overlayrouting)
    DEBUGF("to=%s, via=%s, sender_interface=%d, s1=%d, s2=%d score=%d gateways_en_route=%d",
	alloca_tohex_sid(to->sid), alloca_tohex_sid(via->sid), sender_interface, s1, s2,
	score, gateways_en_route
      );
 
  if (sender_interface>OVERLAY_MAX_INTERFACES || score == 0) {
    if (config.debug.overlayrouting)
      DEBUG("invalid report");
    RETURN(0);
  }

  overlay_node *n = get_node(to,1);
  if (!n)
    RETURN(WHY("Could not create entry for node"));
  
  int slot = -1;
  int i;
  for (i = 0; i < OVERLAY_MAX_OBSERVATIONS; ++i) {
    /* Take note of where we can find space for a fresh observation */
    if (slot == -1 && n->observations[i].observed_score == 0)
      slot = i;
    /* If the intermediate host ("via") address and interface numbers match, then overwrite old
       observation with new one */
    if (n->observations[i].sender == via) {
      slot = i;
      break;
    }
  }
  /* If in doubt, replace a random slot.
     XXX - we should probably replace the lowest scoring slot instead, but random will work well
     enough for now. */
  if (slot == -1) {
    slot = random() % OVERLAY_MAX_OBSERVATIONS;
    if (config.debug.overlayrouting)
      DEBUGF("allocate observation slot=%d", slot);
  } else {
    if (config.debug.overlayrouting)
      DEBUGF("overwrite observation slot=%d (sender=%s interface=%u observed_score=%u rx_time=%lld)",
	  slot,
	  n->observations[slot].sender?alloca_tohex_sid(n->observations[slot].sender->sid):"[None]",
	  n->observations[slot].interface,
	  n->observations[slot].observed_score,
	  n->observations[slot].rx_time
	);
  }

  n->observations[slot].observed_score=0;
  n->observations[slot].gateways_en_route=gateways_en_route;
  n->observations[slot].rx_time=now;
  n->observations[slot].sender = via;
  n->observations[slot].observed_score=score;
  n->observations[slot].interface=sender_interface;
  
  /* Remember that we have seen an observation for this node.
     XXX - This should actually be set to the time that the last first-hand
     observation of the node was made, so that stale information doesn't build
     false belief of reachability.
     This is why the timestamp field is supplied, which is just copied from the
     original selfannouncement ack.  We just have to register it against our
     local time to interpret it (XXX which comes with some risks related to
     clock-skew, but we will deal with those in due course).
  */
  n->last_observation_time_ms=now;
  if (s2>n->last_first_hand_observation_time_millisec)
    n->last_first_hand_observation_time_millisec=s2;

  overlay_route_recalc_node_metrics(n,now);
  
  if (config.debug.overlayroutemonitor)
    overlay_route_dump();
  
  RETURN(0);
}

int node_dump(struct subscriber *subscriber, void *context){
  strbuf *b=context;
  overlay_node *node = subscriber->node;
  int o;
  
  if (node){
    
    strbuf_sprintf(*b,"  %s* : %d :", alloca_tohex(subscriber->sid, 7),
		   node->best_link_score);
    for(o=0;o<OVERLAY_MAX_OBSERVATIONS;o++)
    {
      if (node->observations[o].observed_score)
      {
	overlay_node_observation *ob=&node->observations[o];
	if (ob->corrected_score)
	  strbuf_sprintf(*b," %d/%d via %s*",
			 ob->corrected_score,ob->gateways_en_route,
			 alloca_tohex(ob->sender->sid,7));
      }
    }       
    strbuf_sprintf(*b,"\n");
  }
  return 0;
}

int overlay_route_dump()
{
  int n,i;
  time_ms_t now = gettime_ms();
  strbuf b = strbuf_alloca(8192);

  strbuf_sprintf(b,"Overlay Local Identities\n------------------------\n");
  int cn,in,kp;
  for(cn=0;cn<keyring->context_count;cn++)
    for(in=0;in<keyring->contexts[cn]->identity_count;in++)
      for(kp=0;kp<keyring->contexts[cn]->identities[in]->keypair_count;kp++)
	if (keyring->contexts[cn]->identities[in]->keypairs[kp]->type
	    ==KEYTYPE_CRYPTOBOX)
	  {
	    for(i=0;i<SID_SIZE;i++)
	      strbuf_sprintf(b,"%02x",keyring->contexts[cn]->identities[in]
		      ->keypairs[kp]->public_key[i]);
	    strbuf_sprintf(b,"\n");
	  }
  DEBUG(strbuf_str(b));

  strbuf_reset(b);
  strbuf_sprintf(b,"\nOverlay Neighbour Table\n------------------------\n");
  for(n=0;n<overlay_neighbour_count;n++)
    if (overlay_neighbours[n].node)
      {
	strbuf_sprintf(b,"  %s* : %lldms ago :",
		alloca_tohex(overlay_neighbours[n].node->subscriber->sid, 7),
		(long long)(now - overlay_neighbours[n].last_observation_time_ms));
	for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
	  if (overlay_neighbours[n].scores[i]) 
	    strbuf_sprintf(b," %d(via #%d)",
		    overlay_neighbours[n].scores[i],i);
	strbuf_sprintf(b,"\n");
      }
  DEBUG(strbuf_str(b));
  
  strbuf_reset(b);
  strbuf_sprintf(b,"Overlay Mesh Route Table\n------------------------\n");
  
  enum_subscribers(NULL, node_dump, &b);
  
  DEBUG(strbuf_str(b));
  return 0;
}

/* Ticking neighbours is easy; we just pretend we have heard from them again,
   and recalculate the score that way, which already includes a mechanism for
   taking into account the age of the most recent observation */
int overlay_route_tick_neighbour(int neighbour_id, time_ms_t now)
{
  if (neighbour_id>0 && overlay_neighbours[neighbour_id].node)
    if (overlay_route_recalc_neighbour_metrics(&overlay_neighbours[neighbour_id],now)) 
      WHY("overlay_route_recalc_neighbour_metrics() failed");
  
  return 0;
}

/* Updating the route score to get to a node it trickier, as they might not be a
   neighbour.  Even if they are a neighbour, all we have to go on is the node's
   observations.
   From these we can work out a discounted score based on their age.

   XXX This is where the discounting should be modified for nodes that are 
   updated less often as they exhibit score stability.  Actually, for the
   most part we can tolerate these without any special action, as their high
   scores will keep them reachable for longer anyway.
*/
int overlay_route_tick_node(struct subscriber *subscriber, void *context)
{
  if (subscriber->node)
    overlay_route_recalc_node_metrics(subscriber->node, gettime_ms());
  return 0;
}

void overlay_route_tick(struct sched_ent *alarm)
{
  int n;
  time_ms_t now = gettime_ms();
  
  /* Go through some of neighbour list */
  for (n=0;n<overlay_max_neighbours;n++)
    overlay_route_tick_neighbour(n,now);
  
  /* Go through the node list */
  enum_subscribers(NULL, overlay_route_tick_node, NULL);
  
  /* Update callback interval based on how much work we have to do */
  alarm->alarm = gettime_ms()+5000;
  alarm->deadline = alarm->alarm+100;
  schedule(alarm);
  return;
}

int overlay_route_node_info(overlay_mdp_nodeinfo *node_info)
{
  time_ms_t now = gettime_ms();

  if (0) 
    DEBUGF("Looking for node %s* (prefix len=0x%x)",
	 alloca_tohex(node_info->sid, node_info->sid_prefix_length),
	 node_info->sid_prefix_length
	 );

  node_info->foundP=0;
  
  /* check if it is a local identity */
  int cn,in,kp;
  for(cn=0;cn<keyring->context_count;cn++)
    for(in=0;in<keyring->contexts[cn]->identity_count;in++)
      for(kp=0;kp<keyring->contexts[cn]->identities[in]->keypair_count;kp++)
	if (keyring->contexts[cn]->identities[in]->keypairs[kp]->type
	    ==KEYTYPE_CRYPTOBOX)
	  {
	    if (!memcmp(&node_info->sid[0],
			&keyring->contexts[cn]->identities[in]
			->keypairs[kp]->public_key[0],
			node_info->sid_prefix_length/2))
	      {
		node_info->foundP=1;
		node_info->localP=1;
		node_info->neighbourP=0;
		node_info->time_since_last_observation = 0;
		node_info->score=256;
		node_info->interface_number=-1;
		bcopy(&keyring->contexts[cn]->identities[in]
		      ->keypairs[kp]->public_key[0],
		      &node_info->sid[0],SID_SIZE);

		return 0;
	      }
	  }

  struct subscriber *subscriber = find_subscriber(node_info->sid, node_info->sid_prefix_length/2, 0);
  if (subscriber && subscriber->node){
    overlay_node *node = subscriber->node;
    
    node_info->foundP=1;
    node_info->localP=0;
    node_info->score=-1;
    node_info->interface_number=-1;
    bcopy(subscriber->sid,
	  node_info->sid,SID_SIZE);
    
    if (subscriber->node->neighbour_id){
      int n = subscriber->node->neighbour_id;
      node_info->neighbourP=1;
      node_info->time_since_last_observation = now - overlay_neighbours[n].last_observation_time_ms;
      
      int i;
      for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
	if (overlay_neighbours[n].scores[i]>node_info->score)
	{
	  node_info->score=overlay_neighbours[n].scores[i];
	  node_info->interface_number=i;
	}
      
    }else{
      node_info->neighbourP=0;
      node_info->time_since_last_observation = -1;
      int o;
      for(o=0;o<OVERLAY_MAX_OBSERVATIONS;o++)
	if (node->observations[o].observed_score)
	{
	  overlay_node_observation *ob
	  =&node->observations[o];
	  if (ob->corrected_score>node_info->score) {
	    node_info->score=ob->corrected_score;
	  }
	  if (node_info->time_since_last_observation == -1 || now - ob->rx_time < node_info->time_since_last_observation)
	    node_info->time_since_last_observation = now - ob->rx_time;
	}
    }
  }

  return 0;
}
