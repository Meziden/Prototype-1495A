/* 
 * A naive implementation of NETCONF server
 * Based on CESNET/libnetconf2 C-API
 * Revision : 20190320-alpha1
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <nc_server.h>

/* SSH/TLS Authentication Related Callbacks*/
#include "auth_callbacks.h"
 
/* RPC Callbacks */
#include "rpc_callbacks.h"

/* Error Handler Macro */
#define nc_assert(cond) if (!(cond)) { fprintf(stderr, "[NC_ASSERT]: Failed at %s:%d\n", __FILE__, __LINE__); exit(1); }

#define NC_ENABLED_SSH
#define NC_ENABLED_TLS

/* Constants */
const char* 	SEARCH_PATH 	= "./modules/";
const char* 	CONFIG_PATH 	= "./configs/";
const char*		SSH_ENDPT 		= "main";
const char*		SERVER_ADDR 	= "0.0.0.0";
const uint16_t	SERVER_PORT 	= 830;
/* millisec , 0 for non-block */
const int SERVER_ACCEPT_TIMEOUT = 0;
/* millisec , 0 for non-block */
const int SERVER_POLL_TIMEOUT = 0;

/* Global Libyang Context Pointer */
struct ly_ctx* ctx = NULL;

/* Global Pollsession Pointers */
struct nc_pollsession* g_pollsession = NULL;

/* Global Datastore Pointers */
struct lyd_node* g_node_running;
struct lyd_node* g_node_candidate;
struct lyd_node* g_node_startup;
struct lyd_node* g_node_state;
/* Global Datastore Filepath */
const char* RUNNING_XML_PATH = "configs/userconfig.xml";
const char* CANDIDATE_XML_PATH = "configs/userconfig_candidate.xml";
const char* STARTUP_XML_PATH = "configs/userconfig_startup.xml";
const char* STATE_XML_PATH = "configs/userdata.xml";

/* Global Datastore Access Control */
pthread_mutex_t g_sidmutex_running;
volatile uint32_t g_sid_running;
pthread_mutex_t g_sidmutex_candidate;
volatile uint32_t g_sid_candidate;
pthread_mutex_t g_sidmutex_startup;
volatile uint32_t g_sid_startup;

/* Global Control Flags */
int g_ctl_server = 1;

/* Unix Environment Settings */
int unixenv_init(int argc, char** argv);
void signal_handler(int signo);

/* Server Thread Entry Prototype */
void* server_thread_entry(void* arg);

/* FileWatch Thread Entry Prototype */
void* filewatch_thread_entry(void* arg);
const uint32_t FILEWATCH_MODE = IN_OPEN | IN_CLOSE | IN_DELETE | IN_CREATE;
const int FILEWATCH_BUFSIZE = 4096;
int fd_filewatch;
int wd_running;
int wd_candidate;
int wd_state;

/* Notificator Thread Entry Prototype */
void* notificator_thread_entry(void* arg);
pthread_mutex_t g_notif_mutex;

