/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ip/format.h>
#include <vnet/ip/ip.h>
#include <vnet/dpo/drop_dpo.h>
#include <vnet/dpo/receive_dpo.h>
#include <vnet/dpo/load_balance_map.h>
#include <vnet/dpo/lookup_dpo.h>

#include <vnet/adj/adj.h>

#include <vnet/fib/fib_path.h>
#include <vnet/fib/fib_node.h>
#include <vnet/fib/fib_table.h>
#include <vnet/fib/fib_entry.h>
#include <vnet/fib/fib_path_list.h>
#include <vnet/fib/fib_internal.h>
#include <vnet/fib/fib_urpf_list.h>

/**
 * Enurmeration of path types
 */
typedef enum fib_path_type_t_ {
    /**
     * Marker. Add new types after this one.
     */
    FIB_PATH_TYPE_FIRST = 0,
    /**
     * Attached-nexthop. An interface and a nexthop are known.
     */
    FIB_PATH_TYPE_ATTACHED_NEXT_HOP = FIB_PATH_TYPE_FIRST,
    /**
     * attached. Only the interface is known.
     */
    FIB_PATH_TYPE_ATTACHED,
    /**
     * recursive. Only the next-hop is known.
     */
    FIB_PATH_TYPE_RECURSIVE,
    /**
     * special. nothing is known. so we drop.
     */
    FIB_PATH_TYPE_SPECIAL,
    /**
     * exclusive. user provided adj.
     */
    FIB_PATH_TYPE_EXCLUSIVE,
    /**
     * deag. Link to a lookup adj in the next table
     */
    FIB_PATH_TYPE_DEAG,
    /**
     * receive. it's for-us.
     */
    FIB_PATH_TYPE_RECEIVE,
    /**
     * Marker. Add new types before this one, then update it.
     */
    FIB_PATH_TYPE_LAST = FIB_PATH_TYPE_RECEIVE,
} __attribute__ ((packed)) fib_path_type_t;

/**
 * The maximum number of path_types
 */
#define FIB_PATH_TYPE_MAX (FIB_PATH_TYPE_LAST + 1)

#define FIB_PATH_TYPES {					\
    [FIB_PATH_TYPE_ATTACHED_NEXT_HOP] = "attached-nexthop",	\
    [FIB_PATH_TYPE_ATTACHED]          = "attached",		\
    [FIB_PATH_TYPE_RECURSIVE]         = "recursive",	        \
    [FIB_PATH_TYPE_SPECIAL]           = "special",	        \
    [FIB_PATH_TYPE_EXCLUSIVE]         = "exclusive",	        \
    [FIB_PATH_TYPE_DEAG]              = "deag",	                \
    [FIB_PATH_TYPE_RECEIVE]           = "receive",	        \
}

#define FOR_EACH_FIB_PATH_TYPE(_item) \
    for (_item = FIB_PATH_TYPE_FIRST; _item <= FIB_PATH_TYPE_LAST; _item++)

/**
 * Enurmeration of path operational (i.e. derived) attributes
 */
typedef enum fib_path_oper_attribute_t_ {
    /**
     * Marker. Add new types after this one.
     */
    FIB_PATH_OPER_ATTRIBUTE_FIRST = 0,
    /**
     * The path forms part of a recursive loop.
     */
    FIB_PATH_OPER_ATTRIBUTE_RECURSIVE_LOOP = FIB_PATH_OPER_ATTRIBUTE_FIRST,
    /**
     * The path is resolved
     */
    FIB_PATH_OPER_ATTRIBUTE_RESOLVED,
    /**
     * The path has become a permanent drop.
     */
    FIB_PATH_OPER_ATTRIBUTE_DROP,
    /**
     * Marker. Add new types before this one, then update it.
     */
    FIB_PATH_OPER_ATTRIBUTE_LAST = FIB_PATH_OPER_ATTRIBUTE_DROP,
} __attribute__ ((packed)) fib_path_oper_attribute_t;

/**
 * The maximum number of path operational attributes
 */
#define FIB_PATH_OPER_ATTRIBUTE_MAX (FIB_PATH_OPER_ATTRIBUTE_LAST + 1)

#define FIB_PATH_OPER_ATTRIBUTES {					\
    [FIB_PATH_OPER_ATTRIBUTE_RECURSIVE_LOOP] = "recursive-loop",	\
    [FIB_PATH_OPER_ATTRIBUTE_RESOLVED]       = "resolved",	        \
    [FIB_PATH_OPER_ATTRIBUTE_DROP]           = "drop",		        \
}

#define FOR_EACH_FIB_PATH_OPER_ATTRIBUTE(_item) \
    for (_item = FIB_PATH_OPER_ATTRIBUTE_FIRST; \
	 _item <= FIB_PATH_OPER_ATTRIBUTE_LAST; \
	 _item++)

/**
 * Path flags from the attributes
 */
typedef enum fib_path_oper_flags_t_ {
    FIB_PATH_OPER_FLAG_NONE = 0,
    FIB_PATH_OPER_FLAG_RECURSIVE_LOOP = (1 << FIB_PATH_OPER_ATTRIBUTE_RECURSIVE_LOOP),
    FIB_PATH_OPER_FLAG_DROP = (1 << FIB_PATH_OPER_ATTRIBUTE_DROP),
    FIB_PATH_OPER_FLAG_RESOLVED = (1 << FIB_PATH_OPER_ATTRIBUTE_RESOLVED),
} __attribute__ ((packed)) fib_path_oper_flags_t;

/**
 * A FIB path
 */
typedef struct fib_path_t_ {
    /**
     * A path is a node in the FIB graph.
     */
    fib_node_t fp_node;

    /**
     * The index of the path-list to which this path belongs
     */
    u32 fp_pl_index;

    /**
     * This marks the start of the memory area used to hash
     * the path
     */
    STRUCT_MARK(path_hash_start);

    /**
     * Configuration Flags
     */
    fib_path_cfg_flags_t fp_cfg_flags;

    /**
     * The type of the path. This is the selector for the union
     */
    fib_path_type_t fp_type;

    /**
     * The protocol of the next-hop, i.e. the address family of the
     * next-hop's address. We can't derive this from the address itself
     * since the address can be all zeros
     */
    fib_protocol_t fp_nh_proto;

    /**
     * UCMP [unnormalised] weigt
     */
    u32 fp_weight;

    /**
     * per-type union of the data required to resolve the path
     */
    union {
	struct {
	    /**
	     * The next-hop
	     */
	    ip46_address_t fp_nh;
	    /**
	     * The interface
	     */
	    u32 fp_interface;
	} attached_next_hop;
	struct {
	    /**
	     * The interface
	     */
	    u32 fp_interface;
	} attached;
	struct {
	    union
	    {
		/**
		 * The next-hop
		 */
		ip46_address_t fp_ip;
		/**
		 * The local label to resolve through.
		 */
		mpls_label_t fp_local_label;
	    } fp_nh;
	    /**
	     * The FIB table index in which to find the next-hop.
	     * This needs to be fixed. We should lookup the adjacencies in
	     * a separate table of adjacencies, rather than from the FIB.
	     * Two reasons I can think of:
	     *   - consider:
	     *       int ip addr Gig0 10.0.0.1/24
	     *       ip route 10.0.0.2/32 via Gig1 192.168.1.2
	     *       ip route 1.1.1.1/32 via Gig0 10.0.0.2
	     *     this is perfectly valid.
	     *     Packets addressed to 10.0.0.2 should be sent via Gig1.
	     *     Packets address to 1.1.1.1 should be sent via Gig0.
	     *    when we perform the adj resolution from the FIB for the path
	     *    "via Gig0 10.0.0.2" the lookup will result in the route via Gig1
	     *    and so we will pick up the adj via Gig1 - which was not what the
	     *    operator wanted.
	     *  - we can only return link-type IPv4 and so not the link-type MPLS.
	     *    more on this in a later commit.
	     *
	     * The table ID should only belong to a recursive path and indicate
	     * which FIB should be used to resolve the next-hop.
	     */
	    fib_node_index_t fp_tbl_id;
	} recursive;
	struct {
	    /**
	     * The FIB index in which to perfom the next lookup
	     */
	    fib_node_index_t fp_tbl_id;
	} deag;
	struct {
	} special;
	struct {
	    /**
	     * The user provided 'exclusive' DPO
	     */
	    dpo_id_t fp_ex_dpo;
	} exclusive;
	struct {
	    /**
	     * The interface on which the local address is configured
	     */
	    u32 fp_interface;
	    /**
	     * The next-hop
	     */
	    ip46_address_t fp_addr;
	} receive;
    };
    STRUCT_MARK(path_hash_end);

    /**
     * Memebers in this last section represent information that is
     * dervied during resolution. It should not be copied to new paths
     * nor compared.
     */

    /**
     * Operational Flags
     */
    fib_path_oper_flags_t fp_oper_flags;

    /**
     * the resolving via fib. not part of the union, since it it not part
     * of the path's hash.
     */
    fib_node_index_t fp_via_fib;

    /**
     * The Data-path objects through which this path resolves for IP.
     */
    dpo_id_t fp_dpo;

    /**
     * the index of this path in the parent's child list.
     */
    u32 fp_sibling;
} fib_path_t;

