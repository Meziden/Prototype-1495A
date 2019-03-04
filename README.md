# NETCONF Server / Prototype 1495A
  
The implementation of the server contains：  
**Located in main.cpp:**
 - Main Thread：NETCONF context initializing and control.
 - Server Thread：NETCONF sessions management.
 - (Not Complete) Filewatch Thread：config file accesss control and YANG data instance maintaining.
 - (TODO) Notificator Thread：Notifications / state data.

**Located in rpc_callbacks.h/.cpp**
 - (Not Complete) RPC Handlers：Collection of RPC callbacks。

**Located in auth_callbacks.h/.cpp**
 - (Not Complete) SSH/TLS Authentication：SSH/TLS auth. callbacks.
---------
**RPC Handlers**
 **Working, with missing features.**
 1. get
 2. get-config(filter not implemented)
 3. get-schema(integrated)
 4. lock
 5. unlock
 6. kill-sesssion
 7. close-session(integrated)
 8. copy-config
 9. delete-config
 10. commit
 
 **Not Working, in progress.**  
 1. edit-config
