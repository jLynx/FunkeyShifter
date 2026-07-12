#include "funkey_protocol.hpp"

namespace funkey::protocol {

static const uint8_t s_capabilities_response[FUNKEY_CAPABILITIES_LEN] = {
    0x46, 0x53, 0x48, 0x31,
    FUNKEY_CAP_VERSION,
    FUNKEY_CAP_MANAGED_CATALOG | FUNKEY_CAP_BLE_CONTROL | FUNKEY_CAP_RAW_PACKET_SET
#if FUNKEY_PHYSICAL_ENABLED
        | FUNKEY_CAP_PHYSICAL_READER
#endif
    ,
    0x00,
    0x00,
};

const uint8_t *capabilitiesResponse()
{
    return s_capabilities_response;
}

size_t capabilitiesResponseLen()
{
    return sizeof(s_capabilities_response);
}

} // namespace funkey::protocol