/*
 * Array of strings/names for the path types and attributes
 */
static const char *fib_path_type_names[] = FIB_PATH_TYPES;
static const char *fib_path_oper_attribute_names[] = FIB_PATH_OPER_ATTRIBUTES;
static const char *fib_path_cfg_attribute_names[]  = FIB_PATH_CFG_ATTRIBUTES;

/*
 * The memory pool from which we allocate all the paths
 */
static fib_path_t *fib_path_pool;

/*
 * Debug macro
 */
#ifdef FIB_DEBUG
#define FIB_PATH_DBG(_p, _fmt, _args...)			\
{       		          				\
    u8 *_tmp = NULL;						\
    _tmp = fib_path_format(fib_path_get_index(_p), _tmp);	\
    clib_warning("path:[%d:%s]:" _fmt,				\
		 fib_path_get_index(_p), _tmp,			\
		 ##_args);					\
    vec_free(_tmp);						\
}
#else
#define FIB_PATH_DBG(_p, _fmt, _args...)
#endif

static fib_path_t *
fib_path_get (fib_node_index_t index)
{
    return (pool_elt_at_index(fib_path_pool, index));
}

static fib_node_index_t 
fib_path_get_index (fib_path_t *path)
{
    return (path - fib_path_pool);
}

static fib_node_t *
fib_path_get_node (fib_node_index_t index)
{
    return ((fib_node_t*)fib_path_get(index));
}

static fib_path_t*
fib_path_from_fib_node (fib_node_t *node)
{
#if CLIB_DEBUG > 0
    ASSERT(FIB_NODE_TYPE_PATH == node->fn_type);
#endif
    return ((fib_path_t*)node);
}

u8 *
format_fib_path (u8 * s, va_list * args)
{
    fib_path_t *path = va_arg (*args, fib_path_t *);
    vnet_main_t * vnm = vnet_get_main();
    fib_path_oper_attribute_t oattr;
    fib_path_cfg_attribute_t cattr;

    s = format (s, "      index:%d ", fib_path_get_index(path));
    s = format (s, "pl-index:%d ", path->fp_pl_index);
    s = format (s, "%U ", format_fib_protocol, path->fp_nh_proto);
    s = format (s, "weight=%d ", path->fp_weight);
    s = format (s, "%s: ", fib_path_type_names[path->fp_type]);
    if (FIB_PATH_OPER_FLAG_NONE != path->fp_oper_flags) {
	s = format(s, " oper-flags:");
	FOR_EACH_FIB_PATH_OPER_ATTRIBUTE(oattr) {
	    if ((1<<oattr) & path->fp_oper_flags) {
		s = format (s, "%s,", fib_path_oper_attribute_names[oattr]);
	    }
	}
    }
    if (FIB_PATH_CFG_FLAG_NONE != path->fp_cfg_flags) {
	s = format(s, " cfg-flags:");
	FOR_EACH_FIB_PATH_CFG_ATTRIBUTE(cattr) {
	    if ((1<<cattr) & path->fp_cfg_flags) {
		s = format (s, "%s,", fib_path_cfg_attribute_names[cattr]);
	    }
	}
    }
    s = format(s, "\n       ");

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	s = format (s, "%U", format_ip46_address,
		    &path->attached_next_hop.fp_nh,
		    IP46_TYPE_ANY);
	if (path->fp_oper_flags & FIB_PATH_OPER_FLAG_DROP)
	{
	    s = format (s, " if_index:%d", path->attached_next_hop.fp_interface);
	}
	else
	{
	    s = format (s, " %U",
			format_vnet_sw_interface_name,
			vnm,
			vnet_get_sw_interface(
			    vnm,
			    path->attached_next_hop.fp_interface));
	    if (vnet_sw_interface_is_p2p(vnet_get_main(),
					 path->attached_next_hop.fp_interface))
	    {
		s = format (s, " (p2p)");
	    }
	}
	if (!dpo_id_is_valid(&path->fp_dpo))
	{
	    s = format(s, "\n          unresolved");
	}
	else
	{
	    s = format(s, "\n          %U",
		       format_dpo_id,
		       &path->fp_dpo, 13);
	}
	break;
    case FIB_PATH_TYPE_ATTACHED:
	if (path->fp_oper_flags & FIB_PATH_OPER_FLAG_DROP)
	{
	    s = format (s, " if_index:%d", path->attached_next_hop.fp_interface);
	}
	else
	{
	    s = format (s, " %U",
			format_vnet_sw_interface_name,
			vnm,
			vnet_get_sw_interface(
			    vnm,
			    path->attached.fp_interface));
	}
	break;
    case FIB_PATH_TYPE_RECURSIVE:
	if (FIB_PROTOCOL_MPLS == path->fp_nh_proto)
	{
	    s = format (s, "via %U",
			format_mpls_unicast_label,
			path->recursive.fp_nh.fp_local_label);
	}
	else
	{
	    s = format (s, "via %U",
			format_ip46_address,
			&path->recursive.fp_nh.fp_ip,
			IP46_TYPE_ANY);
	}
	s = format (s, " in fib:%d",
		    path->recursive.fp_tbl_id,
		    path->fp_via_fib); 
	s = format (s, " via-fib:%d", path->fp_via_fib); 
	s = format (s, " via-dpo:[%U:%d]",
		    format_dpo_type, path->fp_dpo.dpoi_type, 
		    path->fp_dpo.dpoi_index);

	break;
    case FIB_PATH_TYPE_RECEIVE:
    case FIB_PATH_TYPE_SPECIAL:
    case FIB_PATH_TYPE_DEAG:
    case FIB_PATH_TYPE_EXCLUSIVE:
	if (dpo_id_is_valid(&path->fp_dpo))
	{
	    s = format(s, "%U", format_dpo_id,
		       &path->fp_dpo, 2);
	}
	break;
    }
    return (s);
}

