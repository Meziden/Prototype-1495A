#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <nc_server.h>
#include "rpc_callbacks.h"

/* Global Libyang Context Pointer */
extern struct ly_ctx* ctx;

/* Global Pollsession Pointers */
extern struct nc_pollsession* g_pollsession;

/* Global Datastore Pointer, maintained by fdmonitor thread. */
extern struct lyd_node* g_node_running;
extern struct lyd_node* g_node_candidate;
extern struct lyd_node* g_node_state;

/* Global Datastore Access Control */
extern pthread_mutex_t g_sidmutex_running;
extern volatile uint32_t g_sid_running;
extern pthread_mutex_t g_sidmutex_candidate;
extern volatile uint32_t g_sid_candidate;
/* File Sync Flags */
bool syncflag_running = 0;
bool syncflag_candidate = 0;

struct nc_server_reply* rpc_callback_get(struct lyd_node* rpc,struct nc_session *session)
{
	printf("<get>/<get-config> RPC Received.\n");
	
	/* Duplicate the <rpc> data, directly get the correct XPath in schema. */
	struct lyd_node* data = lyd_dup(rpc, LYD_DUP_OPT_RECURSIVE);
	struct lyd_node* source_data = NULL;
	struct lyd_node* data_state = NULL;
	
	/* Add state data for <get> operation. */
	if(!strcmp(rpc->schema->name, "get"))
	{
		source_data = lyd_dup(g_node_running,LYD_DUP_OPT_RECURSIVE);
		data_state = lyd_dup(g_node_state,LYD_DUP_OPT_RECURSIVE);
		/* Combine Return Data. */
		lyd_insert_after(source_data, data_state);
	}
	/* Choose correct datastore for <get-config> operation. */
	else
	{
		struct ly_set* nodeset = lyd_find_path(rpc, "/ietf-netconf:get-config/source/*");
		if (!strcmp(nodeset->set.d[0]->schema->name, "running"))
			source_data = lyd_dup(g_node_running,LYD_DUP_OPT_RECURSIVE);
		else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate"))
			source_data = lyd_dup(g_node_candidate,LYD_DUP_OPT_RECURSIVE);
		//else if (!strcmp(nodeset->set.d[0]->schema->name, "startup"))
		//	source_data = NULL;
		else
			printf("[RPC Handler] <get-config> Unexpected datastore source.");	
		ly_set_free(nodeset);
	}
	
	/* TODO YANG Data Instance Filter */
	
	/* Link the data node to the <rpc-reply> YANG Data Instance. */
	lyd_new_output_anydata(data, NULL, "data", source_data, LYD_ANYDATA_DATATREE);
	lyd_validate(&data, LYD_OPT_RPCREPLY, NULL);
	
	/* Send <rpc-reply> */
	return nc_server_reply_data(data, NC_WD_ALL, NC_PARAMTYPE_FREE);
}

struct nc_server_reply* rpc_callback_edit(struct lyd_node* rpc, struct nc_session *session)
{
	printf("<edit-config> RPC Received.\n");
	/* TODO Too many complicated operations. */
	return nc_server_reply_ok();
}

struct nc_server_reply* rpc_callback_copy(struct lyd_node* rpc, struct nc_session *session)
{
	printf("<copy-config> RPC Received.\n");
	struct lyd_node* source_data = NULL;
	struct lyd_node* target_node = NULL;
	struct ly_set* nodeset = NULL;
	
	/* Processing target argument, check permission */
	nodeset = lyd_find_path(rpc, "/ietf-netconf:copy-config/target/*");
	if (!strcmp(nodeset->set.d[0]->schema->name, "running"))
	{
		/* Keep holding sid 0 unchanged until write operation complete. */
		pthread_mutex_lock(&g_sidmutex_running);
		if(g_sid_running == 0)
		{
			// Do NOT unlock here or the write operation may be interrupted.
			target_node = g_node_running;
			syncflag_running = 1;
		}
		else
		{
			pthread_mutex_unlock(&g_sidmutex_running);
			return nc_server_reply_err(nc_err(NC_ERR_LOCK_DENIED, g_sid_running));
		}
	}
	else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate"))
	{
		pthread_mutex_lock(&g_sidmutex_candidate);
		if(g_sid_candidate == 0)
		{
			// Do NOT unlock here or the write operation may be interrupted.
			target_node = g_node_candidate;
			syncflag_candidate = 1;
		}
		else
		{
			pthread_mutex_unlock(&g_sidmutex_candidate);
			return nc_server_reply_err(nc_err(NC_ERR_LOCK_DENIED, g_sid_candidate));
		}
	}
	else
		printf("[RPC Handler] <copy-config> Unexpected <target>.\n");	
	ly_set_free(nodeset);
	
