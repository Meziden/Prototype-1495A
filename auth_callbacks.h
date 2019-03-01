#ifndef AUTH_CALLBACKS_H
#define AUTH_CALLBACKS_H

/* SSH Authentication Callbacks */
int auth_callback_ssh_passwd(	const struct nc_session* session,
								const char* password,
								void* user_data);

int auth_callback_ssh_interactive(	const struct nc_session* session,
									const ssh_message* msg,
									void* user_data);

int auth_callback_ssh_pubkey(	const struct nc_session* session,
								ssh_key key,
								void* user_data);
							
int auth_callback_ssh_hostkey(	const char* name,
								void* user_data,
								char** privkey_path,
								char** privkey_data,
								int* privkey_data_rsa);

#endif