u8 *
fib_path_format (fib_node_index_t pi, u8 *s)
{
    fib_path_t *path;

    path = fib_path_get(pi);
    ASSERT(NULL != path);

    return (format (s, "%U", format_fib_path, path));
}

u8 *
fib_path_adj_format (fib_node_index_t pi,
		     u32 indent,
		     u8 *s)
{
    fib_path_t *path;

    path = fib_path_get(pi);
    ASSERT(NULL != path);

    if (!dpo_id_is_valid(&path->fp_dpo))
    {
	s = format(s, " unresolved");
    }
    else
    {
	s = format(s, "%U", format_dpo_id,
		   &path->fp_dpo, 2);
    }

    return (s);
}

/*
 * fib_path_last_lock_gone
 *
 * We don't share paths, we share path lists, so the [un]lock functions
 * are no-ops
 */
static void
fib_path_last_lock_gone (fib_node_t *node)
{
    ASSERT(0);
}

static const adj_index_t
fib_path_attached_next_hop_get_adj (fib_path_t *path,
				    vnet_link_t link)
{
    if (vnet_sw_interface_is_p2p(vnet_get_main(),
				 path->attached_next_hop.fp_interface))
    {
	/*
	 * if the interface is p2p then the adj for the specific
	 * neighbour on that link will never exist. on p2p links
	 * the subnet address (the attached route) links to the
	 * auto-adj (see below), we want that adj here too.
	 */
	return (adj_nbr_add_or_lock(path->fp_nh_proto,
				    link,
				    &zero_addr,
				    path->attached_next_hop.fp_interface));
    }
    else
    {
	return (adj_nbr_add_or_lock(path->fp_nh_proto,
				    link,
				    &path->attached_next_hop.fp_nh,
				    path->attached_next_hop.fp_interface));
    }
}

static void
fib_path_attached_next_hop_set (fib_path_t *path)
{
    /*
     * resolve directly via the adjacnecy discribed by the
     * interface and next-hop
     */
    if (!vnet_sw_interface_is_admin_up(vnet_get_main(),
				      path->attached_next_hop.fp_interface))
    {
	path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
    }

    dpo_set(&path->fp_dpo,
	    DPO_ADJACENCY,
	    fib_proto_to_dpo(path->fp_nh_proto),
	    fib_path_attached_next_hop_get_adj(
		 path,
		 fib_proto_to_link(path->fp_nh_proto)));

    /*
     * become a child of the adjacency so we receive updates
     * when its rewrite changes
     */
    path->fp_sibling = adj_child_add(path->fp_dpo.dpoi_index,
				     FIB_NODE_TYPE_PATH,
				     fib_path_get_index(path));
}

/*
 * create of update the paths recursive adj
 */
static void
fib_path_recursive_adj_update (fib_path_t *path,
			       fib_forward_chain_type_t fct,
			       dpo_id_t *dpo)
{
    dpo_id_t via_dpo = DPO_INVALID;

    /*
     * get the DPO to resolve through from the via-entry
     */
    fib_entry_contribute_forwarding(path->fp_via_fib,
				    fct,
				    &via_dpo);


    /*
     * hope for the best - clear if restrictions apply.
     */
    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RESOLVED;

    /*
     * Validate any recursion constraints and over-ride the via
     * adj if not met
     */
    if (path->fp_oper_flags & FIB_PATH_OPER_FLAG_RECURSIVE_LOOP)
    {
	path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
	dpo_copy(&via_dpo, drop_dpo_get(fib_proto_to_dpo(path->fp_nh_proto)));
    }
    else if (path->fp_cfg_flags & FIB_PATH_CFG_FLAG_RESOLVE_HOST)
    {
	/*
	 * the via FIB must be a host route.
	 * note the via FIB just added will always be a host route
	 * since it is an RR source added host route. So what we need to
	 * check is whether the route has other sources. If it does then
	 * some other source has added it as a host route. If it doesn't
	 * then it was added only here and inherits forwarding from a cover.
	 * the cover is not a host route.
	 * The RR source is the lowest priority source, so we check if it
	 * is the best. if it is there are no other sources.
	 */
	if (fib_entry_get_best_source(path->fp_via_fib) >= FIB_SOURCE_RR)
	{
	    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
            dpo_copy(&via_dpo, drop_dpo_get(fib_proto_to_dpo(path->fp_nh_proto)));

            /*
             * PIC edge trigger. let the load-balance maps know
             */
            load_balance_map_path_state_change(fib_path_get_index(path));
	}
    }
    else if (path->fp_cfg_flags & FIB_PATH_CFG_FLAG_RESOLVE_ATTACHED)
    {
	/*
	 * RR source entries inherit the flags from the cover, so
	 * we can check the via directly
	 */
	if (!(FIB_ENTRY_FLAG_ATTACHED & fib_entry_get_flags(path->fp_via_fib)))
	{
	    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
            dpo_copy(&via_dpo, drop_dpo_get(fib_proto_to_dpo(path->fp_nh_proto)));

            /*
             * PIC edge trigger. let the load-balance maps know
             */
            load_balance_map_path_state_change(fib_path_get_index(path));
	}
    }

    /*
     * update the path's contributed DPO
     */
    dpo_copy(dpo, &via_dpo);

    FIB_PATH_DBG(path, "recursive update: %U",
		 fib_get_lookup_main(path->fp_nh_proto),
		 &path->fp_dpo, 2);

    dpo_reset(&via_dpo);
}

/*
 * fib_path_is_permanent_drop
 *
 * Return !0 if the path is configured to permanently drop,
 * despite other attributes.
 */
static int
fib_path_is_permanent_drop (fib_path_t *path)
{
    return ((path->fp_cfg_flags & FIB_PATH_CFG_FLAG_DROP) ||
	    (path->fp_oper_flags & FIB_PATH_OPER_FLAG_DROP));
}

/*
 * fib_path_unresolve
 *
 * Remove our dependency on the resolution target
 */
static void
fib_path_unresolve (fib_path_t *path)
{
    /*
     * the forced drop path does not need unresolving
     */
    if (fib_path_is_permanent_drop(path))
    {
	return;
    }

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_RECURSIVE:
	if (FIB_NODE_INDEX_INVALID != path->fp_via_fib)
	{
	    fib_prefix_t pfx;

	    fib_entry_get_prefix(path->fp_via_fib, &pfx);
	    fib_entry_child_remove(path->fp_via_fib,
				   path->fp_sibling);
	    fib_table_entry_special_remove(path->recursive.fp_tbl_id,
					   &pfx,
					   FIB_SOURCE_RR);
	    path->fp_via_fib = FIB_NODE_INDEX_INVALID;
	}
	break;
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
    case FIB_PATH_TYPE_ATTACHED:
	adj_child_remove(path->fp_dpo.dpoi_index,
			 path->fp_sibling);
        adj_unlock(path->fp_dpo.dpoi_index);
        break;
    case FIB_PATH_TYPE_EXCLUSIVE:
	dpo_reset(&path->exclusive.fp_ex_dpo);
        break;
    case FIB_PATH_TYPE_SPECIAL:
    case FIB_PATH_TYPE_RECEIVE:
    case FIB_PATH_TYPE_DEAG:
        /*
         * these hold only the path's DPO, which is reset below.
         */
	break;
    }

    /*
     * release the adj we were holding and pick up the
     * drop just in case.
     */
    dpo_reset(&path->fp_dpo);
    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;

    return;
}

