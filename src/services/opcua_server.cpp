/**
 * @file opcua_server.cpp
 * @brief Generic OPC UA server for ESP32.
 *
 * Each OpcUaObject stepsed to opcua_server_start_task() is exposed as an
 * OPC UA Object node whose properties are variable nodes. Values are served
 * through data sources, so the server always returns the current value of the
 * local variables and forwards client writes back to them.
 */

#include "services/opcua_server.h"
#include "config.h"
#include "services/wifi_manager.h"
#include <Arduino.h>
#include <open62541.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <stdlib.h>
#include <string.h>

static UA_Server* server = NULL;            /**< OPC UA server instance. */
static OpcUaObject* objects = NULL;         /**< Copy of the exposed objects. */
static size_t objectCount = 0;              /**< Number of exposed objects. */

/**
 * @brief Node context stepsed to OPC UA data source callbacks.
 *
 * Associates a property with its parent object so that the object-level
 * lock/unlock callbacks can be used to protect concurrent accesses.
 */
typedef struct
{
    OpcUaProperty* prop;   /**< Property descriptor. */
    OpcUaObject*   obj;    /**< Parent object (may be NULL). */
} PropertyNodeContext;

/**
 * @brief Returns the OPC UA data type matching a value type.
 */
static const UA_DataType* typeFor(OpcUaValueType type)
{
    switch (type)
    {
        case OPCUA_TYPE_BOOL:   return &UA_TYPES[UA_TYPES_BOOLEAN];
        case OPCUA_TYPE_INT32:  return &UA_TYPES[UA_TYPES_INT32];
        case OPCUA_TYPE_UINT32: return &UA_TYPES[UA_TYPES_UINT32];
        case OPCUA_TYPE_DOUBLE: return &UA_TYPES[UA_TYPES_DOUBLE];
        case OPCUA_TYPE_STRING: return &UA_TYPES[UA_TYPES_STRING];
        case OPCUA_TYPE_FLOAT:
        default:                return &UA_TYPES[UA_TYPES_FLOAT];
    }
}

/**
 * @brief Data source read callback: copies the local value into the variant.
 */