	/* Processing source argument */
	nodeset = lyd_find_path(rpc, "/ietf-netconf:copy-config/source/*");
	if (!strcmp(nodeset->set.d[0]->schema->name, "running"))
	{
		if(target_node == g_node_running)
			return nc_server_reply_ok();
		else
			source_data = lyd_dup(g_node_running, LYD_DUP_OPT_RECURSIVE);
	}
	else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate"))
	{
		if(target_node == g_node_candidate)
			return nc_server_reply_ok();
		else
			source_data = lyd_dup(g_node_candidate, LYD_DUP_OPT_RECURSIVE);
	}
	else if (!strcmp(nodeset->set.d[0]->schema->name, "config"))
	{	
		/* Get struct lyd_node_anydata */
		struct lyd_node_anydata* anydata = (struct lyd_node_anydata*)nodeset->set.d[0];
		
		/* Reconstruct YANG Data Instance node, to get correct YANG Schema node */
		if(anydata -> value_type == LYD_ANYDATA_XML)
			source_data = lyd_parse_xml(ctx, &anydata->value.xml, LYD_OPT_CONFIG);
		
	}
	else
		printf("[RPC Handler] <copy-config> Unexpected <source>.\n");	
	ly_set_free(nodeset);
	
	
	/* Merge Configuration */
	lyd_merge(target_node, source_data, LYD_OPT_EXPLICIT);
	
	/* Synchronizing Configuration Files */
	if(syncflag_running)
	{
		syncflag_running = 0;
		lyd_print_path("./configs/userconfig.xml", g_node_running, LYD_XML, LYP_FORMAT);
		pthread_mutex_unlock(&g_sidmutex_running);
	}
	if(syncflag_candidate)
	{
		syncflag_candidate = 0;
		lyd_print_path("./configs/userconfig_candidate.xml", g_node_candidate, LYD_XML, LYP_FORMAT);
		pthread_mutex_unlock(&g_sidmutex_candidate);
	}
	
	/* Use lyd_free_withsiblings here, 'cause additional info is added in config. */
	lyd_free_withsiblings(source_data);
	return nc_server_reply_ok();
}

struct nc_server_reply* rpc_callback_delete(struct lyd_node* rpc, struct nc_session *session)
{
	printf("<delete-config> RPC Received.\n");
	
	return nc_server_reply_ok();
}

struct nc_server_reply* rpc_callback_lock(struct lyd_node* rpc,struct nc_session *session)
{
	printf("<lock> RPC Received.\n");
	
	/* Processing target argument, check permission */
	struct ly_set* nodeset = lyd_find_path(rpc, "/ietf-netconf:lock/target/*");
	if (!strcmp(nodeset->set.d[0]->schema->name, "running"))
	{
		ly_set_free(nodeset);
		
		/* Lock the datastore sid mutex. */
		pthread_mutex_lock(&g_sidmutex_running);
		if(g_sid_running == 0)
		{
			g_sid_running = nc_session_get_id(session);
			pthread_mutex_unlock(&g_sidmutex_running);
			return nc_server_reply_ok();
		}
		else
		{
			pthread_mutex_unlock(&g_sidmutex_running);
			return nc_server_reply_err(nc_err(NC_ERR_LOCK_DENIED, g_sid_running));
		}
	}
	else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate"))
	{
		ly_set_free(nodeset);
		
		/* Lock the datastore sid mutex. */
		pthread_mutex_lock(&g_sidmutex_candidate);
		if(g_sid_candidate == 0)
		{
			g_sid_candidate = nc_session_get_id(session);
			pthread_mutex_unlock(&g_sidmutex_candidate);
			return nc_server_reply_ok();
		}
		else
		{
			pthread_mutex_unlock(&g_sidmutex_candidate);
			return nc_server_reply_err(nc_err(NC_ERR_LOCK_DENIED, g_sid_candidate));
		}
	}
	else
		printf("[RPC Handler] <lock> Unexpected <target>.\n");	
	ly_set_free(nodeset);
	return nc_server_reply_err(nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP));
}

struct nc_server_reply* rpc_callback_unlock(struct lyd_node* rpc,struct nc_session *session)
{
	printf("<unlock> RPC Received.\n");
	