static fib_forward_chain_type_t
fib_path_proto_to_chain_type (fib_protocol_t proto)
{
    switch (proto)
    {
    case FIB_PROTOCOL_IP4:
	return (FIB_FORW_CHAIN_TYPE_UNICAST_IP4);
    case FIB_PROTOCOL_IP6:
	return (FIB_FORW_CHAIN_TYPE_UNICAST_IP6);
    case FIB_PROTOCOL_MPLS:
	return (FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS);
    }
    return (FIB_FORW_CHAIN_TYPE_UNICAST_IP4);
}

/*
 * fib_path_back_walk_notify
 *
 * A back walk has reach this path.
 */
static fib_node_back_walk_rc_t
fib_path_back_walk_notify (fib_node_t *node,
			   fib_node_back_walk_ctx_t *ctx)
{
    fib_path_t *path;

    path = fib_path_from_fib_node(node);

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_RECURSIVE:
	if (FIB_NODE_BW_REASON_FLAG_EVALUATE & ctx->fnbw_reason)
	{
	    /*
	     * modify the recursive adjacency to use the new forwarding
	     * of the via-fib.
	     * this update is visible to packets in flight in the DP.
	     */
	    fib_path_recursive_adj_update(
		path,
		fib_path_proto_to_chain_type(path->fp_nh_proto),
		&path->fp_dpo);
	}
	if ((FIB_NODE_BW_REASON_FLAG_ADJ_UPDATE & ctx->fnbw_reason) ||
            (FIB_NODE_BW_REASON_FLAG_ADJ_DOWN   & ctx->fnbw_reason))
	{
	    /*
	     * ADJ updates (complete<->incomplete) do not need to propagate to
	     * recursive entries.
	     * The only reason its needed as far back as here, is that the adj
	     * and the incomplete adj are a different DPO type, so the LBs need
	     * to re-stack.
	     * If this walk was quashed in the fib_entry, then any non-fib_path
	     * children (like tunnels that collapse out the LB when they stack)
	     * would not see the update.
	     */
	    return (FIB_NODE_BACK_WALK_CONTINUE);
	}
	break;
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	/*
FIXME comment
	 * ADJ_UPDATE backwalk pass silently through here and up to
	 * the path-list when the multipath adj collapse occurs.
	 * The reason we do this is that the assumtption is that VPP
	 * runs in an environment where the Control-Plane is remote
	 * and hence reacts slowly to link up down. In order to remove
	 * this down link from the ECMP set quickly, we back-walk.
	 * VPP also has dedicated CPUs, so we are not stealing resources
	 * from the CP to do so.
	 */
	if (FIB_NODE_BW_REASON_FLAG_INTERFACE_UP & ctx->fnbw_reason)
	{
            if (path->fp_oper_flags & FIB_PATH_OPER_FLAG_RESOLVED)
            {
                /*
                 * alreday resolved. no need to walk back again
                 */
                return (FIB_NODE_BACK_WALK_CONTINUE);
            }
	    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RESOLVED;
	}
	if (FIB_NODE_BW_REASON_FLAG_INTERFACE_DOWN & ctx->fnbw_reason)
	{
            if (!(path->fp_oper_flags & FIB_PATH_OPER_FLAG_RESOLVED))
            {
                /*
                 * alreday unresolved. no need to walk back again
                 */
                return (FIB_NODE_BACK_WALK_CONTINUE);
            }
	    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
	}
	if (FIB_NODE_BW_REASON_FLAG_INTERFACE_DELETE & ctx->fnbw_reason)
	{
	    /*
	     * The interface this path resolves through has been deleted.
	     * This will leave the path in a permanent drop state. The route
	     * needs to be removed and readded (and hence the path-list deleted)
	     * before it can forward again.
	     */
	    fib_path_unresolve(path);
	    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_DROP;
	}
        if (FIB_NODE_BW_REASON_FLAG_ADJ_UPDATE & ctx->fnbw_reason)
	{
            /*
             * restack the DPO to pick up the correct DPO sub-type
             */
            uword if_is_up;
            adj_index_t ai;

            if_is_up = vnet_sw_interface_is_admin_up(
                           vnet_get_main(),
                           path->attached_next_hop.fp_interface);

            if (if_is_up)
            {
                path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RESOLVED;
            }

            ai = fib_path_attached_next_hop_get_adj(
                     path,
                     fib_proto_to_link(path->fp_nh_proto));

            dpo_set(&path->fp_dpo, DPO_ADJACENCY,
                    fib_proto_to_dpo(path->fp_nh_proto),
                    ai);
            adj_unlock(ai);

            if (!if_is_up)
            {
                /*
                 * If the interface is not up there is no reason to walk
                 * back to children. if we did they would only evalute
                 * that this path is unresolved and hence it would
                 * not contribute the adjacency - so it would be wasted
                 * CPU time.
                 */
                return (FIB_NODE_BACK_WALK_CONTINUE);
            }
        }
        if (FIB_NODE_BW_REASON_FLAG_ADJ_DOWN & ctx->fnbw_reason)
	{
            if (!(path->fp_oper_flags & FIB_PATH_OPER_FLAG_RESOLVED))
            {
                /*
                 * alreday unresolved. no need to walk back again
                 */
                return (FIB_NODE_BACK_WALK_CONTINUE);
            }
            /*
             * the adj has gone down. the path is no longer resolved.
             */
	    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
        }
	break;
    case FIB_PATH_TYPE_ATTACHED:
	/*
	 * FIXME; this could schedule a lower priority walk, since attached
	 * routes are not usually in ECMP configurations so the backwalk to
	 * the FIB entry does not need to be high priority
	 */
	if (FIB_NODE_BW_REASON_FLAG_INTERFACE_UP & ctx->fnbw_reason)
	{
	    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RESOLVED;
	}
	if (FIB_NODE_BW_REASON_FLAG_INTERFACE_DOWN & ctx->fnbw_reason)
	{
	    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
	}
	if (FIB_NODE_BW_REASON_FLAG_INTERFACE_DELETE & ctx->fnbw_reason)
	{
	    fib_path_unresolve(path);
	    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_DROP;
	}
	break;
    case FIB_PATH_TYPE_DEAG:
	/*
	 * FIXME When VRF delete is allowed this will need a poke.
	 */
    case FIB_PATH_TYPE_SPECIAL:
    case FIB_PATH_TYPE_RECEIVE:
    case FIB_PATH_TYPE_EXCLUSIVE:
	/*
	 * these path types have no parents. so to be
	 * walked from one is unexpected.
	 */
	ASSERT(0);
	break;
    }

    /*
     * propagate the backwalk further to the path-list
     */
    fib_path_list_back_walk(path->fp_pl_index, ctx);

    return (FIB_NODE_BACK_WALK_CONTINUE);
}

static void
fib_path_memory_show (void)
{
    fib_show_memory_usage("Path",
			  pool_elts(fib_path_pool),
			  pool_len(fib_path_pool),
			  sizeof(fib_path_t));
}

/*
 * The FIB path's graph node virtual function table
 */
static const fib_node_vft_t fib_path_vft = {
    .fnv_get = fib_path_get_node,
    .fnv_last_lock = fib_path_last_lock_gone,
    .fnv_back_walk = fib_path_back_walk_notify,
    .fnv_mem_show = fib_path_memory_show,
};