static UA_StatusCode propertyRead(UA_Server* s, const UA_NodeId* sessionId,
                                  void* sessionContext, const UA_NodeId* nodeId,
                                  void* nodeContext, UA_Boolean sourceTimeStamp,
                                  const UA_NumericRange* range, UA_DataValue* dataValue)
{
    (void)s; (void)sessionId; (void)sessionContext; (void)nodeId;
    (void)sourceTimeStamp; (void)range;

    PropertyNodeContext* ctx = (PropertyNodeContext*)nodeContext;
    if (ctx == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    OpcUaProperty* prop = ctx->prop;
    OpcUaObject* obj = ctx->obj;
    if (prop == NULL || prop->value == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Keep the critical section as short as possible: copy the live value to a
       local temporary under the object lock, then build the OPC UA variant
       outside the lock (memory allocation is not safe inside a critical
       section). */
    if (prop->type == OPCUA_TYPE_STRING)
    {
        char tmp[128];
        size_t maxLen = sizeof(tmp) - 1;
        if (prop->size > 0 && prop->size - 1 < maxLen)
        {
            maxLen = prop->size - 1;
        }

        if (obj != NULL && obj->onLock != NULL)
        {
            obj->onLock(obj->lockContext);
        }
        size_t n = strnlen((const char*)prop->value, maxLen);
        memcpy(tmp, prop->value, n);
        tmp[n] = '\0';
        if (obj != NULL && obj->onUnlock != NULL)
        {
            obj->onUnlock(obj->lockContext);
        }

        UA_String s = UA_String_fromChars(tmp);
        UA_Variant_setScalarCopy(&dataValue->value, &s, &UA_TYPES[UA_TYPES_STRING]);
        UA_String_clear(&s);
    }
    else
    {
        uint64_t tmp = 0;
        size_t valueSize = 0;
        switch (prop->type)
        {
            case OPCUA_TYPE_BOOL:   valueSize = sizeof(bool); break;
            case OPCUA_TYPE_INT32:  valueSize = sizeof(int32_t); break;
            case OPCUA_TYPE_UINT32: valueSize = sizeof(uint32_t); break;
            case OPCUA_TYPE_FLOAT:  valueSize = sizeof(float); break;
            case OPCUA_TYPE_DOUBLE: valueSize = sizeof(double); break;
            default:                valueSize = sizeof(float); break;
        }

        if (obj != NULL && obj->onLock != NULL)
        {
            obj->onLock(obj->lockContext);
        }
        memcpy(&tmp, prop->value, valueSize);
        if (obj != NULL && obj->onUnlock != NULL)
        {
            obj->onUnlock(obj->lockContext);
        }

        UA_Variant_setScalarCopy(&dataValue->value, &tmp, typeFor(prop->type));
    }
    dataValue->hasValue = true;
    return UA_STATUSCODE_GOOD;
}

/**
 * @brief Data source write callback: copies the client value into local storage.
 */
static UA_StatusCode propertyWrite(UA_Server* s, const UA_NodeId* sessionId,
                                   void* sessionContext, const UA_NodeId* nodeId,
                                   void* nodeContext, const UA_NumericRange* range,
                                   const UA_DataValue* dataValue)
{
    (void)s; (void)sessionId; (void)sessionContext; (void)nodeId; (void)range;

    PropertyNodeContext* ctx = (PropertyNodeContext*)nodeContext;
    if (ctx == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    OpcUaProperty* prop = ctx->prop;
    OpcUaObject* obj = ctx->obj;
    if (prop == NULL || prop->value == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    if (prop->access != OPCUA_ACCESS_READWRITE)
    {
        return UA_STATUSCODE_BADNOTWRITABLE;
    }
    if (!dataValue->hasValue || dataValue->value.type != typeFor(prop->type))
    {
        return UA_STATUSCODE_BADTYPEMISMATCH;
    }

    /* Validate and copy the incoming value to a local temporary outside the
       lock, then perform the actual write under the lock. */
    switch (prop->type)
    {
        case OPCUA_TYPE_BOOL:
        {
            bool tmp = *(UA_Boolean*)dataValue->value.data;
            if (obj != NULL && obj->onLock != NULL) obj->onLock(obj->lockContext);
            *(bool*)prop->value = tmp;
            if (obj != NULL && obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
            break;
        }
        case OPCUA_TYPE_INT32:
        {
            int32_t tmp = *(UA_Int32*)dataValue->value.data;
            if (obj != NULL && obj->onLock != NULL) obj->onLock(obj->lockContext);
            *(int32_t*)prop->value = tmp;
            if (obj != NULL && obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
            break;
        }
        case OPCUA_TYPE_UINT32:
        {
            uint32_t tmp = *(UA_UInt32*)dataValue->value.data;
            if (obj != NULL && obj->onLock != NULL) obj->onLock(obj->lockContext);
            *(uint32_t*)prop->value = tmp;
            if (obj != NULL && obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
            break;
        }
        case OPCUA_TYPE_FLOAT:
        {
            float tmp = *(UA_Float*)dataValue->value.data;
            if (obj != NULL && obj->onLock != NULL) obj->onLock(obj->lockContext);
            *(float*)prop->value = tmp;
            if (obj != NULL && obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
            break;
        }
        case OPCUA_TYPE_DOUBLE:
        {
            double tmp = *(UA_Double*)dataValue->value.data;
            if (obj != NULL && obj->onLock != NULL) obj->onLock(obj->lockContext);
            *(double*)prop->value = tmp;
            if (obj != NULL && obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
            break;
        }
        case OPCUA_TYPE_STRING:
        {
            UA_String* s = (UA_String*)dataValue->value.data;
            if (prop->size == 0 || prop->value == NULL)
            {
                return UA_STATUSCODE_BADINTERNALERROR;
            }
            size_t n = (s->length < prop->size - 1) ? s->length : prop->size - 1;
            if (obj != NULL && obj->onLock != NULL) obj->onLock(obj->lockContext);
            memcpy(prop->value, s->data, n);
            ((char*)prop->value)[n] = '\0';
            if (obj != NULL && obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
            break;
        }
    }

    return UA_STATUSCODE_GOOD;
}

/**
 * @brief Creates (or reuses) a folder object node under a parent.
 * @return The NodeId of the folder object.
 */
static UA_NodeId addFolderNode(UA_NodeId parent, const char* name)
{
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en-US", (char*)name);

    UA_NodeId folderId;
    UA_StatusCode retval = UA_Server_addObjectNode(
        server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, (char*)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
        attr, NULL, &folderId);

    if (retval != UA_STATUSCODE_GOOD)
    {
        Serial.printf("[OPC UA] Failed to add folder '%s': %s\n",
                      name, UA_StatusCode_name(retval));
        return UA_NODEID_NULL;
    }
    return folderId;
}

/**
 * @brief Creates the OPC UA nodes (one object node per object, optional folder
 *        nodes, and one variable node per property).
 */
static void addObjectNodes(OpcUaObject* obj)
{
    UA_ObjectAttributes objAttr = UA_ObjectAttributes_default;
    objAttr.displayName = UA_LOCALIZEDTEXT("en-US", (char*)obj->name);

    UA_NodeId objRequested = (obj->nodeId == 0)
        ? UA_NODEID_NULL
        : UA_NODEID_NUMERIC(1, obj->nodeId);

    UA_NodeId objNodeId;
    UA_StatusCode objRetval = UA_Server_addObjectNode(
        server, objRequested,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, (char*)obj->name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
        objAttr, NULL, &objNodeId);

    if (objRetval != UA_STATUSCODE_GOOD)
    {
        Serial.printf("[OPC UA] Failed to add object '%s': %s\n",
                      obj->name, UA_StatusCode_name(objRetval));
        return;
    }

    // Allocate one node context per property so that the callbacks know which
    // object (and therefore which lock) the property belongs to.
    PropertyNodeContext* contexts = (PropertyNodeContext*)malloc(
        sizeof(PropertyNodeContext) * obj->propertyCount);
    if (contexts == NULL)
    {
        Serial.printf("[OPC UA] Out of memory while creating contexts for '%s'.\n", obj->name);
        return;
    }

    // Small cache of the folder nodes already created for this object.
    const size_t MAX_FOLDERS = 16;
    const char* folderNames[MAX_FOLDERS];
    UA_NodeId folderIds[MAX_FOLDERS];
    size_t folderCount = 0;

    for (size_t i = 0; i < obj->propertyCount; ++i)
    {
        OpcUaProperty* prop = &obj->properties[i];
        contexts[i].prop = prop;
        contexts[i].obj = obj;

        // Resolve the parent node: the object itself or a folder.
        UA_NodeId parentId = objNodeId;
        if (prop->folder != NULL)
        {
            size_t f = 0;
            while (f < folderCount && strcmp(folderNames[f], prop->folder) != 0)
            {
                ++f;
            }
            if (f == folderCount && folderCount < MAX_FOLDERS)
            {
                folderNames[folderCount] = prop->folder;
                folderIds[folderCount] = addFolderNode(objNodeId, prop->folder);
                ++folderCount;
            }
            if (f < folderCount && !UA_NodeId_isNull(&folderIds[f]))
            {
                parentId = folderIds[f];
            }
        }

        UA_VariableAttributes attr = UA_VariableAttributes_default;
        attr.displayName = UA_LOCALIZEDTEXT("en-US", (char*)prop->name);
        attr.dataType = typeFor(prop->type)->typeId;
        attr.accessLevel = (prop->access == OPCUA_ACCESS_READWRITE)
            ? (UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE)
            : UA_ACCESSLEVELMASK_READ;
        attr.userAccessLevel = attr.accessLevel;

        UA_DataSource dataSource;
        dataSource.read = propertyRead;
        dataSource.write = (prop->access == OPCUA_ACCESS_READWRITE) ? propertyWrite : NULL;

        UA_NodeId propRequested = (prop->nodeId == 0)
            ? UA_NODEID_NULL
            : UA_NODEID_NUMERIC(1, prop->nodeId);

        UA_StatusCode retval = UA_Server_addDataSourceVariableNode(
            server, propRequested, parentId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
            UA_QUALIFIEDNAME(1, (char*)prop->name),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, dataSource, &contexts[i], NULL);

        if (retval != UA_STATUSCODE_GOOD)
        {
            Serial.printf("[OPC UA] Failed to add property '%s': %s\n",
                          prop->name, UA_StatusCode_name(retval));
        }
    }
}

/**
 * @brief Calls the update callback of every object (Diag/ values, etc.).
 */
static void updateObjects()
{
    for (size_t i = 0; i < objectCount; ++i)
    {
        OpcUaObject* obj = &objects[i];
        if (obj->onUpdate != NULL)
        {
            obj->onUpdate();
        }
    }
}

/**
 * @brief Prints the current values of every object to the serial port.
 */
static void logObjects()
{
    for (size_t i = 0; i < objectCount; ++i)
    {
        OpcUaObject* obj = &objects[i];

        Serial.printf("[OPC UA] %s:", obj->name);
        for (size_t j = 0; j < obj->propertyCount; ++j)
        {
            OpcUaProperty* prop = &obj->properties[j];

            /* Copy the live value to a local temporary under lock, then print
               outside the lock so that the critical section stays short. */
            switch (prop->type)
            {
                case OPCUA_TYPE_BOOL:
                {
                    bool v;
                    if (obj->onLock != NULL) obj->onLock(obj->lockContext);
                    v = *(bool*)prop->value;
                    if (obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
                    Serial.printf(" %s=%s", prop->name, v ? "true" : "false");
                    break;
                }
                case OPCUA_TYPE_INT32:
                {
                    int32_t v;
                    if (obj->onLock != NULL) obj->onLock(obj->lockContext);
                    v = *(int32_t*)prop->value;
                    if (obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
                    Serial.printf(" %s=%d", prop->name, (int)v);
                    break;
                }
                case OPCUA_TYPE_UINT32:
                {
                    uint32_t v;
                    if (obj->onLock != NULL) obj->onLock(obj->lockContext);
                    v = *(uint32_t*)prop->value;
                    if (obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
                    Serial.printf(" %s=%u", prop->name, (unsigned)v);
                    break;
                }
                case OPCUA_TYPE_FLOAT:
                {
                    float v;
                    if (obj->onLock != NULL) obj->onLock(obj->lockContext);
                    v = *(float*)prop->value;
                    if (obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
                    Serial.printf(" %s=%.2f", prop->name, v);
                    break;
                }
                case OPCUA_TYPE_DOUBLE:
                {
                    double v;
                    if (obj->onLock != NULL) obj->onLock(obj->lockContext);
                    v = *(double*)prop->value;
                    if (obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
                    Serial.printf(" %s=%.2f", prop->name, v);
                    break;
                }
                case OPCUA_TYPE_STRING:
                {
                    char tmp[128];
                    size_t maxLen = sizeof(tmp) - 1;
                    if (prop->size > 0 && prop->size - 1 < maxLen)
                    {
                        maxLen = prop->size - 1;
                    }
                    if (obj->onLock != NULL) obj->onLock(obj->lockContext);
                    size_t n = strnlen((const char*)prop->value, maxLen);
                    memcpy(tmp, prop->value, n);
                    tmp[n] = '\0';
                    if (obj->onUnlock != NULL) obj->onUnlock(obj->lockContext);
                    Serial.printf(" %s=\"%s\"", prop->name, tmp);
                    break;
                }
            }
        }
        Serial.println();
    }
}

/**
 * @brief OPC UA server task running on core 1.
 * @param pvParameters FreeRTOS task parameters (unused).
 */
static void opcuaServerTask(void* pvParameters)
{
    (void)pvParameters;

    Serial.println("[OPC UA] Starting server...");

    UA_ServerConfig config;
    memset(&config, 0, sizeof(UA_ServerConfig));

    UA_StatusCode retval = UA_ServerConfig_setMinimalCustomBuffer(
        &config, OPCUA_PORT, NULL, OPCUA_SEND_BUFFER, OPCUA_RECV_BUFFER);

    if (retval != UA_STATUSCODE_GOOD)
    {
        Serial.printf("[OPC UA] Configuration error: %s\n", UA_StatusCode_name(retval));
        vTaskDelete(NULL);
        return;
    }

    // Limit logs to warnings to reduce CPU and serial load.
    config.logger = UA_Log_Stdout_withLevel(UA_LOGLEVEL_WARNING);

    /* The default minimal configuration advertises username/password and x509
       user token policies. With SecurityPolicy None, Ignition 8.3 rejects the
       endpoint as insecure and fails during discovery. Re-initialize access
       control for anonymous-only login, then synchronize the endpoints. */
    UA_AccessControl_default(&config, true, NULL,
                             &config.securityPolicies[0].policyUri, 0, NULL);
    for (size_t i = 0; i < config.endpointsSize; ++i)
    {
        UA_Array_delete(config.endpoints[i].userIdentityTokens,
                        config.endpoints[i].userIdentityTokensSize,
                        &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
        config.endpoints[i].userIdentityTokens = NULL;
        config.endpoints[i].userIdentityTokensSize = 0;
        if (config.accessControl.userTokenPoliciesSize > 0)
        {
            UA_Array_copy(config.accessControl.userTokenPolicies,
                          config.accessControl.userTokenPoliciesSize,
                          (void**)&config.endpoints[i].userIdentityTokens,
                          &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
            config.endpoints[i].userIdentityTokensSize =
                config.accessControl.userTokenPoliciesSize;
        }
    }

    // Application identity expected by strict clients (e.g. Ignition).
    config.applicationDescription.applicationUri =
        UA_String_fromChars("urn:meublesfutur:screwstation");
    config.applicationDescription.applicationName.text =
        UA_String_fromChars("ScrewStation SW-4L");
    config.applicationDescription.applicationName.locale =
        UA_String_fromChars("en-US");
    for (size_t i = 0; i < config.endpointsSize; ++i)
    {
        UA_String_clear(&config.endpoints[i].server.applicationUri);
        UA_LocalizedText_clear(&config.endpoints[i].server.applicationName);
        UA_String_copy(&config.applicationDescription.applicationUri,
                       &config.endpoints[i].server.applicationUri);
        UA_LocalizedText_copy(&config.applicationDescription.applicationName,
                              &config.endpoints[i].server.applicationName);
    }

    // Use the ESP32 IP address as hostname to avoid DNS resolution issues.
    char ipBuffer[16];
    wifi_get_ip(ipBuffer, sizeof(ipBuffer));
    UA_String_clear(&config.customHostname);
    config.customHostname = UA_String_fromChars(ipBuffer);

    /* Force the discovery URL advertised by GetEndpoints to the actual IP.
       Some clients send an empty endpointUrl in the discovery request and then
       use the discoveryUrls returned by the server. If that URL points to a
       hostname that does not resolve on the client PC, Ignition reports
       "Error retrieving discovery server information". */
    char discoveryUrlBuffer[64];
    snprintf(discoveryUrlBuffer, sizeof(discoveryUrlBuffer),
             "opc.tcp://%s:%d/", ipBuffer, OPCUA_PORT);
    UA_String discoveryUrl = UA_String_fromChars(discoveryUrlBuffer);
    if (config.applicationDescription.discoveryUrlsSize > 0)
    {
        UA_Array_delete(config.applicationDescription.discoveryUrls,
                        config.applicationDescription.discoveryUrlsSize,
                        &UA_TYPES[UA_TYPES_STRING]);
    }
    config.applicationDescription.discoveryUrls =
        (UA_String*)UA_malloc(sizeof(UA_String));
    if (config.applicationDescription.discoveryUrls)
    {
        UA_String_copy(&discoveryUrl,
                       &config.applicationDescription.discoveryUrls[0]);
        config.applicationDescription.discoveryUrlsSize = 1;
    }
    UA_String_clear(&discoveryUrl);

    Serial.printf("[OPC UA] Endpoint URL: %s\n", discoveryUrlBuffer);

    server = UA_Server_newWithConfig(&config);
    if (!server)
    {
        Serial.println("[OPC UA] Failed to create server!");
        UA_String_clear(&config.applicationDescription.applicationUri);
        UA_LocalizedText_clear(&config.applicationDescription.applicationName);
        UA_String_clear(&config.customHostname);
        vTaskDelete(NULL);
        return;
    }

    for (size_t i = 0; i < objectCount; ++i)
    {
        addObjectNodes(&objects[i]);
    }

    retval = UA_Server_run_startup(server);
    if (retval != UA_STATUSCODE_GOOD)
    {
        Serial.printf("[OPC UA] Startup error: %s\n", UA_StatusCode_name(retval));
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[OPC UA] Server started successfully.");

    unsigned long lastDiagUpdate = 0;
    unsigned long lastLog = 0;
    unsigned long lastStatusPrint = 0;
    unsigned long lastWifiCheck = 0;

    while (1)
    {
        UA_Server_run_iterate(server, false);

        /* Update computed values (Diag/) every second so that an OPC UA
           client reads up-to-date values even between two logs. */
        if (millis() - lastDiagUpdate >= 1000)
        {
            lastDiagUpdate = millis();
            updateObjects();
        }

        /* Periodic logging of values to the serial console. */
        if (millis() - lastLog >= PUBLISH_INTERVAL_MS)
        {
            lastLog = millis();
            logObjects();
        }

        if (millis() - lastWifiCheck >= 5000)
        {
            lastWifiCheck = millis();
            wifi_check();
        }

        if (millis() - lastStatusPrint >= WIFI_STATUS_INTERVAL_MS)
        {
            lastStatusPrint = millis();
            char ipBuffer[16];
            wifi_get_ip(ipBuffer, sizeof(ipBuffer));
            Serial.printf("[WIFI] Status: %s | IP: %s | RSSI: %d dBm\n",
                          wifi_is_connected() ? "Connected" : "Disconnected",
                          ipBuffer,
                          wifi_get_rssi());
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void opcua_server_start_task(OpcUaObject* objectArray, size_t count)
{
    if (objectArray == NULL || count == 0)
    {
        Serial.println("[OPC UA] No object to expose.");
        return;
    }

    // Copy the descriptors so the caller may use a local array.
    objects = (OpcUaObject*)malloc(sizeof(OpcUaObject) * count);
    if (objects == NULL)
    {
        Serial.println("[OPC UA] Out of memory while copying objects.");
        return;
    }
    memcpy(objects, objectArray, sizeof(OpcUaObject) * count);
    objectCount = count;

    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        opcuaServerTask,
        "OpcUaTask",
        12288,
        NULL,
        1,
        NULL,
        1
    );

    if (taskCreated != pdPASS)
    {
        Serial.println("[OPC UA] Failed to create server task!");
        free(objects);
        objects = NULL;
        objectCount = 0;
    }
}