int main(int argc, char** argv)
{
	/* Unix Process Environment and CLI Argument Settings */
	nc_assert(!unixenv_init(argc, argv));
	
	const struct lys_module* module;
    const struct lys_node* node;
	
	/* Create libyang Context */
	ctx = ly_ctx_new(SEARCH_PATH, LY_CTX_TRUSTED);
	nc_assert(ctx);

	
	/* YANG Schema - Load Modules */
	/* 
	 * libyang internal modules:
	 * ietf-yang-metadata@2016-08-05
	 * yang@2017-02-20
	 * ietf-inet-types@2013-07-15
	 * ietf-yang-types@2013-07-15
	 * ietf-yang-library@2016-06-21
	 */
	module = ly_ctx_load_module(ctx, "ietf-netconf-acm", NULL);
	nc_assert(module);
	module = ly_ctx_load_module(ctx, "ietf-netconf", NULL);
	nc_assert(module);

	/* ietf-netconf module / optional feature configuration */
	lys_features_enable(module, "candidate");
	lys_features_enable(module, "writable-running");
	
	module = ly_ctx_load_module(ctx, "nc-notifications", NULL);
	nc_assert(module);
	module = ly_ctx_load_module(ctx, "notifications", NULL);
	nc_assert(module);
	module = ly_ctx_load_module(ctx, "ietf-netconf-notifications", NULL);
	nc_assert(module);
	module = ly_ctx_load_module(ctx, "ietf-netconf-monitoring", NULL);
	nc_assert(module);
	//module = ly_ctx_load_module(ctx, "ietf-datastores", NULL);
	//nc_assert(module);
	//module = ly_ctx_load_module(ctx, "ietf-system", NULL);
	//nc_assert(module);
	
	/* User Defined Modules */
	module = ly_ctx_load_module(ctx, "userconfig", NULL);
	nc_assert(module);
	module = ly_ctx_load_module(ctx, "userdata", NULL);
	nc_assert(module);
	
	/* YANG Data Instance - XML Parsing */
	g_node_running = lyd_parse_path(ctx, RUNNING_XML_PATH, LYD_XML, LYD_OPT_CONFIG);
	nc_assert(g_node_running);
	g_node_candidate = lyd_parse_path(ctx, CANDIDATE_XML_PATH, LYD_XML, LYD_OPT_CONFIG);
	nc_assert(g_node_candidate);
	g_node_state = lyd_parse_path(ctx, STATE_XML_PATH, LYD_XML, LYD_OPT_DATA_ADD_YANGLIB);
	nc_assert(g_node_state);
	
	//DEBUG
	//lyd_print_file(stdout, g_node_state, LYD_XML, LYP_FORMAT);
	
	lyd_validate(&g_node_running, LYD_OPT_CONFIG, NULL);
	lyd_validate(&g_node_candidate, LYD_OPT_CONFIG, NULL);
	lyd_validate(&g_node_state, LYD_OPT_DATA, NULL);
	
	/* Set RPC Callbacks */
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:get", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_get);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:get-config", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_get);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:edit-config", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_edit);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:copy-config", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_copy);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:delete-config", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_delete);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:lock", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_lock);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:unlock", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_unlock);
	
	//node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:close-session", 0);
	//nc_assert(node);
	//lys_set_private(node, (void*)rpc_callback_close);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:kill-session", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_kill);
	
	node = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:commit", 0);
	nc_assert(node);
	lys_set_private(node, (void*)rpc_callback_commit);
	
	/* set Notifications subscription callback */
    node = ly_ctx_get_node(ctx, NULL, "/notifications:create-subscription", 0);
    lys_set_private(node, (void*)rpc_callback_subscribe);
	
	/* NETCONF server init */
	nc_server_init(ctx);
	
	/* NETCONF Capability Configuration */
	nc_server_set_capab_withdefaults(NC_WD_EXPLICIT, NC_WD_ALL | NC_WD_ALL_TAG | NC_WD_TRIM | NC_WD_EXPLICIT);
	/* set capabilities for the NETCONF Notifications */
    nc_server_set_capability("urn:ietf:params:netconf:capability:notification:1.0");
    nc_server_set_capability("urn:ietf:params:netconf:capability:interleave:1.0");
	
	/* SSH/TLS Authentication Settings */
	nc_server_ssh_set_hostkey_clb(auth_callback_ssh_hostkey, NULL, NULL);
	nc_server_ssh_set_passwd_auth_clb(auth_callback_ssh_passwd, NULL, NULL);
	
	/* SSH/TLS Endpoint Settings */
	nc_assert(!nc_server_add_endpt(SSH_ENDPT, NC_TI_LIBSSH));
	nc_assert(!nc_server_endpt_set_address(SSH_ENDPT, SERVER_ADDR));
	nc_assert(!nc_server_endpt_set_port(SSH_ENDPT, SERVER_PORT));
	nc_assert(!nc_server_ssh_endpt_add_hostkey(SSH_ENDPT, "default", -1));
	
	//nc_assert(!nc_server_ssh_endpt_set_auth_methods(SSH_ENDPT, NC_SSH_AUTH_PUBLICKEY | NC_SSH_AUTH_PASSWORD | NC_SSH_AUTH_INTERACTIVE));
	nc_assert(!nc_server_ssh_endpt_set_auth_methods(SSH_ENDPT, NC_SSH_AUTH_PASSWORD));
		
	/* Start Server Thread */
	pthread_t server_tid;
	pthread_create(&server_tid, NULL, server_thread_entry, NULL);
	
	/* Start Notificator Thread */
	pthread_t notificator_tid;
	pthread_create(&notificator_tid, NULL, notificator_thread_entry, NULL);
	
	/* Start Filewatch Thread */
	pthread_t filewatch_tid;
	pthread_create(&filewatch_tid, NULL, filewatch_thread_entry, NULL);

	/* TODO Command Line Interface */
	
	/* Thread Scheduling */
	pthread_join(filewatch_tid, NULL);
	pthread_join(notificator_tid, NULL);
	pthread_join(server_tid, NULL);
	
	/* Stop NETCONF server */
	printf("[Main Thread] Cleaning up allocated resource.\n");
	nc_server_destroy();  
	lyd_free_withsiblings(g_node_running);
	lyd_free_withsiblings(g_node_candidate);
	lyd_free_withsiblings(g_node_state);
	ly_ctx_clean(ctx, NULL);
	ly_ctx_destroy(ctx, NULL);
	return 0;
}