static fib_path_cfg_flags_t
fib_path_route_flags_to_cfg_flags (const fib_route_path_t *rpath)
{
    fib_path_cfg_flags_t cfg_flags = FIB_PATH_CFG_FLAG_NONE;

    if (rpath->frp_flags & FIB_ROUTE_PATH_RESOLVE_VIA_HOST)
	cfg_flags |= FIB_PATH_CFG_FLAG_RESOLVE_HOST;
    if (rpath->frp_flags & FIB_ROUTE_PATH_RESOLVE_VIA_ATTACHED)
	cfg_flags |= FIB_PATH_CFG_FLAG_RESOLVE_ATTACHED;

    return (cfg_flags);
}

/*
 * fib_path_create
 *
 * Create and initialise a new path object.
 * return the index of the path.
 */
fib_node_index_t
fib_path_create (fib_node_index_t pl_index,
		 fib_protocol_t nh_proto,
		 fib_path_cfg_flags_t flags,
		 const fib_route_path_t *rpath)
{
    fib_path_t *path;

    pool_get(fib_path_pool, path);
    memset(path, 0, sizeof(*path));

    fib_node_init(&path->fp_node,
		  FIB_NODE_TYPE_PATH);

    dpo_reset(&path->fp_dpo);
    path->fp_pl_index = pl_index;
    path->fp_nh_proto = nh_proto;
    path->fp_via_fib = FIB_NODE_INDEX_INVALID;
    path->fp_weight = rpath->frp_weight;
    if (0 == path->fp_weight)
    {
        /*
         * a weight of 0 is a meaningless value. We could either reject it, and thus force
         * clients to always use 1, or we can accept it and fixup approrpiately.
         */
        path->fp_weight = 1;
    }
    path->fp_cfg_flags = flags;
    path->fp_cfg_flags |= fib_path_route_flags_to_cfg_flags(rpath);

    /*
     * deduce the path's tpye from the parementers and save what is needed.
     */
    if (~0 != rpath->frp_sw_if_index)
    {
	if (flags & FIB_PATH_CFG_FLAG_LOCAL)
	{
	    path->fp_type = FIB_PATH_TYPE_RECEIVE;
	    path->receive.fp_interface = rpath->frp_sw_if_index;
            path->receive.fp_addr = rpath->frp_addr;
	}
	else
	{
	    if (ip46_address_is_zero(&rpath->frp_addr))
	    {
		path->fp_type = FIB_PATH_TYPE_ATTACHED;
		path->attached.fp_interface = rpath->frp_sw_if_index;
	    }
	    else
	    {
		path->fp_type = FIB_PATH_TYPE_ATTACHED_NEXT_HOP;
		path->attached_next_hop.fp_interface = rpath->frp_sw_if_index;
		path->attached_next_hop.fp_nh = rpath->frp_addr;
	    }
	}
    }
    else
    {
	if (ip46_address_is_zero(&rpath->frp_addr))
	{
	    if (~0 == rpath->frp_fib_index)
	    {
		path->fp_type = FIB_PATH_TYPE_SPECIAL;
	    }
	    else
	    {
		path->fp_type = FIB_PATH_TYPE_DEAG;
		path->deag.fp_tbl_id = rpath->frp_fib_index;
	    }		
	}
	else
	{
	    path->fp_type = FIB_PATH_TYPE_RECURSIVE;
	    if (FIB_PROTOCOL_MPLS == path->fp_nh_proto)
	    {
		path->recursive.fp_nh.fp_local_label = rpath->frp_local_label;
	    }
	    else
	    {
		path->recursive.fp_nh.fp_ip = rpath->frp_addr;
	    }
	    path->recursive.fp_tbl_id = rpath->frp_fib_index;
	}
    }

    FIB_PATH_DBG(path, "create");

    return (fib_path_get_index(path));
}

/*
 * fib_path_create_special
 *
 * Create and initialise a new path object.
 * return the index of the path.
 */
fib_node_index_t
fib_path_create_special (fib_node_index_t pl_index,
			 fib_protocol_t nh_proto,
			 fib_path_cfg_flags_t flags,
			 const dpo_id_t *dpo)
{
    fib_path_t *path;

    pool_get(fib_path_pool, path);
    memset(path, 0, sizeof(*path));

    fib_node_init(&path->fp_node,
		  FIB_NODE_TYPE_PATH);
    dpo_reset(&path->fp_dpo);

    path->fp_pl_index = pl_index;
    path->fp_weight = 1;
    path->fp_nh_proto = nh_proto;
    path->fp_via_fib = FIB_NODE_INDEX_INVALID;
    path->fp_cfg_flags = flags;

    if (FIB_PATH_CFG_FLAG_DROP & flags)
    {
	path->fp_type = FIB_PATH_TYPE_SPECIAL;
    }
    else if (FIB_PATH_CFG_FLAG_LOCAL & flags)
    {
	path->fp_type = FIB_PATH_TYPE_RECEIVE;
	path->attached.fp_interface = FIB_NODE_INDEX_INVALID;
    }
    else
    {
	path->fp_type = FIB_PATH_TYPE_EXCLUSIVE;
	ASSERT(NULL != dpo);
	dpo_copy(&path->exclusive.fp_ex_dpo, dpo);
    }

    return (fib_path_get_index(path));
}

/*
 * fib_path_copy
 *
 * Copy a path. return index of new path.
 */
fib_node_index_t
fib_path_copy (fib_node_index_t path_index,
	       fib_node_index_t path_list_index)
{
    fib_path_t *path, *orig_path;

    pool_get(fib_path_pool, path);

    orig_path = fib_path_get(path_index);
    ASSERT(NULL != orig_path);

    memcpy(path, orig_path, sizeof(*path));

    FIB_PATH_DBG(path, "create-copy:%d", path_index);

    /*
     * reset the dynamic section
     */
    fib_node_init(&path->fp_node, FIB_NODE_TYPE_PATH);
    path->fp_oper_flags     = FIB_PATH_OPER_FLAG_NONE;
    path->fp_pl_index  = path_list_index;
    path->fp_via_fib   = FIB_NODE_INDEX_INVALID;
    memset(&path->fp_dpo, 0, sizeof(path->fp_dpo));
    dpo_reset(&path->fp_dpo);

    return (fib_path_get_index(path));
}

/*
 * fib_path_destroy
 *
 * destroy a path that is no longer required
 */
void
fib_path_destroy (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    ASSERT(NULL != path);
    FIB_PATH_DBG(path, "destroy");

    fib_path_unresolve(path);

    fib_node_deinit(&path->fp_node);
    pool_put(fib_path_pool, path);
}

/*
 * fib_path_destroy
 *
 * destroy a path that is no longer required
 */
uword
fib_path_hash (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (hash_memory(STRUCT_MARK_PTR(path, path_hash_start),
			(STRUCT_OFFSET_OF(fib_path_t, path_hash_end) -
			 STRUCT_OFFSET_OF(fib_path_t, path_hash_start)),
			0));
}

/*
 * fib_path_cmp_i
 *
 * Compare two paths for equivalence.
 */
