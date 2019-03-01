#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <nc_server.h>
#include "auth_callbacks.h"

/* SSH Authentication Callbacks */
int auth_callback_ssh_passwd
(const struct nc_session* session, const char* password, void* user_data)
{
	/* TODO */
	printf("[Authentication] Verifying SSH Password.\n");
	return 0;
}

int auth_callback_ssh_hostkey(	const char* name,
								void* user_data,
								char** privkey_path,
								char** privkey_data,
								int* privkey_data_rsa)
{
	if(!strcmp(name, "default"))
	{
		/* WARNING: need strdup here, original pointer got no memory capacity. */
		*privkey_path = strdup("/etc/ssh/ssh_host_rsa_key");
		return 0;
	}
	else
	return 1;
}
