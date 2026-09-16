/**
 * @file opcua_server.h
 * @brief OPC UA server task management.
 */

#ifndef OPCUA_SERVER_H
#define OPCUA_SERVER_H

#include <stddef.h>
#include "opcua_object.h"

/**
 * @brief Starts the OPC UA server task pinned to core 1.
 *
 * The array is copied, so it may be a local variable; the objects it points
 * to (properties and values) must stay alive for the lifetime of the server.
 *
 * @param objects     Array of generic objects to expose.
 * @param objectCount Number of objects in the array.
 */
void opcua_server_start_task(OpcUaObject* objects, size_t objectCount);

#endif