static int
fib_path_cmp_i (const fib_path_t *path1,
		const fib_path_t *path2)
{
    int res;

    res = 1;

    /*
     * paths of different types and protocol are not equal.
     * different weights only are the same path.
     */
    if (path1->fp_type != path2->fp_type)
    {
	res = (path1->fp_type - path2->fp_type);
    }
    if (path1->fp_nh_proto != path2->fp_nh_proto)
    {
	res = (path1->fp_nh_proto - path2->fp_nh_proto);
    }
    else
    {
	/*
	 * both paths are of the same type.
	 * consider each type and its attributes in turn.
	 */
	switch (path1->fp_type)
	{
	case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	    res = ip46_address_cmp(&path1->attached_next_hop.fp_nh,
				   &path2->attached_next_hop.fp_nh);
	    if (0 == res) {
		res = vnet_sw_interface_compare(
			  vnet_get_main(),
			  path1->attached_next_hop.fp_interface,
			  path2->attached_next_hop.fp_interface);
	    }
	    break;
	case FIB_PATH_TYPE_ATTACHED:
	    res = vnet_sw_interface_compare(
		      vnet_get_main(),
		      path1->attached.fp_interface,
		      path2->attached.fp_interface);
	    break;
	case FIB_PATH_TYPE_RECURSIVE:
	    res = ip46_address_cmp(&path1->recursive.fp_nh,
				   &path2->recursive.fp_nh);
 
	    if (0 == res)
	    {
		res = (path1->recursive.fp_tbl_id - path2->recursive.fp_tbl_id);
	    }
	    break;
	case FIB_PATH_TYPE_DEAG:
	    res = (path1->deag.fp_tbl_id - path2->deag.fp_tbl_id);
	    break;
	case FIB_PATH_TYPE_SPECIAL:
	case FIB_PATH_TYPE_RECEIVE:
	case FIB_PATH_TYPE_EXCLUSIVE:
	    res = 0;
	    break;
	}
    }
    return (res);
}

/*
 * fib_path_cmp_for_sort
 *
 * Compare two paths for equivalence. Used during path sorting.
 * As usual 0 means equal.
 */
int
fib_path_cmp_for_sort (void * v1,
		       void * v2)
{
    fib_node_index_t *pi1 = v1, *pi2 = v2;
    fib_path_t *path1, *path2;

    path1 = fib_path_get(*pi1);
    path2 = fib_path_get(*pi2);

    return (fib_path_cmp_i(path1, path2));
}

/*
 * fib_path_cmp
 *
 * Compare two paths for equivalence.
 */
int
fib_path_cmp (fib_node_index_t pi1,
	      fib_node_index_t pi2)
{
    fib_path_t *path1, *path2;

    path1 = fib_path_get(pi1);
    path2 = fib_path_get(pi2);

    return (fib_path_cmp_i(path1, path2));
}

int
fib_path_cmp_w_route_path (fib_node_index_t path_index,
			   const fib_route_path_t *rpath)
{
    fib_path_t *path;
    int res;

    path = fib_path_get(path_index);

    res = 1;

    if (path->fp_weight != rpath->frp_weight)
    {
	res = (path->fp_weight - rpath->frp_weight);
    }
    else
    {
	/*
	 * both paths are of the same type.
	 * consider each type and its attributes in turn.
	 */
	switch (path->fp_type)
	{
	case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	    res = ip46_address_cmp(&path->attached_next_hop.fp_nh,
				   &rpath->frp_addr);
	    if (0 == res)
	    {
		res = vnet_sw_interface_compare(
			  vnet_get_main(),
			  path->attached_next_hop.fp_interface,
			  rpath->frp_sw_if_index);
	    }
	    break;
	case FIB_PATH_TYPE_ATTACHED:
	    res = vnet_sw_interface_compare(
		      vnet_get_main(),
		      path->attached.fp_interface,
		      rpath->frp_sw_if_index);
	    break;
	case FIB_PATH_TYPE_RECURSIVE:
            if (FIB_PROTOCOL_MPLS == path->fp_nh_proto)
            {
                res = path->recursive.fp_nh.fp_local_label - rpath->frp_local_label;
            }
            else
            {
                res = ip46_address_cmp(&path->recursive.fp_nh.fp_ip,
                                       &rpath->frp_addr);
            }

            if (0 == res)
            {
                res = (path->recursive.fp_tbl_id - rpath->frp_fib_index);
            }
	    break;
	case FIB_PATH_TYPE_DEAG:
	    res = (path->deag.fp_tbl_id - rpath->frp_fib_index);
	    break;
	case FIB_PATH_TYPE_SPECIAL:
	case FIB_PATH_TYPE_RECEIVE:
	case FIB_PATH_TYPE_EXCLUSIVE:
	    res = 0;
	    break;
	}
    }
    return (res);
}

/*
 * fib_path_recursive_loop_detect
 *
 * A forward walk of the FIB object graph to detect for a cycle/loop. This
 * walk is initiated when an entry is linking to a new path list or from an old.
 * The entry vector passed contains all the FIB entrys that are children of this
 * path (it is all the entries encountered on the walk so far). If this vector
 * contains the entry this path resolve via, then a loop is about to form.
 * The loop must be allowed to form, since we need the dependencies in place
 * so that we can track when the loop breaks.
 * However, we MUST not produce a loop in the forwarding graph (else packets
 * would loop around the switch path until the loop breaks), so we mark recursive
 * paths as looped so that they do not contribute forwarding information.
 * By marking the path as looped, an etry such as;
 *    X/Y
 *     via a.a.a.a (looped)
 *     via b.b.b.b (not looped)
 * can still forward using the info provided by b.b.b.b only
 */
int
fib_path_recursive_loop_detect (fib_node_index_t path_index,
				fib_node_index_t **entry_indicies)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    /*
     * the forced drop path is never looped, cos it is never resolved.
     */
    if (fib_path_is_permanent_drop(path))
    {
	return (0);
    }

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_RECURSIVE:
    {
	fib_node_index_t *entry_index, *entries;
	int looped = 0;
	entries = *entry_indicies;

	vec_foreach(entry_index, entries) {
	    if (*entry_index == path->fp_via_fib)
	    {
		/*
		 * the entry that is about to link to this path-list (or
		 * one of this path-list's children) is the same entry that
		 * this recursive path resolves through. this is a cycle.
		 * abort the walk.
		 */
		looped = 1;
		break;
	    }
	}

	if (looped)
	{
	    FIB_PATH_DBG(path, "recursive loop formed");
	    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RECURSIVE_LOOP;

	    dpo_copy(&path->fp_dpo,
                    drop_dpo_get(fib_proto_to_dpo(path->fp_nh_proto)));
	}
	else
	{
	    /*
	     * no loop here yet. keep forward walking the graph.
	     */	    
	    if (fib_entry_recursive_loop_detect(path->fp_via_fib, entry_indicies))
	    {
		FIB_PATH_DBG(path, "recursive loop formed");
		path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RECURSIVE_LOOP;
	    }
	    else
	    {
		FIB_PATH_DBG(path, "recursive loop cleared");
		path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RECURSIVE_LOOP;
	    }
	}
	break;
    }
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
    case FIB_PATH_TYPE_ATTACHED:
    case FIB_PATH_TYPE_SPECIAL:
    case FIB_PATH_TYPE_DEAG:
    case FIB_PATH_TYPE_RECEIVE:
    case FIB_PATH_TYPE_EXCLUSIVE:
	/*
	 * these path types cannot be part of a loop, since they are the leaves
	 * of the graph.
	 */
	break;
    }

    return (fib_path_is_looped(path_index));
}

