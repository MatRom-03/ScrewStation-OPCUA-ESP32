/**
 * @file opcua_object.h
 * @brief Generic OPC UA object model.
 *
 * An OpcUaObject is a named collection of OpcUaProperty. Each property is
 * exposed as a readable (and optionally writable) OPC UA variable. Devices
 * (sensor, button, motor, ...) only have to describe themselves with this
 * model; the OPC UA server takes care of creating the nodes and syncing the
 * values with the local variables.
 */

#ifndef OPCUA_OBJECT_H
#define OPCUA_OBJECT_H

#include <stddef.h>
#include <stdint.h>

/** Supported OPC UA data types for a property. */
typedef enum
{
    OPCUA_TYPE_BOOL,   /**< Boolean, backed by a C `bool`. */
    OPCUA_TYPE_INT32,  /**< 32-bit signed integer, backed by `int32_t`. */
    OPCUA_TYPE_UINT32, /**< 32-bit unsigned integer, backed by `uint32_t`. */
    OPCUA_TYPE_FLOAT,  /**< 32-bit float, backed by `float`. */
    OPCUA_TYPE_DOUBLE, /**< 64-bit float, backed by `double`. */
    OPCUA_TYPE_STRING  /**< UTF-8 string, backed by a NUL-terminated `char[]`. */
} OpcUaValueType;

/** Client access level of a property. */
typedef enum
{
    OPCUA_ACCESS_READ,     /**< Client can only read the value. */
    OPCUA_ACCESS_READWRITE /**< Client can read and write the value. */
} OpcUaAccess;

/**
 * @brief A single property (OPC UA variable) of an object.
 *
 * `value` must point to a long-lived variable whose C type matches `type`:
 *   - OPCUA_TYPE_BOOL   -> bool
 *   - OPCUA_TYPE_INT32  -> int32_t
 *   - OPCUA_TYPE_UINT32 -> uint32_t
 *   - OPCUA_TYPE_FLOAT  -> float
 *   - OPCUA_TYPE_DOUBLE -> double
 *   - OPCUA_TYPE_STRING -> char[] (NUL-terminated)
 */
typedef struct
{
    const char*    name;   /**< Browse and display name. */
    uint32_t       nodeId; /**< Numeric node id in namespace 1 (0 = automatic). */
    OpcUaValueType type;   /**< Data type. */
    OpcUaAccess    access; /**< Read-only or read/write. */
    void*          value;  /**< Pointer to the live value storage. */
    const char*    folder; /**< Optional parent folder name (NULL = directly under the object). */
    size_t         size;   /**< Buffer size in bytes for STRING values (0 otherwise). */
} OpcUaProperty;

/** @brief Optional callback used to refresh the property values periodically. */
typedef void (*OpcUaUpdateCallback)(void);

/** @brief Optional lock callback called before reading/writing a property value. */
typedef void (*OpcUaLockCallback)(void* context);

/** @brief Optional unlock callback called after reading/writing a property value. */
typedef void (*OpcUaUnlockCallback)(void* context);

/** @brief A generic OPC UA object made of a name and an array of properties. */
typedef struct
{
    const char*        name;          /**< Object browse and display name. */
    uint32_t           nodeId;        /**< Object numeric node id in namespace 1 (0 = automatic). */
    OpcUaProperty*     properties;    /**< Array of properties. */
    size_t             propertyCount; /**< Number of properties. */
    OpcUaUpdateCallback onUpdate;     /**< Optional refresh callback (may be NULL). */
    OpcUaLockCallback   onLock;       /**< Optional lock callback before accessing a property (may be NULL). */
    OpcUaUnlockCallback onUnlock;     /**< Optional unlock callback after accessing a property (may be NULL). */
    void*               lockContext;  /**< Context stepsed to onLock/onUnlock. */
} OpcUaObject;

#endif