void* filewatch_thread_entry(void* arg)
{
	printf("[Filewatch Thread] Initializing...\n");
	/* Add filepaths to the filewatch thread. */
	fd_filewatch = inotify_init1(IN_NONBLOCK);
	if(fd_filewatch <= 0)
		printf("[Filewatch Thread] ERROR: Failed to init Inotify.\n");
	wd_running = inotify_add_watch(fd_filewatch, RUNNING_XML_PATH, FILEWATCH_MODE);
	if(fd_filewatch <= 0)
		printf("[Filewatch Thread] ERROR: Failed to init Inotify.\n");
	wd_candidate = inotify_add_watch(fd_filewatch, CANDIDATE_XML_PATH, FILEWATCH_MODE);
	if(fd_filewatch <= 0)
		printf("[Filewatch Thread] ERROR: Failed to init Inotify.\n");
	wd_state = inotify_add_watch(fd_filewatch, STATE_XML_PATH, FILEWATCH_MODE);
	if(fd_filewatch <= 0)
		printf("[Filewatch Thread] ERROR: Failed to init Inotify.\n");
	
	/* Inotify Events */
	char event_buf[FILEWATCH_BUFSIZE];  
	int event_length;  
	struct inotify_event *event_ptr;
	
	/* Poll Releated Stuff */
	struct pollfd poll_fds[1];
	nfds_t poll_nfds = 1;
	int poll_number;
	
	poll_fds[0].fd = fd_filewatch;
	poll_fds[0].events = POLLIN;
	
	printf("[Filewatch Thread] Ready.\n");
	while(g_ctl_server)
	{
        /* Using poll IO multiplexing. */
        poll_number = poll(poll_fds, poll_nfds, 500);
        if(poll_number == 0)
        	continue;
        if(poll_fds[0].revents & POLLIN)
        {
			/* Use non-block here to support poll. */
			event_length = read(fd_filewatch, event_buf, sizeof(event_buf));
			if(event_length <= 0 && errno != EAGAIN)  
        		printf("[Filewatch Thread] read() failed.\n");
			if(event_length < (int)sizeof(struct inotify_event))
    	    	printf("[Filewatch Thread] WARNING: Incomplete inotify event.\n");
  	      
  	      /* Init Buffer Pointer*/
  	      event_ptr =(struct inotify_event *)event_buf;
  	      
    	    /* Handle events and TODO update YANG data instance / send notofication. */
			while(event_length > 0)
			{
				if(event_ptr -> wd == wd_running)
				{
					if(event_ptr -> mask & IN_OPEN)
					{
						printf("[Filewatch Thread] Running Config File Opened.\n");
					}
					if(event_ptr -> mask & IN_CLOSE_NOWRITE)
					{
						printf("[Filewatch Thread] Running Config File Closed, not written.\n");
					}
					if(event_ptr -> mask & IN_CLOSE_WRITE)
					{
						printf("[Filewatch Thread] Running Config File Closed, written.\n");
					}
					if(event_ptr -> mask & IN_CREATE)
					{
						printf("[Filewatch Thread] Running Config File Created.\n");
					}
					if(event_ptr -> mask & IN_DELETE)
					{
						printf("[Filewatch Thread] Running Config File Deleted.\n");
					}
					if(event_ptr -> mask == 32768)
					{
						printf("[Filewatch Thread] Running Config File inode Removed, monitoring new node.\n");
						if(fd_filewatch <= 0)
							printf("[Filewatch Thread] ERROR: Failed to add inode.\n");
						wd_running = inotify_add_watch(fd_filewatch, RUNNING_XML_PATH, FILEWATCH_MODE);
					}
				}
				
				if(event_ptr -> wd == wd_candidate)
				{
					if(event_ptr -> mask & IN_OPEN)
					{
						printf("[Filewatch Thread] Candidate Config File Opened.\n");
					}
					if(event_ptr -> mask & IN_CLOSE_NOWRITE)
					{
						printf("[Filewatch Thread] Candidate Config File Closed, not written.\n");
					}
					if(event_ptr -> mask & IN_CLOSE_WRITE)
					{
						printf("[Filewatch Thread] Candidate Config File Closed, written.\n");
					}
					if(event_ptr -> mask & IN_CREATE)
					{
						printf("[Filewatch Thread] Candidate Config File Created.\n");
					}
					if(event_ptr -> mask & IN_DELETE)
					{
						printf("[Filewatch Thread] Candidate Config File Deleted.\n");
					}
					if(event_ptr -> mask == 32768)
					{
						printf("[Filewatch Thread] Candidate Config File inode Removed, monitoring new node.\n");
						if(fd_filewatch <= 0)
							printf("[Filewatch Thread] ERROR: Failed to add inode.\n");
						wd_running = inotify_add_watch(fd_filewatch, CANDIDATE_XML_PATH, FILEWATCH_MODE);
					}
				}
				
				if(event_ptr -> wd == wd_state)
				{
					if(event_ptr -> mask & IN_OPEN)
					{
						printf("[Filewatch Thread] State File Opened.\n");
					}
					if(event_ptr -> mask & IN_CLOSE_NOWRITE)
					{
						printf("[Filewatch Thread] State File Closed, not written.\n");
					}
					if(event_ptr -> mask & IN_CLOSE_WRITE)
					{
						printf("[Filewatch Thread] State File Closed, written.\n");
					}
					if(event_ptr -> mask & IN_CREATE)
					{
						printf("[Filewatch Thread] State File Created.\n");
					}
					if(event_ptr -> mask & IN_DELETE)
					{
						printf("[Filewatch Thread] State File Deleted.\n");
					}
					if(event_ptr -> mask == 32768)
					{
						printf("[Filewatch Thread] State File inode Removed, monitoring new node.\n");
					if(fd_filewatch <= 0)
						printf("[Filewatch Thread] ERROR: Failed to add inode.\n");
						wd_running = inotify_add_watch(fd_filewatch, STATE_XML_PATH, FILEWATCH_MODE);
					}
				}
				/* Flush the buffered stdout, for real-time monitoring. */
				fflush(stdout);
				event_length -= (sizeof(struct inotify_event) + event_ptr->len);
				event_ptr += (sizeof(struct inotify_event) + event_ptr->len);
			}
		}
	}
	printf("[Filewatch Thread] Cleaning up allocated resource.\n");
	inotify_rm_watch(fd_filewatch, wd_running);
	inotify_rm_watch(fd_filewatch, wd_candidate);
	inotify_rm_watch(fd_filewatch, wd_state);
	close(fd_filewatch);
	nc_thread_destroy();
}

