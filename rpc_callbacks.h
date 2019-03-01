#ifndef RPC_CALLBACKS_H
#define RPC_CALLBACKS_H
/* Function Prototypes of Mandatory RPC Processsing Callbacks */
/* <get> and <get-config> operation */
struct nc_server_reply* rpc_callback_get(struct lyd_node* rpc, struct nc_session *session);

/* <edit-config> , <copy-config> and <delete-config> operation */
/* Co-operates with Filewatch Subsystem. */
struct nc_server_reply* rpc_callback_edit(struct lyd_node* rpc, struct nc_session *session);
struct nc_server_reply* rpc_callback_copy(struct lyd_node* rpc, struct nc_session *session);
struct nc_server_reply* rpc_callback_delete(struct lyd_node* rpc, struct nc_session *session);

/* <lock> and <unlock> operation */
struct nc_server_reply* rpc_callback_lock(struct lyd_node* rpc, struct nc_session *session);
struct nc_server_reply* rpc_callback_unlock(struct lyd_node* rpc, struct nc_session *session);

/* <close-session> and <kill-session> operation */
// Using libnetconf2 built-in <close-session> RPC handler.
//struct nc_server_reply* rpc_callback_close(struct lyd_node* rpc, struct nc_session *session);
struct nc_server_reply* rpc_callback_kill(struct lyd_node* rpc, struct nc_session *session);

/* Function Prototypes of Optional RPC Processsing Callbacks */
/* <commit> operation, needs CANDIDATE feature */
struct nc_server_reply* rpc_callback_commit(struct lyd_node* rpc, struct nc_session *session);

#endif
