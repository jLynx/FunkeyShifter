#pragma once

#include <stdint.h>

#include "funkey_protocol.hpp"

namespace funkey::report {

using NotifyFn = void (*)();

void setNotifyCallbacks(NotifyFn reportNotify, NotifyFn physicalReportNotify);

void copy(uint8_t out[FUNKEY_REPORT_LEN]);
void set(const uint8_t in[FUNKEY_REPORT_LEN]);
void setId(uint32_t id);
void remove();
bool isRemoved(const uint8_t report[FUNKEY_REPORT_LEN]);
uint32_t id(const uint8_t report[FUNKEY_REPORT_LEN]);
void fromId(uint32_t id, uint8_t out[FUNKEY_REPORT_LEN]);

} // namespace funkey::report

namespace funkey::portal {

bool packetFromId(uint32_t id, uint8_t out[FUNKEY_PORTAL_PACKET_LEN]);
void setRawPacket(const uint8_t packet[FUNKEY_PORTAL_PACKET_LEN]);
void copyStablePacket(uint8_t out[FUNKEY_PORTAL_PACKET_LEN]);

} // namespace funkey::portal

#if FUNKEY_PHYSICAL_ENABLED
namespace funkey::physical_report {

void copy(uint8_t out[FUNKEY_REPORT_LEN]);
void set(const uint8_t in[FUNKEY_REPORT_LEN]);
void setId(uint32_t id);
void remove();
void contactIssue();
bool isContactIssue(const uint8_t report[FUNKEY_REPORT_LEN]);

} // namespace funkey::physical_report
#endif