int
fib_path_resolve (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    /*
     * hope for the best.
     */
    path->fp_oper_flags |= FIB_PATH_OPER_FLAG_RESOLVED;

    /*
     * the forced drop path resolves via the drop adj
     */
    if (fib_path_is_permanent_drop(path))
    {
	dpo_copy(&path->fp_dpo,
                 drop_dpo_get(fib_proto_to_dpo(path->fp_nh_proto)));
	path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
	return (fib_path_is_resolved(path_index));
    }

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	fib_path_attached_next_hop_set(path);
	break;
    case FIB_PATH_TYPE_ATTACHED:
	/*
	 * path->attached.fp_interface
	 */
	if (!vnet_sw_interface_is_admin_up(vnet_get_main(),
					   path->attached.fp_interface))
	{
	    path->fp_oper_flags &= ~FIB_PATH_OPER_FLAG_RESOLVED;
	}
	if (vnet_sw_interface_is_p2p(vnet_get_main(),
				     path->attached.fp_interface))
	{
	    /*
	     * point-2-point interfaces do not require a glean, since
	     * there is nothing to ARP. Install a rewrite/nbr adj instead
	     */
	    dpo_set(&path->fp_dpo,
		    DPO_ADJACENCY,
		    fib_proto_to_dpo(path->fp_nh_proto),
		    adj_nbr_add_or_lock(
			path->fp_nh_proto,
			fib_proto_to_link(path->fp_nh_proto),
			&zero_addr,
			path->attached.fp_interface));
	}
	else
	{
	    dpo_set(&path->fp_dpo,
		    DPO_ADJACENCY_GLEAN,
		    fib_proto_to_dpo(path->fp_nh_proto),
		    adj_glean_add_or_lock(path->fp_nh_proto,
					  path->attached.fp_interface,
					  NULL));
	}
	/*
	 * become a child of the adjacency so we receive updates
	 * when the interface state changes
	 */
	path->fp_sibling = adj_child_add(path->fp_dpo.dpoi_index,
					 FIB_NODE_TYPE_PATH,
					 fib_path_get_index(path));

	break;
    case FIB_PATH_TYPE_RECURSIVE:
    {
	/*
	 * Create a RR source entry in the table for the address
	 * that this path recurses through.
	 * This resolve action is recursive, hence we may create
	 * more paths in the process. more creates mean maybe realloc
	 * of this path.
	 */
	fib_node_index_t fei;
	fib_prefix_t pfx;

	ASSERT(FIB_NODE_INDEX_INVALID == path->fp_via_fib);

	if (FIB_PROTOCOL_MPLS == path->fp_nh_proto)
	{
	    fib_prefix_from_mpls_label(path->recursive.fp_nh.fp_local_label, &pfx);
	}
	else
	{
	    fib_prefix_from_ip46_addr(&path->recursive.fp_nh.fp_ip, &pfx);
	}

	fei = fib_table_entry_special_add(path->recursive.fp_tbl_id,
					  &pfx,
					  FIB_SOURCE_RR,
					  FIB_ENTRY_FLAG_NONE,
					  ADJ_INDEX_INVALID);

	path = fib_path_get(path_index);
	path->fp_via_fib = fei;

	/*
	 * become a dependent child of the entry so the path is 
	 * informed when the forwarding for the entry changes.
	 */
	path->fp_sibling = fib_entry_child_add(path->fp_via_fib,
					       FIB_NODE_TYPE_PATH,
					       fib_path_get_index(path));

	/*
	 * create and configure the IP DPO
	 */
	fib_path_recursive_adj_update(
	    path,
	    fib_path_proto_to_chain_type(path->fp_nh_proto),
	    &path->fp_dpo);

	break;
    }
    case FIB_PATH_TYPE_SPECIAL:
	/*
	 * Resolve via the drop
	 */
	dpo_copy(&path->fp_dpo,
                 drop_dpo_get(fib_proto_to_dpo(path->fp_nh_proto)));
	break;
    case FIB_PATH_TYPE_DEAG:
	/*
	 * Resolve via a lookup DPO.
         * FIXME. control plane should add routes with a table ID
	 */
	lookup_dpo_add_or_lock_w_fib_index(path->deag.fp_tbl_id,
                                          fib_proto_to_dpo(path->fp_nh_proto),
                                          LOOKUP_INPUT_DST_ADDR,
                                          LOOKUP_TABLE_FROM_CONFIG,
                                          &path->fp_dpo);
	break;
    case FIB_PATH_TYPE_RECEIVE:
	/*
	 * Resolve via a receive DPO.
	 */
	receive_dpo_add_or_lock(fib_proto_to_dpo(path->fp_nh_proto),
                                path->receive.fp_interface,
                                &path->receive.fp_addr,
                                &path->fp_dpo);
	break;
    case FIB_PATH_TYPE_EXCLUSIVE:
	/*
	 * Resolve via the user provided DPO
	 */
	dpo_copy(&path->fp_dpo, &path->exclusive.fp_ex_dpo);
	break;
    }

    return (fib_path_is_resolved(path_index));
}

u32
fib_path_get_resolving_interface (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	return (path->attached_next_hop.fp_interface);
    case FIB_PATH_TYPE_ATTACHED:
	return (path->attached.fp_interface);
    case FIB_PATH_TYPE_RECEIVE:
	return (path->receive.fp_interface);
    case FIB_PATH_TYPE_RECURSIVE:
	return (fib_entry_get_resolving_interface(path->fp_via_fib));    
    case FIB_PATH_TYPE_SPECIAL:
    case FIB_PATH_TYPE_DEAG:
    case FIB_PATH_TYPE_EXCLUSIVE:
	break;
    }
    return (~0);
}

adj_index_t
fib_path_get_adj (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    ASSERT(dpo_is_adj(&path->fp_dpo));
    if (dpo_is_adj(&path->fp_dpo))
    {
	return (path->fp_dpo.dpoi_index);
    }
    return (ADJ_INDEX_INVALID);
}

int
fib_path_get_weight (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    ASSERT(path);

    return (path->fp_weight);
}

/**
 * @brief Contribute the path's adjacency to the list passed.
 * By calling this function over all paths, recursively, a child
 * can construct its full set of forwarding adjacencies, and hence its
 * uRPF list.
 */
void
fib_path_contribute_urpf (fib_node_index_t path_index,
			  index_t urpf)
{
    fib_path_t *path;

    if (!fib_path_is_resolved(path_index))
	return;

    path = fib_path_get(path_index);

    switch (path->fp_type)
    {
    case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	fib_urpf_list_append(urpf, path->attached_next_hop.fp_interface);
	break;

    case FIB_PATH_TYPE_ATTACHED:
	fib_urpf_list_append(urpf, path->attached.fp_interface);
	break;

    case FIB_PATH_TYPE_RECURSIVE:
	fib_entry_contribute_urpf(path->fp_via_fib, urpf);
	break;

    case FIB_PATH_TYPE_EXCLUSIVE:
    case FIB_PATH_TYPE_SPECIAL:
	/*
	 * these path types may link to an adj, if that's what
	 * the clinet gave
	 */
	if (dpo_is_adj(&path->fp_dpo))
	{
	    ip_adjacency_t *adj;

	    adj = adj_get(path->fp_dpo.dpoi_index);

	    fib_urpf_list_append(urpf, adj->rewrite_header.sw_if_index);
	}
	break;

    case FIB_PATH_TYPE_DEAG:
    case FIB_PATH_TYPE_RECEIVE:
	/*
	 * these path types don't link to an adj
	 */
	break;
    }
}

