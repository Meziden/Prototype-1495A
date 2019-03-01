#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <nc_server.h>
#include "rpc_callbacks.h"

/* Global Pollsession Pointers */
extern struct nc_pollsession* g_pollsession;

/* Global Datastore Pointer, maintained by fdmonitor thread. */
extern struct lyd_node* g_node_config;
extern struct lyd_node* g_node_state;

/* Global Access Control */
extern pthread_mutex_t g_locksid_mutex;
extern volatile uint32_t g_locksid;

struct nc_server_reply* rpc_callback_get(struct lyd_node* rpc,struct nc_session *session)
{
	printf("<get>/<get-config> RPC Received.\n");
	
	/* Duplicate the <rpc> data, directly get the correct XPath in schema. */
	struct lyd_node* data = lyd_dup(rpc, LYD_DUP_OPT_RECURSIVE);
	struct lyd_node* data_config = NULL;
	struct lyd_node* data_state = NULL;
	
	/* Add state data for <get> operation. */
	if(!strcmp(rpc->schema->name, "get"))
	{
		data_config = lyd_dup(g_node_config,LYD_DUP_OPT_RECURSIVE);
		data_state = lyd_dup(g_node_state,LYD_DUP_OPT_RECURSIVE);
		/* Combine Return Data. */
		lyd_insert_after(data_config, data_state);
	}
	/* Choose correct datastore for <get-config> operation. */
	else
	{
		struct ly_set* nodeset = lyd_find_path(rpc, "/ietf-netconf:get-config/source/*");
		if (!strcmp(nodeset->set.d[0]->schema->name, "running"))
			data_config = lyd_dup(g_node_config,LYD_DUP_OPT_RECURSIVE);
		else if (!strcmp(nodeset->set.d[0]->schema->name, "startup"))
			data_config = NULL;
		else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate"))
			data_config = NULL;
		else
			printf("[RPC Handler] <get-config> Unexpected datastore source.");	
		ly_set_free(nodeset);
	}
	
	/* TODO YANG Data Instance Filter */
	
	/* Link the data node to the <rpc-reply> YANG Data Instance. */
	lyd_new_output_anydata(data, NULL, "data", data_config, LYD_ANYDATA_DATATREE);
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
	/* Search for the config <anyxml> element. */
	struct ly_set* nodeset = lyd_find_path(rpc, "/ietf-netconf:copy-config/source/config");
	//DEBUG
	if(nodeset->number == 0)
	{
		printf("WARNING: Unexpected Empty Data.\n");
		return nc_server_reply_ok();
	}
	else
	{
		lyd_print_file(stdout,nodeset->set.d[0], LYD_XML, LYP_FORMAT | LYP_WITHSIBLINGS);
	}
	
	struct lyd_node* data = lyd_dup(nodeset->set.d[0], LYD_DUP_OPT_RECURSIVE);
	struct lyd_node* data_config = lyd_dup(g_node_config,LYD_DUP_OPT_RECURSIVE);
	
	if (!strcmp(data->schema->name, "running"))
		printf("RUNNING.\n");
	else if (!strcmp(data->schema->name, "startup"))
		printf("STARTUP.\n");
	else if (!strcmp(data->schema->name, "config"))
	{	
		//data->schema = data_config->schema;
		data->parent = data_config->parent;
		if(lyd_insert(g_node_config, data));//, LYD_OPT_EXPLICIT));
			printf("WARNING!\n");
	}
	else
		printf("[RPC Handler] <copy-config> Unexpected source.\n");	
	
	ly_set_free(nodeset);
	lyd_free(data);
	lyd_free(data_config);
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
	/* Lock the datastore sid mutex. */
	pthread_mutex_lock(&g_locksid_mutex);
	if(g_locksid == 0)
	{
		g_locksid = nc_session_get_id(session);
		pthread_mutex_unlock(&g_locksid_mutex);
		return nc_server_reply_ok();
	}
	else
	{
		pthread_mutex_unlock(&g_locksid_mutex);
		return nc_server_reply_err(nc_err(NC_ERR_LOCK_DENIED, g_locksid));
	}
}

struct nc_server_reply* rpc_callback_unlock(struct lyd_node* rpc,struct nc_session *session)
{
	printf("<unlock> RPC Received.\n");
	/* Lock the datastore sid mutex. */
	pthread_mutex_lock(&g_locksid_mutex);
	if(g_locksid == nc_session_get_id(session))
	{
		g_locksid = 0;
		pthread_mutex_unlock(&g_locksid_mutex);
		return nc_server_reply_ok();
	}
	else
	{
		pthread_mutex_unlock(&g_locksid_mutex);
		struct nc_server_error* e;
		if(!g_locksid)
		{
			e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        	nc_err_set_msg(e, "[RPC Handler] The lock is not active.", "en");
        }
        else
        {
        	e = nc_err(NC_ERR_LOCK_DENIED, g_locksid);
        }
        return nc_server_reply_err(e);
	}
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
	return nc_server_reply_ok();
}