void* server_thread_entry(void* arg)
{
	printf("[Server Thread] Started.\n");
	
	NC_MSG_TYPE msgtype;
	struct nc_session* session = NULL;
	
	/* Poll Session */
	g_pollsession = nc_ps_new();
	nc_assert(g_pollsession);
	
	/* Server Thread Loop */
	while(g_ctl_server)
	{
		msgtype = nc_accept(SERVER_ACCEPT_TIMEOUT, &session);
		switch(msgtype)
		{
			case NC_MSG_HELLO:
				printf("[Server Thread] <hello> received.\n");
				/* Fill Poll Session with Accepted Session */
				nc_assert(!nc_ps_add_session(g_pollsession, session));
				printf("[Server Thread] Session Accepted, %d remaining.\n", nc_ps_session_count(g_pollsession));
				break;
			case NC_MSG_WOULDBLOCK:
				/* NON-BLOCK NC_ACCEPT(), UNUSED HERE */
				//printf("[Server Thread] Timeout.\n");
				break;
			case NC_MSG_BAD_HELLO:
				printf("[Server Thread] <hello> parsing failed.\n");
				break;
			case NC_MSG_ERROR:
				printf("[Server Thread] nc_accept() Error.\n");
				break;
			default:
				printf("[Server Thread] Unexpected response from nc_accept().\n");
		}
		int poll_ret = nc_ps_poll(g_pollsession, SERVER_POLL_TIMEOUT, &session);
		if(poll_ret & NC_PSPOLL_SESSION_TERM)
		{
			/* Access Control : Release closed session controlled datastores */
			pthread_mutex_lock(&g_sidmutex_running);
			if(g_sid_running == nc_session_get_id(session))
			{
				printf("[Server Thread] Releasing related datastore locks.\n");
				g_sid_running = 0;
			}
			pthread_mutex_unlock(&g_sidmutex_running);
			
			pthread_mutex_lock(&g_sidmutex_candidate);
			if(g_sid_candidate == nc_session_get_id(session))
			{
				printf("[Server Thread] Releasing related datastore locks.\n");
				g_sid_candidate = 0;
			}
			pthread_mutex_unlock(&g_sidmutex_candidate);
			
			nc_assert(!nc_ps_del_session(g_pollsession, session));
			nc_session_free(session, NULL);
			printf("[Server Thread] Session Closed, %d remaining.\n", nc_ps_session_count(g_pollsession));
		}
		else
		{
			if(poll_ret & (NC_PSPOLL_NOSESSIONS | NC_PSPOLL_TIMEOUT | NC_PSPOLL_ERROR))
				usleep(10000);
		}
		
	}
	printf("[Server Thread] Cleaning up allocated resource.\n");
	nc_ps_clear(g_pollsession, 0, NULL);
	pthread_mutex_destroy(&g_sidmutex_running);
    nc_ps_free(g_pollsession);
	nc_thread_destroy();
}