void
fib_path_contribute_forwarding (fib_node_index_t path_index,
				fib_forward_chain_type_t fct,
				dpo_id_t *dpo)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    ASSERT(path);
    ASSERT(FIB_FORW_CHAIN_TYPE_MPLS_EOS != fct);

    FIB_PATH_DBG(path, "contribute");

    /*
     * The DPO stored in the path was created when the path was resolved.
     * This then represents the path's 'native' protocol; IP.
     * For all others will need to go find something else.
     */
    if (fib_path_proto_to_chain_type(path->fp_nh_proto) == fct)
    {
	dpo_copy(dpo, &path->fp_dpo);
    }
    else
    {
	switch (path->fp_type)
	{
	case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
	    switch (fct)
	    {
	    case FIB_FORW_CHAIN_TYPE_UNICAST_IP4:
	    case FIB_FORW_CHAIN_TYPE_UNICAST_IP6:
	    case FIB_FORW_CHAIN_TYPE_MPLS_EOS:
	    case FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS:
	    case FIB_FORW_CHAIN_TYPE_ETHERNET:
	    {
		adj_index_t ai;

		/*
		 * get a appropriate link type adj.
		 */
		ai = fib_path_attached_next_hop_get_adj(
		         path,
			 fib_forw_chain_type_to_link_type(fct));
		dpo_set(dpo, DPO_ADJACENCY,
			fib_forw_chain_type_to_dpo_proto(fct), ai);
		adj_unlock(ai);

		break;
	    }
	    }
	    break;
	case FIB_PATH_TYPE_RECURSIVE:
	    switch (fct)
	    {
	    case FIB_FORW_CHAIN_TYPE_MPLS_EOS:
	    case FIB_FORW_CHAIN_TYPE_UNICAST_IP4:
	    case FIB_FORW_CHAIN_TYPE_UNICAST_IP6:
	    case FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS:
		fib_path_recursive_adj_update(path, fct, dpo);
		break;
	    case FIB_FORW_CHAIN_TYPE_ETHERNET:
		ASSERT(0);
		break;
	    }
	    break;
	case FIB_PATH_TYPE_DEAG:
	    switch (fct)
	    {
	    case FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS:
                lookup_dpo_add_or_lock_w_table_id(MPLS_FIB_DEFAULT_TABLE_ID,
                                                  DPO_PROTO_MPLS,
                                                  LOOKUP_INPUT_DST_ADDR,
                                                  LOOKUP_TABLE_FROM_CONFIG,
                                                  dpo);
                break;
	    case FIB_FORW_CHAIN_TYPE_UNICAST_IP4:
	    case FIB_FORW_CHAIN_TYPE_UNICAST_IP6:
	    case FIB_FORW_CHAIN_TYPE_MPLS_EOS:
		dpo_copy(dpo, &path->fp_dpo);
		break;		
	    case FIB_FORW_CHAIN_TYPE_ETHERNET:
		ASSERT(0);
		break;
            }
            break;
	case FIB_PATH_TYPE_EXCLUSIVE:
	    dpo_copy(dpo, &path->exclusive.fp_ex_dpo);
	    break;
        case FIB_PATH_TYPE_ATTACHED:
	case FIB_PATH_TYPE_RECEIVE:
	case FIB_PATH_TYPE_SPECIAL:
	    ASSERT(0);
            break;
	}

    }
}

load_balance_path_t *
fib_path_append_nh_for_multipath_hash (fib_node_index_t path_index,
				       fib_forward_chain_type_t fct,
				       load_balance_path_t *hash_key)
{
    load_balance_path_t *mnh;
    fib_path_t *path;

    path = fib_path_get(path_index);

    ASSERT(path);

    if (fib_path_is_resolved(path_index))
    {
	vec_add2(hash_key, mnh, 1);

	mnh->path_weight = path->fp_weight;
	mnh->path_index = path_index;
	fib_path_contribute_forwarding(path_index, fct, &mnh->path_dpo);
    }

    return (hash_key);
}

int
fib_path_is_recursive (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (FIB_PATH_TYPE_RECURSIVE == path->fp_type);
}

int
fib_path_is_exclusive (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (FIB_PATH_TYPE_EXCLUSIVE == path->fp_type);
}

int
fib_path_is_deag (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (FIB_PATH_TYPE_DEAG == path->fp_type);
}

int
fib_path_is_resolved (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (dpo_id_is_valid(&path->fp_dpo) &&
	    (path->fp_oper_flags & FIB_PATH_OPER_FLAG_RESOLVED) &&
	    !fib_path_is_looped(path_index) &&
	    !fib_path_is_permanent_drop(path));
}

int
fib_path_is_looped (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (path->fp_oper_flags & FIB_PATH_OPER_FLAG_RECURSIVE_LOOP);
}

int
fib_path_encode (fib_node_index_t path_list_index,
		 fib_node_index_t path_index,
                 void *ctx)
{
    fib_route_path_encode_t **api_rpaths = ctx;
    fib_route_path_encode_t *api_rpath;
    fib_path_t *path;

    path = fib_path_get(path_index);
    if (!path)
      return (0);
    vec_add2(*api_rpaths, api_rpath, 1);
    api_rpath->rpath.frp_weight = path->fp_weight;
    api_rpath->rpath.frp_proto = path->fp_nh_proto;
    api_rpath->rpath.frp_sw_if_index = ~0;
    api_rpath->dpo = path->exclusive.fp_ex_dpo;
    switch (path->fp_type)
      {
      case FIB_PATH_TYPE_RECEIVE:
        api_rpath->rpath.frp_addr = path->receive.fp_addr;
        api_rpath->rpath.frp_sw_if_index = path->receive.fp_interface;
        break;
      case FIB_PATH_TYPE_ATTACHED:
        api_rpath->rpath.frp_sw_if_index = path->attached.fp_interface;
        break;
      case FIB_PATH_TYPE_ATTACHED_NEXT_HOP:
        api_rpath->rpath.frp_sw_if_index = path->attached_next_hop.fp_interface;
        api_rpath->rpath.frp_addr = path->attached_next_hop.fp_nh;
        break;
      case FIB_PATH_TYPE_SPECIAL:
        break;
      case FIB_PATH_TYPE_DEAG:
        break;
      case FIB_PATH_TYPE_RECURSIVE:
        api_rpath->rpath.frp_addr = path->recursive.fp_nh.fp_ip;
        break;
      default:
        break;
      }
    return (1);
}

fib_protocol_t
fib_path_get_proto (fib_node_index_t path_index)
{
    fib_path_t *path;

    path = fib_path_get(path_index);

    return (path->fp_nh_proto);
}

void
fib_path_module_init (void)
{
    fib_node_register_type (FIB_NODE_TYPE_PATH, &fib_path_vft);
}

static clib_error_t *
show_fib_path_command (vlib_main_t * vm,
			unformat_input_t * input,
			vlib_cli_command_t * cmd)
{
    fib_node_index_t pi;
    fib_path_t *path;

    if (unformat (input, "%d", &pi))
    {
	/*
	 * show one in detail
	 */
	if (!pool_is_free_index(fib_path_pool, pi))
	{
	    path = fib_path_get(pi);
	    u8 *s = fib_path_format(pi, NULL);
	    s = format(s, "children:");
	    s = fib_node_children_format(path->fp_node.fn_children, s);
	    vlib_cli_output (vm, "%s", s);
	    vec_free(s);
	}
	else
	{
	    vlib_cli_output (vm, "path %d invalid", pi);
	}
    }
    else
    {
	vlib_cli_output (vm, "FIB Paths");
	pool_foreach(path, fib_path_pool,
	({
	    vlib_cli_output (vm, "%U", format_fib_path, path);
	}));
    }

    return (NULL);
}

VLIB_CLI_COMMAND (show_fib_path, static) = {
  .path = "show fib paths",
  .function = show_fib_path_command,
  .short_help = "show fib paths",
};
