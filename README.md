# NETCONF Server /Prototype 1495A
  
The implementation of the server contains：  
**Located in main.cpp:**
 - Main Thread：NETCONF context initializing and control.
 - Server Thread：Maintains NETCONF sessions.
 - (Not Complete) Filewatch Thread：config file accesss control and YANG data instance maintaining.
 - (TODO) Notificator Thread：Notifications / state data.

**Located in rpc_callbacks.h/.cpp**
 - (Not Complete) RPC Handlers：Collection of RPC callbacks。

**Located in auth_callbacks.h/.cpp**
 - (Not Complete) SSH/TLS Authentication：SSH/TLS auth. callbacks.
---------
**RPC Handlers**
 **Necessary, Working**
 1. get
 2. get-config(filter not implemented)
 3. get-schema(integrated)
 4. lock
 5. unlock
 6. kill-sesssion
 7. close-session(integrated)
 
 **Necessary, Waiting for Filewatch Thread to complete**
 1. copy-config
 2. edit-config
 3. delete-config
 
 **Optional, Waiting for Filewatch Thread to complete**
 1. commit
