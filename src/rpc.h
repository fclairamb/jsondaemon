/*
 * HTTP routes:
 *
 *   GET  /                    daemon info and the list of plugins
 *   GET  /_health             liveness
 *   GET  /<mount>             one plugin's info
 *   POST /<mount>             JSON-RPC 2.0 (single call or batch)
 *   POST /<mount>/<method>    plain call: the body is the params, the answer the result
 *   GET  /<mount>/<method>    same, params taken from the query string
 */
#ifndef JD_RPC_H
#define JD_RPC_H

#include "http.h"

/* http_handler; ctx is the registry. */
void rpc_handle(const http_request *req, http_response *res, void *ctx);

#endif /* JD_RPC_H */