void* notificator_thread_entry(void* arg)
{
	printf("[Notificator Thread] Started.\n");
	pthread_mutex_init(&g_notif_mutex,NULL);
	
	while(g_ctl_server)
	{
		char msg_buf[26] = {0};
		const struct lys_module* mod = ly_ctx_get_module(ctx, "nc-notifications", NULL, 1);
		struct lyd_node* event = lyd_new(NULL, mod, "replayComplete");
		struct nc_server_notif* notifdata = nc_server_notif_new(event,
											nc_time2datetime(time(NULL),NULL,msg_buf),
											NC_PARAMTYPE_DUP_AND_FREE);
		for(uint16_t psid; ;psid++)
		{
			struct nc_session* session_ptr = nc_ps_get_session(g_pollsession, psid);
			if(session_ptr == NULL)
				break;
			else
			{
				nc_server_notif_send(session_ptr, notifdata, -1);
			}
		}
		printf("[Notificator Thread] Cyka Blyat!\n");
		sleep(1);
	}
	printf("[Notificator Thread] Cleaning up allocated resource.\n");
	pthread_mutex_destroy(&g_notif_mutex);
	nc_thread_destroy();
}

/* Unix Related Stuff */
int unixenv_init(int argc, char** argv)
{
	printf("[Main Thread] Starting NETCONF Server...\n");
	/* Setting up signal handlers */
	sigset_t block_mask;
	sigfillset(&block_mask);
	struct sigaction action;
	action.sa_handler = signal_handler;
	action.sa_mask = block_mask;
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);
	/* For Remote Logins, ignore SIGHUP, continue running when logout. */
	sigaction(SIGHUP, &action, NULL);
	
	/* Access Control related*/
	pthread_mutex_init(&g_sidmutex_running, NULL);
	
	return 0;
}

void signal_handler(int signo)
{
	switch(signo)
	{
		case SIGINT:
			printf("\nSIGINT received, stopping server.\n");
			g_ctl_server = 0;
			break;
		case SIGTERM:
			printf("\nSIGTERM received, stopping server\n.");
			g_ctl_server = 0;
			break;
		case SIGSEGV:
			printf("\nSIGSEGV received, stopping server.\n");
			g_ctl_server = 0;
			exit(0);
			break;
	}
}