	/* Processing target argument, check permission */
	struct ly_set* nodeset = lyd_find_path(rpc, "/ietf-netconf:unlock/target/*");
	if (!strcmp(nodeset->set.d[0]->schema->name, "running"))
	{
		ly_set_free(nodeset);
		pthread_mutex_lock(&g_sidmutex_running);
		if(g_sid_running == nc_session_get_id(session))
		{
			g_sid_running = 0;
			pthread_mutex_unlock(&g_sidmutex_running);
			return nc_server_reply_ok();
		}
		else
		{
			struct nc_server_error* e;
			if(!g_sid_running)
			{
				e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
		    	nc_err_set_msg(e, "[RPC Handler] The <running> lock is not active.", "en");
		    }
		    else
		    {
		    	e = nc_err(NC_ERR_LOCK_DENIED, g_sid_running);
		    }
			pthread_mutex_unlock(&g_sidmutex_running);
		    return nc_server_reply_err(e);
		}
	}
	else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate"))
	{
		ly_set_free(nodeset);
		pthread_mutex_lock(&g_sidmutex_candidate);
		if(g_sid_candidate == nc_session_get_id(session))
		{
			g_sid_candidate = 0;
			pthread_mutex_unlock(&g_sidmutex_candidate);
			return nc_server_reply_ok();
		}
		else
		{
			struct nc_server_error* e;
			if(!g_sid_candidate)
			{
				e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
		    	nc_err_set_msg(e, "[RPC Handler] The <candidate> lock is not active.", "en");
		    }
		    else
		    {
		    	e = nc_err(NC_ERR_LOCK_DENIED, g_sid_candidate);
		    }
			pthread_mutex_unlock(&g_sidmutex_candidate);
		    return nc_server_reply_err(e);
		}
	}
	else
		printf("[RPC Handler] <lock> Unexpected <target>.\n");	
	ly_set_free(nodeset);
	return nc_server_reply_err(nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP));
}

/* disconnect command.
struct nc_server_reply* rpc_callback_close(struct lyd_node* rpc, struct nc_session *session)
{
	nc_session_set_status(session, NC_STATUS_INVALID);
    nc_session_set_term_reason(session, NC_SESSION_TERM_CLOSED);

    return nc_server_reply_ok();
}*/

struct nc_server_reply* rpc_callback_kill(struct lyd_node* rpc, struct nc_session *session)
{
	struct ly_set* nodeset = lyd_find_path(rpc, "session-id");
	if (!nodeset || (nodeset->number != 1) || (nodeset->set.d[0]->schema->nodetype != LYS_LEAF))
	{
        struct nc_server_error* e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, "[RPC Handler] Invalid Argument.", "en");
        return nc_server_reply_err(e);
    }
	
	uint32_t target_sid = ((struct lyd_node_leaf_list*)nodeset->set.d[0])->value.uint32;
    if (target_sid == nc_session_get_id(session))
    {
        struct nc_server_error* e = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_PROT);
        nc_err_set_msg(e, "[RPC Handler] Forbidden to kill own session.", "en");
        return nc_server_reply_err(e);
    }
	
	struct nc_session* target_session = NULL;
	for (int i = 0; (target_session = nc_ps_get_session(g_pollsession, i)); ++i)
	{
        if (nc_session_get_id(target_session) == target_sid)
            break;
    }
	if (!target_session) 
	{
        struct nc_server_error* e = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_PROT);
        nc_err_set_msg(e, "Session with the specified \"session-id\" not found.", "en");
        return  nc_server_reply_err(e);
    }
	
	nc_session_set_status(target_session, NC_STATUS_INVALID);
    nc_session_set_term_reason(target_session, NC_SESSION_TERM_KILLED);
    nc_session_set_killed_by(target_session, nc_session_get_id(session));
	
    return nc_server_reply_ok();
}

struct nc_server_reply* rpc_callback_commit(struct lyd_node* rpc,struct nc_session *session)
{
	printf("<commit> RPC Received.\n");
	pthread_mutex_lock(&g_sidmutex_running);
	if(g_sid_running == 0)
	{
		lyd_merge(g_node_running, g_node_candidate, LYD_OPT_EXPLICIT);
		lyd_print_path("./configs/userconfig.xml", g_node_running, LYD_XML, LYP_FORMAT);
		pthread_mutex_unlock(&g_sidmutex_running);
		return nc_server_reply_ok();
	}
	else
	{
		pthread_mutex_unlock(&g_sidmutex_running);
	    return nc_server_reply_err(nc_err(NC_ERR_LOCK_DENIED, g_sid_running));
	}
}
